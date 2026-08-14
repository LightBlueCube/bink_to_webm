#include "webm_hooks.h"
#include "lib/plugin_logging.h"
#include "lib/hook.h"
#include "webm_assets.h"
#include "webm_renderer.h"
#include "webm_audio.h"
#include "webm_decoder.h"

#include <MinHook.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <windows.h>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

namespace
{

struct BinkPlane
{
	uint8_t* buffer = nullptr;
	uint32_t allocate = 0;
	uint32_t pitch = 0;
};
// clang-format off
static_assert(offsetof(BinkPlane, buffer) 		== 0x00);
static_assert(offsetof(BinkPlane, allocate) 	== 0x08);
static_assert(offsetof(BinkPlane, pitch) 		== 0x0c);
static_assert(	sizeof(BinkPlane) 				== 0x10);
// clang-format on

struct BinkFramePlaneSet
{
	BinkPlane y;
	BinkPlane cr;
	BinkPlane cb;
	BinkPlane a;
};
// clang-format off
static_assert(offsetof(BinkFramePlaneSet, y) 	== 0x00);
static_assert(offsetof(BinkFramePlaneSet, cr) 	== 0x10);
static_assert(offsetof(BinkFramePlaneSet, cb) 	== 0x20);
static_assert(offsetof(BinkFramePlaneSet, a) 	== 0x30);
static_assert(	sizeof(BinkFramePlaneSet) 		== 0x40);
// clang-format on

struct BinkFrameBuffers
{
	uint32_t totalFrames = 0;
	uint32_t yWidth = 0;
	uint32_t yHeight = 0;
	uint32_t chromaWidth = 0;
	uint32_t chromaHeight = 0;
	uint32_t frameNum = 0;
	BinkFramePlaneSet frames[2];
};
// clang-format off
static_assert(offsetof(BinkFrameBuffers, totalFrames) 	== 0x00);
static_assert(offsetof(BinkFrameBuffers, yWidth) 		== 0x04);
static_assert(offsetof(BinkFrameBuffers, yHeight) 		== 0x08);
static_assert(offsetof(BinkFrameBuffers, chromaWidth) 	== 0x0c);
static_assert(offsetof(BinkFrameBuffers, chromaHeight) 	== 0x10);
static_assert(offsetof(BinkFrameBuffers, frameNum) 		== 0x14);
static_assert(offsetof(BinkFrameBuffers, frames) 		== 0x18);
static_assert(	sizeof(BinkFrameBuffers) 				== 0x98);
// clang-format on

struct WebmBinkHandle
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t frameCount = 0;
	uint32_t frameNumber = 1;
	uint32_t lastFrameNumber = UINT32_MAX;
	uint32_t frameRate = 30;
	uint32_t frameRateDenom = 1;
	uint32_t readError = 0;
	uint32_t openFlags = 0;
	uint32_t reserved[5] = {};
	uint32_t trackCount = 0;

	WebmDecoder decoder;
	WebmAudioPlayer audioPlayer;
	BinkFrameBuffers* frameBuffers = nullptr;
	// borrowed from CBik. renderer use this as an identity key; never dereference it.
	void* material = nullptr;
	std::vector<uint8_t> alphaPlane;
	uint32_t alphaPitch = 0;
	bool paused = false;
	bool decodedCurrentFrame = false;
	bool warnedColorSpace = false;
	bool warnedColorRange = false;
	std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point pauseTime;
	std::mutex mutex;
};
// clang-format off
static_assert(offsetof(WebmBinkHandle, width) 				== 0x00);
static_assert(offsetof(WebmBinkHandle, height) 				== 0x04);
static_assert(offsetof(WebmBinkHandle, frameCount) 			== 0x08);
static_assert(offsetof(WebmBinkHandle, frameNumber) 		== 0x0c);
static_assert(offsetof(WebmBinkHandle, lastFrameNumber) 	== 0x10);
static_assert(offsetof(WebmBinkHandle, frameRate) 			== 0x14);
static_assert(offsetof(WebmBinkHandle, frameRateDenom) 		== 0x18);
static_assert(offsetof(WebmBinkHandle, readError) 			== 0x1c);
static_assert(offsetof(WebmBinkHandle, openFlags) 			== 0x20);
static_assert(offsetof(WebmBinkHandle, reserved) 			== 0x24);
static_assert(offsetof(WebmBinkHandle, trackCount) 			== 0x38);
// clang-format on

