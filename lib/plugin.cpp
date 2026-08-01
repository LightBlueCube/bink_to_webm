#include "plugin_interfaces.h"
#include "plugin_logging.h"
#include "hook.h"
#include "webm_hooks.h"
#include "playvideoex.h"
#include "webm_audio.h"
#include "webm_renderer.h"
#include "webm_assets.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <tlhelp32.h>


class PluginId : public IPluginId
{
public:
	const char* GetString(PluginString property) override
	{
		switch(property)
		{
		case PluginString::LOG_NAME:
			return "BIKTOWEBM";
		case PluginString::NAME:
		case PluginString::DEPENDENCY_NAME:
			return "bik_to_webm";
		}

		return nullptr;
	}

	int64_t GetField(PluginField property) override
	{
		switch(property)
		{
		case PluginField::CONTEXT:
			return PluginContext::CLIENT;

		// #00ffa0
		case PluginField::COLOR:
			return 0xa0ff00;
		}

		return 0;
	}
};

static std::atomic_uint32_t HookStatus = 0b0;
static std::atomic_bool HasError = false;
static std::atomic_bool HookEnabled = false;
static void Successed(size_t id)
{
	HookStatus |= 1u << id;
}
static void ErrorOccurred()
{
	HasError = true;
	PluginLogError("initialization aborted! (an error occurred)");
}

class PluginCallbacks : public IPluginCallbacks
{
public:
	void Init(HMODULE northstar, const PluginNorthstarData* initData, bool reloaded) override
	{
		if((HasError = !InitializePluginLogging(northstar, initData)))
			return;

		PluginLogInfo("bik_to_webm initialized: reloaded={}", reloaded);

		if(!HookUtils::InitMinHook())
		{
			PluginLogError("faild to initialize MinHook");
			ErrorOccurred();
			return;
		}

		WebmAssets::Scan(std::filesystem::path("R2Northstar") / "media.webm");
		ReplayLoadedLibraries();
	}

	void Finalize() override {}

	bool Unload() override
	{
		FlushPluginLogs();

		PluginLogInfo("bik_to_webm unloading");


		if(!PrepareRemoveWebmHooks())
			return false;

		if(!HookUtils::UninitMinHook())
		{
			PluginLogError("failed to uninitialize MinHook");
			return false;
		}
		WebmRenderer::Shutdown();
		RemovePlayVideoEXCommand();
		WebmAssets::Clear();

		return true;
	}

	void OnSqvmCreated(void* /*sqvm*/) override {}

	void OnSqvmDestroying(void* /*sqvm*/) override {}

	void OnLibraryLoaded(HMODULE module, const char* name) override
	{
		if(HasError)
			return;

		if(_stricmp(name, "engine.dll") == 0)
		{
			RegisterWebmHooks_engine(module) ? Successed(0) : ErrorOccurred();
		}
		else if(_stricmp(name, "client.dll") == 0)
		{
			SetupWebmAudio_client(module);
			RegisterWebmHooks_client(module) ? Successed(1) : ErrorOccurred();
			InstallPlayVideoEXCommand(module) ? Successed(2) : ErrorOccurred();
		}
		else if(_stricmp(name, "mileswin64.dll") == 0)
		{
			SetupWebmAudio_mileswin64(module) ? Successed(3) : ErrorOccurred();
		}
		else if(_stricmp(name, "bink2w64.dll") == 0)
		{
			RegisterWebmHooks_bink2w64(module) ? Successed(4) : ErrorOccurred();
		}
		else if(_stricmp(name, "materialsystem_dx11.dll") == 0)
		{
			WebmRenderer::RegisterHooks(module) ? Successed(5) : ErrorOccurred();
		}
	}

	void RunFrame() override
	{
		FlushPluginLogs();

		if(HookEnabled || HasError)
			return;

		if(!(HookStatus & 1u << 6))
		{
			if(WebmRenderer::Ready())
				Successed(6);
			return;
		}

		if(HookStatus == (1u << 7) - 1)
			HasError = !(HookEnabled = EnableWebmHooks());
	}

private:
	void ReplayLoadedLibraries()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
		if(snapshot == INVALID_HANDLE_VALUE)
		{
			PluginLogWarn("Could not enumerate loaded modules: {}", GetLastError());
			return;
		}

		MODULEENTRY32 moduleEntry = {};
		moduleEntry.dwSize = sizeof(moduleEntry);
		bool status = Module32First(snapshot, &moduleEntry);
		while(status)
		{
			OnLibraryLoaded(moduleEntry.hModule, moduleEntry.szModule);
			status = Module32Next(snapshot, &moduleEntry);
		}

		CloseHandle(snapshot);
	}
};


static PluginId PluginId;
static PluginCallbacks PluginCallbacks;
extern "C" __declspec(dllexport) void* CreateInterface(const char* name, InterfaceStatus* returnCode)
{
	if(!name)
		goto failed;

	if(strcmp(name, PLUGIN_ID_VERSION) == 0)
	{
		if(returnCode)
			*returnCode = IFACE_OK;
		return &PluginId;
	}

	if(strcmp(name, "PluginCallbacks001") == 0)
	{
		if(returnCode)
			*returnCode = IFACE_OK;
		return &PluginCallbacks;
	}

failed:
	if(returnCode)
		*returnCode = IFACE_FAILED;
	return nullptr;
}
