#include "webm_renderer.h"
#include "lib/hook.h"
#include "lib/plugin_logging.h"

// provided via webm_shader.hlsl
#include "webm_shader.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3d11.h>
#include <memory>
#include <minwindef.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

template<typename T>
void Release(T*& object)
{
	if(object)
		object->Release();
	object = nullptr;
}

struct AlphaTextureEntry
{
	~AlphaTextureEntry()
	{
		Release(view);
		Release(texture);
	}

	std::mutex mutex;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels;
	bool isDirty = true;
	ID3D11Texture2D* texture = nullptr;
	ID3D11ShaderResourceView* view = nullptr;
};

struct BlendStateSnapshot
{
	ID3D11BlendState* state = nullptr;
	FLOAT factor[4] = {};
	UINT sampleMask = 0xffffffff;
};

// clang-format off
using PSSetShaderResourcesFn 	= void 			(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
																	 ID3D11ShaderResourceView* const* shaderResourceViews);
using PSSetShaderFn 			= void 			(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
																	 ID3D11ClassInstance* const* classInstances, UINT classInstanceCount);
using CreateInterfaceFn 		= void* 		(*)					(const char* name, int* returnCode);
using GetCurrentShaderHandleFn 	= uint16_t 		(*)					(int shaderType);
using GetShaderNameFn 			= const char* 	(*)					(int shaderType, uint16_t shaderHandle);
using GetRenderContextFn 		= void* 		(*)					();
using ReleaseRenderContextFn 	= int 			(__fastcall*)		(void* renderContext);
using DrawFn 					= void 			(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation);
using DrawIndexedFn 			= void 			(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation);
PSSetShaderResourcesFn 		PSSetShaderResources 		= nullptr;
PSSetShaderFn 				PSSetShader 				= nullptr;
GetCurrentShaderHandleFn 	GetCurrentShaderHandle 		= nullptr;
GetShaderNameFn 			GetShaderName 				= nullptr;
GetRenderContextFn 			GetRenderContext 			= nullptr;
ReleaseRenderContextFn 		ReleaseRenderContext 		= nullptr;
DrawFn 						Draw 						= nullptr;
DrawIndexedFn 				DrawIndexed 				= nullptr;
PFN_D3D11_CREATE_DEVICE 	Orig_D3D11CreateDevice 		= nullptr;

constexpr size_t DC_VTABLE_PSSetShaderResources 	= 8;
constexpr size_t DC_VTABLE_PSSetShader 				= 9;
constexpr size_t DC_VTABLE_DrawIndexed 				= 12;
constexpr size_t DC_VTABLE_Draw 					= 13;

constexpr uintptr_t MATERIAL_SYSTEM_DEVICE_RVA 					= 0x14e8dd0;
constexpr uintptr_t MATERIAL_SYSTEM_DEVICE_CONTEXT_RVA 			= 0x14e8dd8;
constexpr uintptr_t MATERIAL_SYSTEM_CURRENT_PIXEL_SHADER_RVA 	= 0x19c3840;
constexpr uintptr_t GET_CURRENT_SHADER_HANDLE_RVA 				= 0x33930;
constexpr uintptr_t GET_SHADER_NAME_RVA 						= 0x31c90;
constexpr uintptr_t GET_RENDER_CONTEXT_RVA 						= 0x603d0;
constexpr uintptr_t RELEASE_RENDER_CONTEXT_RVA 					= 0x65160;
// clang-format on

constexpr int PIXEL_SHADER_TYPE = 0;
// CMatRenderContext::Bind stores IMaterial* at +0x18 (see materialsystem_dx11.dll + 0x180071f40)
constexpr size_t RENDER_CONTEXT_CURRENT_MATERIAL_OFFSET = 0x18;

HMODULE MaterialSystem = nullptr;
void* MaterialSystemInterface = nullptr;
ID3D11Device** MaterialSystemDevice = nullptr;
ID3D11PixelShader** MaterialSystemCurrentPixelShader = nullptr;
std::atomic_bool ReplacementShaderActive = false;
std::unordered_map<void*, std::shared_ptr<AlphaTextureEntry>> AlphaTextures;
ID3D11PixelShader* LastBinkShader = nullptr;