// clang-format off
using CreateInterfaceFn 			= void* 	(*)			 (const char* name, int* returnCode);
using CreateBikMaterialFn 			= uint16_t 	(__fastcall*)(void* bik, const char* materialName, const char* fileName, const char* pathId, uint32_t flags);
using GetBikMaterialFn 				= void* 	(__fastcall*)(void* bik, uint16_t handle);
using BinkOpenFn 					= void* 	(__fastcall*)(const char* fileName, uint32_t flags);
using BinkCloseFn 					= void 		(__fastcall*)(void* bink);
using BinkHandleFn 					= uint64_t 	(__fastcall*)(void* bink);
using BinkNextFrameFn 				= void 		(__fastcall*)(void* bink);
using BinkPauseFn 					= int 		(__fastcall*)(void* bink, int pause);
using BinkGotoFn 					= void 		(__fastcall*)(void* bink, uint32_t frame, uint32_t flags);
using BinkFrameBuffersFn 			= void 		(__fastcall*)(void* bink, void* buffers);
using BinkRealtimeFn 				= void 		(__fastcall*)(void* bink, void* realtime, uint32_t frameCount);
using BinkSoundOnOffFn 				= uint64_t 	(__fastcall*)(void* bink, int enabled);
using BinkSpeakerVolumesFn 			= void 		(__fastcall*)(void* bink, int track, const int* speakers, const int* volumes, uint32_t count);
using BinkGetTrackIdFn 				= uint32_t 	(__fastcall*)(void* bink, uint32_t track);
using BinkSetVolumeFn 				= void 		(__fastcall*)(void* bink, int track, int volume);
using VideoPanelBeginPlaybackFn 	= bool 		(__fastcall*)(void* panel, const char* fileName);
CreateBikMaterialFn 		CreateBikMaterial 			= nullptr;
GetBikMaterialFn 			GetBikMaterial 				= nullptr;
BinkOpenFn 					BinkOpen 					= nullptr;
BinkCloseFn 				BinkClose 					= nullptr;
BinkHandleFn 				BinkWait 					= nullptr;
BinkHandleFn 				BinkDoFrame 				= nullptr;
BinkHandleFn 				BinkShouldSkip 				= nullptr;
BinkNextFrameFn 			BinkNextFrame 				= nullptr;
BinkPauseFn 				BinkPause 					= nullptr;
BinkGotoFn 					BinkGoto 					= nullptr;
BinkFrameBuffersFn 			BinkGetFrameBuffersInfo 	= nullptr;
BinkFrameBuffersFn 			BinkRegisterFrameBuffers 	= nullptr;
BinkRealtimeFn 				BinkGetRealtime 			= nullptr;
BinkSoundOnOffFn 			BinkSetSoundOnOff 			= nullptr;
BinkSpeakerVolumesFn 		BinkSetSpeakerVolumes 		= nullptr;
BinkGetTrackIdFn 			BinkGetTrackId 				= nullptr;
BinkSetVolumeFn 			BinkSetVolume 				= nullptr;
VideoPanelBeginPlaybackFn 	VideoPanelBeginPlayback 	= nullptr;
// clang-format on

constexpr uintptr_t CLIENT_VIDEO_PANEL_BEGIN_PLAYBACK_RVA = 0x356130;
constexpr uintptr_t CLIENT_STOP_ALL_VIDEOS_RVA = 0x3579e0;
constexpr uintptr_t VIDEO_PANEL_BLACK_BACKGROUND_OFFSET = 0x316;

std::unordered_set<WebmBinkHandle*> WebmHandles;
std::mutex WebmHandlesMutex;

thread_local std::filesystem::path PendingWebmPath;
thread_local WebmBinkHandle* OpenedWebmHandle = nullptr;

// VBik001 slot 12 resolves a Bink material handle to the IMaterial* stored at the start of its CBik instance.
constexpr size_t BIK_VTABLE_GET_MATERIAL = 12;
constexpr size_t BIK_VTABLE_CREATE_BIK_MATERIAL = 8;



