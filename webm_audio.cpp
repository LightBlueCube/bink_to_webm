#include "lib/plugin_logging.h"
#include "lib/hook.h"
#include "webm_audio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

// clang-format off
using MilesSampleCreateFn 				= void* 	(__fastcall*)(void* driver, void* bus, uint32_t flags);
using MilesSampleDestroyFn 				= void 		(__fastcall*)(void* sample);
using MilesSampleSetSourceDiscardsFn 	= uint32_t 	(__fastcall*)(void* sample, void* callback);
using MilesSampleAddSourceFn 			= uint32_t	(__fastcall*)(void* sample, void* data, uint32_t byteCount);
using MilesSampleSetSourceRawFn 		= uint32_t 	(__fastcall*)(void* sample, void* data, uint32_t flags, int sampleRate, uint16_t channels);
using MilesSampleSetEobCallbackFn 		= void 		(__fastcall*)(void* sample, void* callback);
using MilesSamplePlayFn 				= void 		(__fastcall*)(void* sample);
using MilesSampleGetBufferCountFn 		= int 		(__fastcall*)(void* sample);
MilesSampleCreateFn 			MilesSampleCreate 				= nullptr;
MilesSampleDestroyFn 			MilesSampleDestroy 				= nullptr;
MilesSampleSetSourceDiscardsFn 	MilesSampleSetSourceDiscards 	= nullptr;
MilesSampleAddSourceFn 			MilesSampleAddSource 			= nullptr;
MilesSampleSetSourceRawFn 		MilesSampleSetSourceRaw 		= nullptr;
MilesSampleSetEobCallbackFn 	MilesSampleSetEobCallback 		= nullptr;
MilesSamplePlayFn 				MilesSamplePlay 				= nullptr;
MilesSampleGetBufferCountFn		MilesSampleGetBufferCount		= nullptr;
// clang-format on

constexpr uintptr_t CLIENT_MILES_DRIVER_RVA = 0x2a9f080;
// Theirs voice streaming path caps the number of queued raw sources at three.
constexpr uint32_t MILES_MAX_QUEUED_SOURCES = 3;

void** MilesDriver = nullptr;

std::unordered_map<void*, std::atomic_bool*> MilesSourceStates;
std::mutex MilesMutex;



void __fastcall OnMilesSourceFinished(void*, void* source)
{
	std::lock_guard lock(MilesMutex);
	auto sourceState = MilesSourceStates.find(source);
	if(sourceState == MilesSourceStates.end())
		return;

	*sourceState->second = true;
}

void __fastcall OnMilesSourceDiscarded(void*, uint32_t) {}

} // namespace

struct WebmAudioPlayer::AudioBuffer_t
{
	std::vector<int16_t> samples;
	std::atomic_bool finished = false;
	bool submitted = false;
};

WebmAudioPlayer::WebmAudioPlayer() = default;

WebmAudioPlayer::~WebmAudioPlayer()
{
	std::lock_guard lock(m_mutex);
	Close_Locked();
}

bool WebmAudioPlayer::Submit(const int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint16_t channels)
{
	std::lock_guard lock(m_mutex);
	if(!m_soundEnabled || m_paused)
		return true;

	if(!m_milesSample)
	{
		if(!Open_Locked(sampleRate, channels))
			return false;
	}

	auto buffer = std::make_unique<AudioBuffer_t>();
	buffer->samples.resize(sampleCount);
	float volume = m_trackVolume * m_speakerVolume;
	for(size_t index = 0; index < sampleCount; ++index)
	{
		long sample = std::lrint(static_cast<float>(samples[index]) * volume);
		buffer->samples[index] = static_cast<int16_t>(sample);
	}
	m_buffers.push_back(std::move(buffer));

	if(!QueuePending_Locked())
	{
		Close_Locked();
		return false;
	}
	return true;
}

void WebmAudioPlayer::Pump()
{
	std::lock_guard lock(m_mutex);
	Pump_Locked();
}

void WebmAudioPlayer::Reset()
{
	std::lock_guard lock(m_mutex);
	Close_Locked();
}

void WebmAudioPlayer::SetPaused(bool paused)
{
	std::lock_guard lock(m_mutex);
	if(!m_paused && paused)
		Close_Locked();
	m_paused = paused;
}

void WebmAudioPlayer::SetSoundEnabled(bool enabled)
{
	std::lock_guard lock(m_mutex);
	if(m_soundEnabled && !enabled)
		Close_Locked();
	m_soundEnabled = enabled;
}

void WebmAudioPlayer::SetTrackVolume(int volume)
{
	std::lock_guard lock(m_mutex);
	m_trackVolume = std::clamp(static_cast<float>(volume) / 32768.0f, 0.0f, 1.0f);
}