ID3D11PixelShader* WebmPixelShader = nullptr;
ID3D11BlendState* WebmBlendState = nullptr;
ID3D11ShaderResourceView* OpaqueAlphaView = nullptr;
ID3D11SamplerState* AlphaSampler = nullptr;

std::mutex RendererMutex;

bool WebmRenderInstalled = false;

bool EnsureAlphaTexture(ID3D11DeviceContext* context, AlphaTextureEntry* alpha)
{
	if(alpha->view)
		return true;

	ID3D11Device* device = nullptr;
	context->GetDevice(&device);

	D3D11_TEXTURE2D_DESC description = {};
	description.Width = alpha->width;
	description.Height = alpha->height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_R8_UNORM;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DYNAMIC;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT result = device->CreateTexture2D(&description, nullptr, &alpha->texture);
	if(SUCCEEDED(result))
		result = device->CreateShaderResourceView(alpha->texture, nullptr, &alpha->view);
	Release(device);

	if(FAILED(result))
		goto cleanup;
	return true;

cleanup:
	Release(alpha->view);
	Release(alpha->texture);
	return false;
}

ID3D11ShaderResourceView* UploadAlphaTexture(ID3D11DeviceContext* context, AlphaTextureEntry* alpha)
{
	std::lock_guard lock(alpha->mutex);
	if(!EnsureAlphaTexture(context, alpha))
		return nullptr;

	if(alpha->isDirty)
	{
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if(FAILED(context->Map(alpha->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return nullptr;

		for(size_t row = 0; row < alpha->height; ++row)
			memcpy((uint8_t*)mapped.pData + row * mapped.RowPitch, alpha->pixels.data() + row * alpha->width, alpha->width);
		context->Unmap(alpha->texture, 0);

		alpha->isDirty = false;
	}

	alpha->view->AddRef();
	return alpha->view;
}

bool IsVideoPixelShader(ID3D11PixelShader* shader)
{
	if(shader == WebmPixelShader)
		return true;

	if(shader != *MaterialSystemCurrentPixelShader)
		return false;

	const uint16_t handle = GetCurrentShaderHandle(PIXEL_SHADER_TYPE);
	constexpr uint16_t INVALID_HEADER = 0xffff;
	if(handle >= INVALID_HEADER)
		return false;

	const char* shaderName = GetShaderName(PIXEL_SHADER_TYPE, handle);
	if(!shaderName)
		return false;
	if(strcmp(shaderName, "bik_ps40") != 0)
		return false;

	return true;
}

std::shared_ptr<AlphaTextureEntry> FindAlphaTexture(void* token)
{
	std::lock_guard lock(RendererMutex);
	auto it = AlphaTextures.find(token);
	if(it == AlphaTextures.end())
		return {};
	return it->second;
}

std::shared_ptr<AlphaTextureEntry> FindCurrentAlphaTexture()
{
	void* renderContext = GetRenderContext();
	if(!renderContext)
		return {};

	void* material = *(void**)((uint8_t*)renderContext + RENDER_CONTEXT_CURRENT_MATERIAL_OFFSET);
	std::shared_ptr<AlphaTextureEntry> alpha = FindAlphaTexture(material);
	ReleaseRenderContext(renderContext);
	return alpha;
}

void STDMETHODCALLTYPE Hook_PSSetShaderResources(ID3D11DeviceContext* context, UINT startSlot, UINT numViews, ID3D11ShaderResourceView* const* shaderResourceViews)
{
	PSSetShaderResources(context, startSlot, numViews, shaderResourceViews);
	if(startSlot != 0 || !shaderResourceViews || numViews < 3)
		return;

	ID3D11PixelShader* shader = nullptr;
	context->PSGetShader(&shader, nullptr, nullptr);
	const bool isVideoShader = IsVideoPixelShader(shader);
	Release(shader);
	if(!isVideoShader)
		return;

	std::shared_ptr<AlphaTextureEntry> alphaTexture = FindCurrentAlphaTexture();
	ID3D11ShaderResourceView* alphaView = alphaTexture ? UploadAlphaTexture(context, alphaTexture.get()) : nullptr;
	if(!alphaView)
		(alphaView = OpaqueAlphaView)->AddRef();
	PSSetShaderResources(context, 3, 1, &alphaView);
	Release(alphaView);
	context->PSSetSamplers(3, 1, &AlphaSampler);
}

void STDMETHODCALLTYPE Hook_PSSetShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader, ID3D11ClassInstance* const* classInstances, UINT classInstanceCount)
{
	if(!(ReplacementShaderActive = IsVideoPixelShader(shader)))
		return PSSetShader(context, shader, classInstances, classInstanceCount);

	{
		std::lock_guard lock(RendererMutex);
		LastBinkShader = shader;
	}
	return PSSetShader(context, WebmPixelShader, nullptr, 0);
}

void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation)
{
	if(!ReplacementShaderActive.load())
		return Draw(context, vertexCount, startVertexLocation);

	BlendStateSnapshot previous;
	context->OMGetBlendState(&previous.state, previous.factor, &previous.sampleMask);
	context->OMSetBlendState(WebmBlendState, nullptr, 0xffffffff);
	Draw(context, vertexCount, startVertexLocation);
	context->OMSetBlendState(previous.state, previous.factor, previous.sampleMask);
	Release(previous.state);
}

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* context, UINT indexCount, UINT startIndexLocation, INT baseVertexLocation)
{
	if(!ReplacementShaderActive.load())
		return DrawIndexed(context, indexCount, startIndexLocation, baseVertexLocation);

	BlendStateSnapshot previous;
	context->OMGetBlendState(&previous.state, previous.factor, &previous.sampleMask);
	context->OMSetBlendState(WebmBlendState, nullptr, 0xffffffff);
	DrawIndexed(context, indexCount, startIndexLocation, baseVertexLocation);
	context->OMSetBlendState(previous.state, previous.factor, previous.sampleMask);
	Release(previous.state);
}