bool TryAcquire(void* handle)
{
	std::lock_guard lock(WebmHandlesMutex);
	return WebmHandles.contains(static_cast<WebmBinkHandle*>(handle));
}
bool TryAcquire(void* handle, std::unique_lock<std::mutex>& handleLock)
{
	std::lock_guard lock(WebmHandlesMutex);
	if(!WebmHandles.contains(static_cast<WebmBinkHandle*>(handle)))
		return false;

	handleLock = std::unique_lock<std::mutex>(static_cast<WebmBinkHandle*>(handle)->mutex);
	return true;
}
bool TryAcquire(void* bink, WebmBinkHandle*& handle, std::unique_lock<std::mutex>& handleLock)
{
	std::lock_guard lock(WebmHandlesMutex);
	if(!WebmHandles.contains(static_cast<WebmBinkHandle*>(bink)))
		return false;

	handle = static_cast<WebmBinkHandle*>(bink);
	handleLock = std::unique_lock<std::mutex>(handle->mutex);
	return true;
}

double GetFrameDuration(const WebmBinkHandle* handle)
{
	return static_cast<double>(handle->frameRateDenom) / handle->frameRate;
}

double GetPlaybackTime(const WebmBinkHandle* handle)
{
	auto now = handle->paused ? handle->pauseTime : std::chrono::steady_clock::now();
	return std::chrono::duration<double>(now - handle->startTime).count();
}

void CopyPlane(uint8_t* destination, uint32_t destinationPitch, const uint8_t* source, int sourcePitch, uint32_t width, uint32_t height)
{
	for(uint32_t row = 0; row < height; ++row)
		memcpy(destination + static_cast<size_t>(row) * destinationPitch, source + static_cast<ptrdiff_t>(row) * sourcePitch, width);
}

void SubmitDecodedAudio(WebmBinkHandle* handle)
{
	DecodedAudioFrame_t frame;
	while(handle->decoder.PopAudioFrame(frame))
	{
		if(handle->audioPlayer.Submit(frame.samples.data(), frame.samples.size(), frame.sampleRate, frame.channels))
			continue;
		PluginLogErrorAsync("Failed to submit decoded WebM audio frame");
	}
}