void WebmAudioPlayer::SetSpeakerVolume(int volume)
{
	std::lock_guard lock(m_mutex);
	m_speakerVolume = std::clamp(static_cast<float>(volume) / 32768.0f, 0.0f, 1.0f);
}

bool WebmAudioPlayer::Open_Locked(uint32_t sampleRate, uint16_t channels)
{
	m_milesSample = MilesSampleCreate(*MilesDriver, nullptr, 0);
	if(!m_milesSample)
		return false;

	MilesSampleSetEobCallback(m_milesSample, (void*)OnMilesSourceFinished);
	if(!MilesSampleSetSourceRaw(m_milesSample, m_dummySamples, 0, static_cast<int>(sampleRate), channels) ||
	   !MilesSampleSetSourceDiscards(m_milesSample, (void*)OnMilesSourceDiscarded))
	{
		MilesSampleDestroy(m_milesSample);
		m_milesSample = nullptr;
		return false;
	}
	MilesSamplePlay(m_milesSample);

	PluginLogInfoAsync("Opened WebM audio through Miles: sample_rate={}, channels={}", sampleRate, channels);
	return true;
}

void WebmAudioPlayer::Pump_Locked()
{
	for(auto buffer = m_buffers.cbegin(); buffer != m_buffers.cend();)
	{
		if(!(*buffer)->finished)
		{
			++buffer;
			continue;
		}

		{
			std::lock_guard milesLock(MilesMutex);
			MilesSourceStates.erase((*buffer)->samples.data());
		}
		buffer = m_buffers.erase(buffer);
	}

	if(!QueuePending_Locked())
		Close_Locked();
}

bool WebmAudioPlayer::QueuePending_Locked()
{
	if(!m_milesSample)
		return true;

	uint32_t queuedSources = MilesSampleGetBufferCount(m_milesSample);
	for(const auto& buffer : m_buffers)
	{
		if(queuedSources >= MILES_MAX_QUEUED_SOURCES)
			break;
		if(buffer->submitted)
			continue;

		void* data = buffer->samples.data();
		{
			std::lock_guard milesLock(MilesMutex);
			MilesSourceStates[data] = &buffer->finished;
		}

		uint32_t byteCount = static_cast<uint32_t>(buffer->samples.size() * sizeof(int16_t));
		if(!MilesSampleAddSource(m_milesSample, data, byteCount))
		{
			PluginLogErrorAsync("MilesSampleAddSource failed: byte_count={}, queued_sources={}", byteCount, queuedSources);
			std::lock_guard milesLock(MilesMutex);
			MilesSourceStates.erase(data);
			return false;
		}

		buffer->submitted = true;
		++queuedSources;
	}

	return true;
}

void WebmAudioPlayer::Close_Locked()
{
	void* milesSample = m_milesSample;
	m_milesSample = nullptr;
	if(milesSample)
		MilesSampleDestroy(milesSample);

	{
		std::lock_guard milesLock(MilesMutex);
		for(const auto& buffer : m_buffers)
			MilesSourceStates.erase(buffer->samples.data());
	}

	m_buffers.clear();
}


void SetupWebmAudio_client(HMODULE client)
{
	HookUtils::ModuleExport(client, CLIENT_MILES_DRIVER_RVA, (void**)&MilesDriver);
}

[[nodiscard]] bool SetupWebmAudio_mileswin64(HMODULE miles)
{
	// clang-format off
	const bool success =
		   HookUtils::ModuleExport(miles, "MilesSampleCreate", 				(void**)&MilesSampleCreate)
		&& HookUtils::ModuleExport(miles, "MilesSampleDestroy", 			(void**)&MilesSampleDestroy)
		&& HookUtils::ModuleExport(miles, "MilesSampleSetSourceDiscards", 	(void**)&MilesSampleSetSourceDiscards)
		&& HookUtils::ModuleExport(miles, "MilesSampleAddSource", 			(void**)&MilesSampleAddSource)
		&& HookUtils::ModuleExport(miles, "MilesSampleSetSourceRaw", 		(void**)&MilesSampleSetSourceRaw)
		&& HookUtils::ModuleExport(miles, "MilesSampleSetEOBCallback", 		(void**)&MilesSampleSetEobCallback)
		&& HookUtils::ModuleExport(miles, "MilesSamplePlay", 				(void**)&MilesSamplePlay)
		&& HookUtils::ModuleExport(miles, "MilesSampleGetBufferCount", 		(void**)&MilesSampleGetBufferCount)
		;
	// clang-format on

	if(success)
		PluginLogInfo("Successfully installed WebmAudioMiles hooks");
	return success;
}
