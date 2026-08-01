#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <windows.h>

class WebmAudioPlayer
{
public:
	WebmAudioPlayer();
	~WebmAudioPlayer();

	WebmAudioPlayer(const WebmAudioPlayer&) = delete;
	WebmAudioPlayer& operator=(const WebmAudioPlayer&) = delete;

	bool Submit(const int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint16_t channels);
	void Pump();
	void Reset();
	void SetPaused(bool paused);
	void SetSoundEnabled(bool enabled);
	void SetTrackVolume(int volume);
	void SetSpeakerVolume(int volume);

private:
	struct AudioBuffer_t;

	bool Open_Locked(uint32_t sampleRate, uint16_t channels);
	void Pump_Locked();
	bool QueuePending_Locked();
	void Close_Locked();

	std::mutex m_mutex;
	void* m_milesSample = nullptr;
	std::vector<std::unique_ptr<AudioBuffer_t>> m_buffers;
	int16_t m_dummySamples[2] = {};
	float m_trackVolume = 1.0f;
	float m_speakerVolume = 1.0f;
	bool m_paused = false;
	bool m_soundEnabled = true;
};


void SetupWebmAudio_client(HMODULE client);
[[nodiscard]] bool SetupWebmAudio_mileswin64(HMODULE miles);