bool CopyDecodedFrame(WebmBinkHandle* handle)
{
	if(!handle->decoder.ReadVideoFrame())
	{
		SubmitDecodedAudio(handle);
		return false;
	}

	const AVFrame* frame = handle->decoder.GetVideoFrame();
	if(frame->format != AV_PIX_FMT_YUV420P)
	{
		PluginLogErrorAsync("Unsupported WebM pixel format: {}", av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
		return false;
	}
	if(frame->width != static_cast<int>(handle->width) || frame->height != static_cast<int>(handle->height))
	{
		PluginLogErrorAsync("WebM video frame dimensions changed: expected={}x{}, actual={}x{}", handle->width, handle->height,
							frame->width, frame->height);
		return false;
	}
	uint32_t chromaWidth = (handle->width + 1) / 2;
	uint32_t chromaHeight = (handle->height + 1) / 2;
	uint32_t bufferIndex = handle->frameBuffers->totalFrames == 2 ? handle->frameBuffers->frameNum ^ 1u : 0;
	BinkFramePlaneSet& buffer = handle->frameBuffers->frames[bufferIndex];
	if(!buffer.y.buffer || !buffer.cr.buffer || !buffer.cb.buffer)
	{
		PluginLogErrorAsync("Frame buffers are incomplete: one or more YCrCb plane pointers are null");
		return false;
	}

	const AVFrame* alphaFrame = nullptr;
	if(handle->decoder.HasAlpha())
	{
		alphaFrame = handle->decoder.GetAlphaFrame();
		if(alphaFrame->width != frame->width || alphaFrame->height != frame->height)
		{
			PluginLogErrorAsync("WebM alpha frame dimensions mismatch: expected={}x{}, actual={}x{}", frame->width, frame->height,
								alphaFrame->width, alphaFrame->height);
			return false;
		}
		if(alphaFrame->format != frame->format)
		{
			PluginLogErrorAsync("WebM alpha frame pixel format mismatch: expected={}, actual={}", frame->format, alphaFrame->format);
			return false;
		}
	}

	if(!handle->warnedColorSpace)
	{
		AVColorSpace colorSpace = frame->colorspace;
		if(colorSpace != AVCOL_SPC_BT470BG && colorSpace != AVCOL_SPC_SMPTE170M)
		{
			PluginLogWarnAsync("WebM color space differs from bik_ps40's BT.601 matrix assumption; colors may be incorrect: color_space={}",
							   av_color_space_name(colorSpace));
			handle->warnedColorSpace = true;
		}
	}
	if(!handle->warnedColorRange)
	{
		AVColorRange colorRange = frame->color_range;
		if(colorRange != AVCOL_RANGE_JPEG)
		{
			PluginLogWarnAsync("WebM color range differs from bik_ps40's full-range assumption; brightness may be incorrect: color_range={}",
							   av_color_range_name(colorRange));
			handle->warnedColorRange = true;
		}
	}

	SubmitDecodedAudio(handle);
	CopyPlane(buffer.y.buffer, buffer.y.pitch, frame->data[0], frame->linesize[0], handle->width, handle->height);
	CopyPlane(buffer.cr.buffer, buffer.cr.pitch, frame->data[2], frame->linesize[2], chromaWidth, chromaHeight);
	CopyPlane(buffer.cb.buffer, buffer.cb.pitch, frame->data[1], frame->linesize[1], chromaWidth, chromaHeight);
	if(alphaFrame)
	{
		CopyPlane(handle->alphaPlane.data(), handle->alphaPitch, alphaFrame->data[0], alphaFrame->linesize[0], handle->width, handle->height);
		WebmRenderer::UpdateAlphaTexture(handle->material, handle->alphaPlane.data(), handle->alphaPitch);
	}
	handle->frameBuffers->frameNum = bufferIndex;
	handle->decodedCurrentFrame = true;
	return true;
}

uint16_t __fastcall Hook_CreateBikMaterial(void* bik, const char* materialName, const char* fileName, const char* pathId, uint32_t flags)
{
	const std::filesystem::path* webmPath = WebmAssets::Resolve(fileName);
	if(!webmPath)
		return CreateBikMaterial(bik, materialName, fileName, pathId, flags);

	PluginLogInfo("Routing video: request={}, WebM={}", fileName, webmPath->string());

	OpenedWebmHandle = nullptr;
	PendingWebmPath = *webmPath;
	uint16_t handle = CreateBikMaterial(bik, materialName, fileName, pathId, flags);
	PendingWebmPath.clear();

	// OpenedWebmHandle from Hook_BinkOpen
	WebmBinkHandle* webmHandle = OpenedWebmHandle;
	std::unique_lock<std::mutex> handleLock;
	constexpr uint16_t INVALID_HEADER = 0xffff;
	if(handle == INVALID_HEADER || !TryAcquire(webmHandle, handleLock))
		return handle;
	if(!webmHandle->decoder.HasAlpha())
		return handle;

	webmHandle->material = GetBikMaterial(bik, handle);
	if(!WebmRenderer::PrepareAlphaTexture(webmHandle->material, webmHandle->width, webmHandle->height))
		PluginLogError("Failed to prepare WebM alpha texture: handle={}", handle);

	return handle;
}

void* __fastcall Hook_BinkOpen(const char* fileName, uint32_t flags)
{
	// PendingWebmPath from Hook_CreateBikMaterial
	std::filesystem::path webmPath = PendingWebmPath;
	if(webmPath.empty())
		return BinkOpen(fileName, flags);

	WebmBinkHandle* handle = new WebmBinkHandle();
	if(!handle->decoder.Open(webmPath))
		goto fallback;

	handle->width = handle->decoder.GetWidth();
	handle->height = handle->decoder.GetHeight();
	handle->frameCount = handle->decoder.GetFrameCount();
	handle->decoder.GetFrameRate(handle->frameRate, handle->frameRateDenom);
	handle->openFlags = flags;
	handle->trackCount = handle->decoder.HasAudio() ? 1 : 0;
	handle->startTime = std::chrono::steady_clock::now();
	if(handle->decoder.HasAlpha())
	{
		handle->alphaPitch = (handle->width + 15) & ~15u;
		handle->alphaPlane.assign(static_cast<size_t>(handle->alphaPitch) * handle->height, 0xff);
	}

	if(handle->frameCount == 0)
		goto fallback;

	if(handle->frameRate <= 0 || handle->frameRateDenom <= 0)
	{
		PluginLogError("WebM has invalid frame rate: {}/{}", handle->frameRate, handle->frameRateDenom);
		goto fallback;
	}

	{
		std::lock_guard lock(WebmHandlesMutex);
		WebmHandles.insert(handle);
	}
	OpenedWebmHandle = handle;

	return handle;

fallback:
	PluginLogWarn("Falling back to Bink: request={}, WebM={}", fileName, webmPath.string());
	delete handle;
	OpenedWebmHandle = nullptr;
	return BinkOpen(fileName, flags);
}

void __fastcall Hook_BinkClose(void* bink)
{
	{
		std::lock_guard lock(WebmHandlesMutex);
		auto handle = WebmHandles.find(static_cast<WebmBinkHandle*>(bink));
		if(handle == WebmHandles.end())
			goto original;

		WebmBinkHandle* webmHandle = *handle;
		{
			std::lock_guard handleLock(webmHandle->mutex);
			WebmHandles.erase(handle);
			if(webmHandle->decoder.HasAlpha())
				WebmRenderer::ReleaseAlphaTexture(webmHandle->material);
		}
		delete webmHandle;
	}
	return;

original:
	return BinkClose(bink);
}

void __fastcall Hook_BinkGetFrameBuffersInfo(void* bink, void* buffers)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkGetFrameBuffersInfo(bink, buffers);
	if(!buffers)
		return;

	BinkFrameBuffers* info = static_cast<BinkFrameBuffers*>(buffers);
	*info = {};
	info->totalFrames = handle->frameCount <= 1 ? 1 : 2;
	info->yWidth = handle->width;
	info->yHeight = handle->height;
	info->chromaWidth = (handle->width + 1) / 2;
	info->chromaHeight = (handle->height + 1) / 2;
	info->frameNum = 0;
	for(uint32_t frame = 0; frame < info->totalFrames; ++frame)
	{
		info->frames[frame].y.allocate = 1;
		info->frames[frame].cr.allocate = 1;
		info->frames[frame].cb.allocate = 1;
	}
}

