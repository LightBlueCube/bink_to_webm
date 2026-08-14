#pragma once

#include "lib/plugin_logging.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <vector>
#include <format>
#include <utility>

extern "C"
{
	struct AVCodecContext;
	struct AVFormatContext;
	struct AVFrame;
	struct AVPacket;
	struct AVStream;
	struct SwrContext;
}

struct DecodedAudioFrame_t
{
	std::vector<int16_t> samples;
	uint32_t sampleRate = 0;
	uint16_t channels = 0;
};

class WebmDecoder
{
public:
	WebmDecoder() = default;
	~WebmDecoder();

	WebmDecoder(const WebmDecoder&) = delete;
	WebmDecoder& operator=(const WebmDecoder&) = delete;

	bool Open(const std::filesystem::path& path);
	void Clear();
	bool ReadVideoFrame();
	bool SeekFrame(uint32_t frame);
	bool PopAudioFrame(DecodedAudioFrame_t& frame);

	const AVFrame* GetVideoFrame() const;
	const AVFrame* GetAlphaFrame() const;
	int GetWidth() const;
	int GetHeight() const;
	uint32_t GetFrameCount() const;
	void GetFrameRate(uint32_t&, uint32_t&) const;
	bool HasAudio() const;
	bool HasAlpha() const;

private:
	bool OpenDecoder(int stream, AVCodecContext** context);
	int ReceiveFrames();
	int ProcessPacket();
	int SubmitVideoPacket(const AVPacket* packet);
	int SubmitAlphaPacket(const AVPacket* packet);
	int SubmitAudioPacket(const AVPacket* packet);
	bool StartDecoderDrain();
	bool ReceiveAudioFrames();
	bool InitializeAudioResampler(const AVFrame* frame);
	void ResetAudioState();

	template<typename... Args>
	bool FailCleanup(std::format_string<Args...> fmt, Args&&... args)
	{
		PluginLogError(fmt, std::forward<Args>(args)...);
		return Cleanup();
	}
	template<typename... Args>
	bool Fail(std::format_string<Args...> fmt, Args&&... args) const
	{
		PluginLogError(fmt, std::forward<Args>(args)...);
		return false;
	}
	bool Cleanup()
	{
		Clear();
		return false;
	}

	AVFormatContext* m_formatContext = nullptr;
	AVCodecContext* m_videoDecoder = nullptr;
	AVCodecContext* m_alphaDecoder = nullptr;
	AVCodecContext* m_audioDecoder = nullptr;
	AVFrame* m_videoFrame = nullptr;
	AVFrame* m_alphaFrame = nullptr;
	AVFrame* m_audioFrame = nullptr;
	AVPacket* m_packet = nullptr;
	AVPacket* m_alphaPacket = nullptr;
	SwrContext* m_audioResampler = nullptr;
	AVStream* m_videoStream = nullptr;
	AVStream* m_audioStream = nullptr;
	std::deque<DecodedAudioFrame_t> m_audioFrames;
	int m_videoStreamIndex = -1;
	int m_audioStreamIndex = -1;
	int64_t m_minimumVideoTimestamp = INT64_MIN;
	int64_t m_minimumAudioTimestamp = INT64_MIN;
	int m_outputAudioChannels = 0;
};