bool CreateRenderer(ID3D11Device* device)
{
	ID3D11Texture2D* opaqueTexture = nullptr;

	HRESULT result = device->CreatePixelShader(PSMain, sizeof(PSMain), nullptr, &WebmPixelShader);
	if(FAILED(result))
	{
		PluginLogError("Failed to create WebM pixel shader: HRESULT={:#x}", static_cast<uint32_t>(result));
		goto cleanup;
	}

	{
		D3D11_TEXTURE2D_DESC textureDesc = {};
		textureDesc.Width = 1;
		textureDesc.Height = 1;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = DXGI_FORMAT_R8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
		textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA data = {};
		uint8_t opaque = 0xff;
		data.pSysMem = &opaque;
		data.SysMemPitch = 1;

		result = device->CreateTexture2D(&textureDesc, &data, &opaqueTexture);
		if(FAILED(result))
		{
			PluginLogError("Failed to create opaque alpha texture: HRESULT={:#x}", static_cast<uint32_t>(result));
			goto cleanup;
		}
		result = device->CreateShaderResourceView(opaqueTexture, nullptr, &OpaqueAlphaView);
		Release(opaqueTexture);
		if(FAILED(result))
		{
			PluginLogError("Failed to create opaque alpha shader resource view: HRESULT={:#x}", static_cast<uint32_t>(result));
			goto cleanup;
		}

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MinLOD = 0.0f;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		result = device->CreateSamplerState(&samplerDesc, &AlphaSampler);
		if(FAILED(result))
		{
			PluginLogError("Failed to create alpha sampler: HRESULT={:#x}", static_cast<uint32_t>(result));
			goto cleanup;
		}

		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].BlendEnable = TRUE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		result = device->CreateBlendState(&blendDesc, &WebmBlendState);
		if(FAILED(result))
		{
			PluginLogError("Failed to create webm blend state: HRESULT={:#x}", static_cast<uint32_t>(result));
			goto cleanup;
		}

		return true;
	}

cleanup:
	Release(opaqueTexture);
	Release(WebmPixelShader);
	Release(WebmBlendState);
	Release(OpaqueAlphaView);
	Release(AlphaSampler);

	return false;
}