void __fastcall Hook_BinkRegisterFrameBuffers(void* bink, void* buffers)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkRegisterFrameBuffers(bink, buffers);
	if(!buffers)
		return;

	handle->frameBuffers = static_cast<BinkFrameBuffers*>(buffers);
	return;
}

uint64_t __fastcall Hook_BinkWait(void* bink)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkWait(bink);

	handle->audioPlayer.Pump();
	if(handle->paused)
		return 1;

	double target = (handle->frameNumber - 1) * GetFrameDuration(handle);
	return GetPlaybackTime(handle) + 0.001 < target;
}

uint64_t __fastcall Hook_BinkDoFrame(void* bink)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkDoFrame(bink);

	handle->audioPlayer.Pump();
	if(handle->decodedCurrentFrame)
		return 0;
	if(CopyDecodedFrame(handle))
		return 0;

	handle->readError = 1;
	return 1;
}

void __fastcall Hook_BinkNextFrame(void* bink)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkNextFrame(bink);

	handle->lastFrameNumber = handle->frameNumber;
	if(++handle->frameNumber > handle->frameCount)
	{
		handle->frameNumber = 1;
		handle->decoder.SeekFrame(0);
		handle->audioPlayer.Reset();
		handle->startTime = std::chrono::steady_clock::now();
	}
	handle->decodedCurrentFrame = false;
}

uint64_t __fastcall Hook_BinkShouldSkip(void* bink)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkShouldSkip(bink);

	double target = handle->frameNumber * GetFrameDuration(handle);
	return GetPlaybackTime(handle) > target + GetFrameDuration(handle);
}

int __fastcall Hook_BinkPause(void* bink, int pause)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkPause(bink, pause);

	bool bPause = static_cast<bool>(pause);
	if(bPause == handle->paused)
		return pause;

	auto now = std::chrono::steady_clock::now();
	if(bPause)
		handle->pauseTime = now;
	else
		handle->startTime += now - handle->pauseTime;
	handle->paused = bPause;
	handle->audioPlayer.SetPaused(bPause);

	return pause;
}

void __fastcall Hook_BinkGoto(void* bink, uint32_t frame, uint32_t flags)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkGoto(bink, frame, flags);

	frame = std::clamp(frame, 1u, handle->frameCount);
	if(frame == handle->frameNumber)
		return;

	// Bink frame numbers started from one, while the decoder uses zero-based frame indices.
	if(!handle->decoder.SeekFrame(frame - 1))
		return;

	handle->audioPlayer.Reset();
	handle->lastFrameNumber = frame > 1 ? frame - 1 : UINT32_MAX;
	handle->frameNumber = frame;
	handle->decodedCurrentFrame = false;
	auto backedSeconds = std::chrono::duration<double>(static_cast<double>(frame - 1) * GetFrameDuration(handle));
	handle->startTime = std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(backedSeconds);
}

void __fastcall Hook_BinkGetRealtime(void* bink, void* realtime, uint32_t frameCount)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkGetRealtime(bink, realtime, frameCount);
	if(!realtime)
		return;

	// seems they only uses the first three BINKREALTIME fields for playback synchronization.
	memset(realtime, 0, 0x38);
	uint32_t* info = static_cast<uint32_t*>(realtime);
	info[0] = handle->lastFrameNumber;
	info[1] = handle->frameRate;
	info[2] = handle->frameRateDenom;
}

uint64_t __fastcall Hook_BinkSetSoundOnOff(void* bink, int enabled)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkSetSoundOnOff(bink, enabled);

	handle->audioPlayer.SetSoundEnabled(static_cast<bool>(enabled));
	return static_cast<uint64_t>(handle->decoder.HasAudio());
}

void __fastcall Hook_BinkSetSpeakerVolumes(void* bink, int track, const int* speakers, const int* volumes, uint32_t count)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkSetSpeakerVolumes(bink, track, speakers, volumes, count);
	if(track != 0)
		return;

	int volume = 32768;
	if(volumes && count > 0)
	{
		for(uint32_t index = 0; index < count; ++index)
			volume = std::max(volume, volumes[index]);
	}
	handle->audioPlayer.SetSpeakerVolume(volume);
}

uint32_t __fastcall Hook_BinkGetTrackId(void* bink, uint32_t track)
{
	if(!TryAcquire(bink))
		return BinkGetTrackId(bink, track);

	// we only handle track id 0
	return 0;
}

void __fastcall Hook_BinkSetVolume(void* bink, int track, int volume)
{
	WebmBinkHandle* handle = nullptr;
	std::unique_lock<std::mutex> handleLock;
	if(!TryAcquire(bink, handle, handleLock))
		return BinkSetVolume(bink, track, volume);
	if(track != 0)
		return;

	handle->audioPlayer.SetTrackVolume(volume);
}

bool __fastcall Hook_VideoPanelBeginPlayback(void* panel, const char* fileName)
{
	bool result = VideoPanelBeginPlayback(panel, fileName);
	// OpenedWebmHandle from Hook_BinkOpen
	WebmBinkHandle* webmHandle = OpenedWebmHandle;
	std::unique_lock<std::mutex> handleLock;
	if(result && TryAcquire(webmHandle, handleLock))
	{
		if(webmHandle->decoder.HasAlpha())
			*((uint8_t*)panel + VIDEO_PANEL_BLACK_BACKGROUND_OFFSET) = 0;
	}
	OpenedWebmHandle = nullptr;

	return result;
}

} // namespace