bool CreateRenderHooks(ID3D11Device* device)
{
	ID3D11DeviceContext* context = nullptr;
	device->GetImmediateContext(&context);

	// clang-format off
	const uintptr_t rva_PSSetShaderResources = 	(uintptr_t)HookUtils::VTableMethod((void***)context, DC_VTABLE_PSSetShaderResources) 	- (uintptr_t)MaterialSystem;
	const uintptr_t rva_PSSetShader = 			(uintptr_t)HookUtils::VTableMethod((void***)context, DC_VTABLE_PSSetShader) 			- (uintptr_t)MaterialSystem;
	const uintptr_t rva_DrawIndexed = 			(uintptr_t)HookUtils::VTableMethod((void***)context, DC_VTABLE_DrawIndexed) 			- (uintptr_t)MaterialSystem;
	const uintptr_t rva_Draw = 					(uintptr_t)HookUtils::VTableMethod((void***)context, DC_VTABLE_Draw) 					- (uintptr_t)MaterialSystem;
	const bool success =
		   HookUtils::HookExport(MaterialSystem, rva_PSSetShaderResources, 	(void*)Hook_PSSetShaderResources, 	(void**)&PSSetShaderResources)
		&& HookUtils::HookExport(MaterialSystem, rva_PSSetShader, 			(void*)Hook_PSSetShader, 			(void**)&PSSetShader)
		&& HookUtils::HookExport(MaterialSystem, rva_DrawIndexed, 			(void*)Hook_DrawIndexed, 			(void**)&DrawIndexed)
		&& HookUtils::HookExport(MaterialSystem, rva_Draw, 					(void*)Hook_Draw, 					(void**)&Draw)
		;
	// clang-format on

	Release(context);
	return success;
}

bool InitializeRenderer(ID3D11Device* device)
{
	if(!CreateRenderer(device))
		return false;

	if(!CreateRenderHooks(device))
	{
		Release(WebmPixelShader);
		Release(WebmBlendState);
		Release(OpaqueAlphaView);
		Release(AlphaSampler);
		return false;
	}

	PluginLogInfo("Successfully registered video render hooks");
	WebmRenderInstalled = true;
	return true;
}

HRESULT WINAPI Hook_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
									  const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion,
									  ID3D11Device** device, D3D_FEATURE_LEVEL* selectedFeatureLevel, ID3D11DeviceContext** immediateContext)
{
	HRESULT result = Orig_D3D11CreateDevice(adapter, driverType, software, flags, featureLevels, featureLevelCount, sdkVersion, device,
											selectedFeatureLevel, immediateContext);

	if(device != MaterialSystemDevice)
		return result;
	if(FAILED(result))
		return result;

	if(!InitializeRenderer(*device))
		PluginLogError("Failed to initialize WebM renderer after D3D11CreateDevice");

	return result;
}

} // namespace