[[nodiscard]] bool RegisterWebmHooks_engine(HMODULE engine)
{
	CreateInterfaceFn createInterface;
	if(!HookUtils::ModuleExport(engine, "CreateInterface", (void**)&createInterface))
		return false;

	int returnCode;
	void*** vbik = (void***)createInterface("VBik001", &returnCode);
	if(returnCode != IFACE_OK || !vbik)
	{
		PluginLogError("engine.dll does not provide interface VBik001: return_code={}", returnCode);
		return false;
	}

	GetBikMaterial = (GetBikMaterialFn)HookUtils::VTableMethod(vbik, BIK_VTABLE_GET_MATERIAL);
	void* method = HookUtils::VTableMethod(vbik, BIK_VTABLE_CREATE_BIK_MATERIAL);
	return HookUtils::HookExport(engine, (uintptr_t)method - (uintptr_t)engine, (void*)Hook_CreateBikMaterial, (void**)&CreateBikMaterial);
}

[[nodiscard]] bool RegisterWebmHooks_client(HMODULE client)
{
	return HookUtils::HookExport(client, CLIENT_VIDEO_PANEL_BEGIN_PLAYBACK_RVA, (void*)Hook_VideoPanelBeginPlayback, (void**)&VideoPanelBeginPlayback);
}

[[nodiscard]] bool RegisterWebmHooks_bink2w64(HMODULE bink)
{
	// clang-format off
	const bool success =
		   HookUtils::HookExport(bink, "BinkOpen", 					(void*)Hook_BinkOpen,					(void**)&BinkOpen)
		&& HookUtils::HookExport(bink, "BinkClose", 				(void*)Hook_BinkClose,					(void**)&BinkClose)
		&& HookUtils::HookExport(bink, "BinkWait", 					(void*)Hook_BinkWait,					(void**)&BinkWait)
		&& HookUtils::HookExport(bink, "BinkDoFrame", 				(void*)Hook_BinkDoFrame,				(void**)&BinkDoFrame)
		&& HookUtils::HookExport(bink, "BinkShouldSkip", 			(void*)Hook_BinkShouldSkip,				(void**)&BinkShouldSkip)
		&& HookUtils::HookExport(bink, "BinkNextFrame", 			(void*)Hook_BinkNextFrame,				(void**)&BinkNextFrame)
		&& HookUtils::HookExport(bink, "BinkPause", 				(void*)Hook_BinkPause,					(void**)&BinkPause)
		&& HookUtils::HookExport(bink, "BinkGoto", 					(void*)Hook_BinkGoto,					(void**)&BinkGoto)
		&& HookUtils::HookExport(bink, "BinkGetFrameBuffersInfo", 	(void*)Hook_BinkGetFrameBuffersInfo,	(void**)&BinkGetFrameBuffersInfo)
		&& HookUtils::HookExport(bink, "BinkRegisterFrameBuffers", 	(void*)Hook_BinkRegisterFrameBuffers,	(void**)&BinkRegisterFrameBuffers)
		&& HookUtils::HookExport(bink, "BinkGetRealtime", 			(void*)Hook_BinkGetRealtime,			(void**)&BinkGetRealtime)
		&& HookUtils::HookExport(bink, "BinkSetSoundOnOff", 		(void*)Hook_BinkSetSoundOnOff,			(void**)&BinkSetSoundOnOff)
		&& HookUtils::HookExport(bink, "BinkSetSpeakerVolumes", 	(void*)Hook_BinkSetSpeakerVolumes,		(void**)&BinkSetSpeakerVolumes)
		&& HookUtils::HookExport(bink, "BinkGetTrackID", 			(void*)Hook_BinkGetTrackId,				(void**)&BinkGetTrackId)
		&& HookUtils::HookExport(bink, "BinkSetVolume", 			(void*)Hook_BinkSetVolume,				(void**)&BinkSetVolume)
		;
	// clang-format on
	if(success)
		PluginLogInfo("Successfully registered WebmCodec hooks");
	return success;
}

[[nodiscard]] bool EnableWebmHooks()
{
	if(!HookUtils::EnableMinHook(MH_ALL_HOOKS))
	{
		PluginLogError("Failed while enabling all previously registered Webm hooks");
		return false;
	}
	PluginLogInfo("Successfully enabled all previously registered Webm hooks");
	return true;
}

[[nodiscard]] bool PrepareRemoveWebmHooks()
{
	std::lock_guard lock(WebmHandlesMutex);
	if(!WebmHandles.empty())
	{
		PluginLogError("Refusing to unload bik_to_webm: currently active WebM videos: {}", WebmHandles.size());
		return false;
	}
	return true;
}