namespace WebmRenderer
{

[[nodiscard]] bool PrepareAlphaTexture(void* token, uint32_t width, uint32_t height)
{
	if(width == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height == 0 || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
	{
		PluginLogError("Invalid WebM alpha texture dimensions: {}x{}", width, height);
		return false;
	}

	auto alpha = std::make_shared<AlphaTextureEntry>();
	alpha->width = width;
	alpha->height = height;
	alpha->pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff);

	std::lock_guard lock(RendererMutex);
	AlphaTextures[token] = std::move(alpha);
	return true;
}

void ReleaseAlphaTexture(void* token)
{
	std::shared_ptr<AlphaTextureEntry> alpha;
	{
		std::lock_guard lock(RendererMutex);
		auto it = AlphaTextures.find(token);
		if(it == AlphaTextures.end())
			return;

		alpha = std::move(it->second);
		AlphaTextures.erase(it);
	}
}

void UpdateAlphaTexture(void* token, const uint8_t* data, uint32_t pitch)
{
	std::shared_ptr<AlphaTextureEntry> alpha = FindAlphaTexture(token);
	if(!alpha)
		return;

	std::lock_guard lock(alpha->mutex);
	for(size_t row = 0; row < alpha->height; ++row)
		memcpy(alpha->pixels.data() + row * alpha->width, data + row * pitch, alpha->width);
	alpha->isDirty = true;
}

[[nodiscard]] bool Ready()
{
	return WebmRenderInstalled;
}

[[nodiscard]] bool RegisterHooks(HMODULE materialsystem)
{
	MaterialSystem = materialsystem;

	CreateInterfaceFn createInterface = nullptr;
	if(!HookUtils::ModuleExport(MaterialSystem, "CreateInterface", reinterpret_cast<void**>(&createInterface)))
		return false;

	int returnCode;
	MaterialSystemInterface = createInterface("VMaterialSystem083", &returnCode);
	if(returnCode != IFACE_OK || !MaterialSystemInterface)
	{
		PluginLogError("materialsystem_dx11.dll does not provide interface VMaterialSystem083: return_code={}", returnCode);
		return false;
	}

	// clang-format off
	HookUtils::ModuleExport(MaterialSystem, MATERIAL_SYSTEM_DEVICE_RVA, 				(void**)&MaterialSystemDevice);
	HookUtils::ModuleExport(MaterialSystem, MATERIAL_SYSTEM_CURRENT_PIXEL_SHADER_RVA, 	(void**)&MaterialSystemCurrentPixelShader);
	HookUtils::ModuleExport(MaterialSystem, GET_CURRENT_SHADER_HANDLE_RVA, 				(void**)&GetCurrentShaderHandle);
	HookUtils::ModuleExport(MaterialSystem, GET_SHADER_NAME_RVA, 						(void**)&GetShaderName);
	HookUtils::ModuleExport(MaterialSystem, GET_RENDER_CONTEXT_RVA, 					(void**)&GetRenderContext);
	HookUtils::ModuleExport(MaterialSystem, RELEASE_RENDER_CONTEXT_RVA, 				(void**)&ReleaseRenderContext);
	// clang-format on

	if(*MaterialSystemDevice)
		return InitializeRenderer(*MaterialSystemDevice);

	HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
	if(!d3d11)
	{
		PluginLogError("d3d11.dll is not loaded while registering WebM renderer hooks");
		return false;
	}
	void* target = nullptr;
	if(!HookUtils::ModuleExport(d3d11, "D3D11CreateDevice", &target))
		return false;
	if(!HookUtils::HookExport(d3d11, (uintptr_t)target - (uintptr_t)d3d11, (void*)Hook_D3D11CreateDevice, (void**)&Orig_D3D11CreateDevice))
		return false;
	if(!HookUtils::EnableMinHook(target))
		return false;

	return true;
}

void Shutdown()
{
	ID3D11DeviceContext* context;
	(*MaterialSystemDevice)->GetImmediateContext(&context);

	ID3D11PixelShader* currentShader = nullptr;
	context->PSGetShader(&currentShader, nullptr, nullptr);
	if(currentShader == WebmPixelShader)
	{
		ID3D11PixelShader* nativeBinkShader = nullptr;
		{
			std::lock_guard lock(RendererMutex);
			(nativeBinkShader = LastBinkShader)->AddRef();
		}
		context->PSSetShader(nativeBinkShader, nullptr, 0);
		Release(nativeBinkShader);

		ID3D11ShaderResourceView* nullView = nullptr;
		context->PSSetShaderResources(3, 1, &nullView);
		ID3D11SamplerState* nullSampler = nullptr;
		context->PSSetSamplers(3, 1, &nullSampler);
	}
	Release(currentShader);
	Release(context);

	std::unordered_map<void*, std::shared_ptr<AlphaTextureEntry>> alphaTextures;
	{
		std::lock_guard lock(RendererMutex);
		alphaTextures.swap(AlphaTextures);
		LastBinkShader = nullptr;
		ReplacementShaderActive = false;
	}
	alphaTextures.clear();

	Release(WebmPixelShader);
	Release(WebmBlendState);
	Release(OpaqueAlphaView);
	Release(AlphaSampler);
}

} // namespace WebmRenderer
