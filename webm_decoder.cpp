#include "lib/plugin_logging.h"
#include "webm_decoder.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace
{

constexpr int SAMPLE_RATE_HZ = 48000;

std::string GetFFmpegError(int error)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error, buffer, sizeof(buffer));
	return buffer;
}

} // namespace

WebmDecoder::~WebmDecoder()
{
	Clear();
}

bool WebmDecoder::Open(const std::filesystem::path& path)
{
	Clear();

	std::string fileName = path.string();
	int result = avformat_open_input(&m_formatContext, fileName.c_str(), nullptr, nullptr);
	if(result < 0)
	{
		return FailCleanup("Could not open: {}", GetFFmpegError(result));
	}

	result = avformat_find_stream_info(m_formatContext, nullptr);
	if(result < 0)
	{
		return FailCleanup("Could not read WebM stream information: {}", GetFFmpegError(result));
	}

	m_videoStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if(m_videoStreamIndex < 0)
	{
		return FailCleanup("Failed while finding video stream: {}", GetFFmpegError(m_videoStreamIndex));
	}
	m_videoStream = m_formatContext->streams[m_videoStreamIndex];
	AVCodecID videoCodec = m_videoStream->codecpar->codec_id;
	if(videoCodec != AV_CODEC_ID_VP8 && videoCodec != AV_CODEC_ID_VP9)
	{
		return FailCleanup("WebM uses unsupported video codec: {}", avcodec_get_name(videoCodec));
	}
	if(!OpenDecoder(m_videoStreamIndex, &m_videoDecoder))
		return Cleanup();

	const AVDictionaryEntry* alphaMode = av_dict_get(m_videoStream->metadata, "alpha_mode", nullptr, 0);
	if(alphaMode && strcmp(alphaMode->value, "1") == 0)
		if(!OpenDecoder(m_videoStreamIndex, &m_alphaDecoder))
			return Cleanup();

	m_audioStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if(m_audioStreamIndex < 0 && m_audioStreamIndex != AVERROR_STREAM_NOT_FOUND)
	{
		return FailCleanup("Failed while finding audio stream: {}", GetFFmpegError(m_audioStreamIndex));
	}
	if(m_audioStreamIndex >= 0)
	{
		m_audioStream = m_formatContext->streams[m_audioStreamIndex];
		AVCodecID audioCodec = m_audioStream->codecpar->codec_id;
		if(audioCodec != AV_CODEC_ID_OPUS)
		{
			return FailCleanup("WebM uses unsupported audio codec: {}", avcodec_get_name(audioCodec));
		}

		int channelCount = m_audioStream->codecpar->ch_layout.nb_channels;
		if(channelCount < 1 || channelCount > 2)
		{
			return FailCleanup("WebM uses unsupported audio channel count: {}", channelCount);
		}

		if(!OpenDecoder(m_audioStreamIndex, &m_audioDecoder))
			return Cleanup();
	}


	if(!(m_videoFrame = av_frame_alloc()))
		return FailCleanup("Could not allocate {}", "m_videoFrame");
	if(m_alphaDecoder)
	{
		if(!(m_alphaFrame = av_frame_alloc()))
			return FailCleanup("Could not allocate {}", "m_alphaFrame");
		if(!(m_alphaPacket = av_packet_alloc()))
			return FailCleanup("Could not allocate {}", "m_alphaPacket");
	}
	if(m_audioDecoder)
	{
		if(!(m_audioFrame = av_frame_alloc()))
			return FailCleanup("Could not allocate {}", "m_audioFrame");
	}
	if(!(m_packet = av_packet_alloc()))
		return FailCleanup("Could not allocate {}", "m_packet");

	PluginLogInfo("Opened WebM: file={}, size={}x{}, codec={}, alpha={}, audio={}", fileName, m_videoDecoder->width, m_videoDecoder->height,
				  avcodec_get_name(videoCodec), HasAlpha(), HasAudio());
	return true;
}

bool WebmDecoder::OpenDecoder(int stream, AVCodecContext** context)
{
	AVStream* streamInfo = m_formatContext->streams[stream];
	const AVCodec* codec = avcodec_find_decoder(streamInfo->codecpar->codec_id);
	if(!codec)
	{
		PluginLogError("FFmpeg has no decoder for codec: {}", avcodec_get_name(streamInfo->codecpar->codec_id));
		return false;
	}

	*context = avcodec_alloc_context3(codec);
	if(!*context)
	{
		PluginLogError("Failed to allocate codec context for codec: {}", avcodec_get_name(streamInfo->codecpar->codec_id));
		return false;
	}

	int result = avcodec_parameters_to_context(*context, streamInfo->codecpar);
	if(result < 0)
	{
		PluginLogError("Failed to copy parameters to codec context: err={}, codec={}", GetFFmpegError(result), codec->name);
		return false;
	}

	result = avcodec_open2(*context, codec, nullptr);
	if(result < 0)
	{
		PluginLogError("Failed to open codec: err={}, codec={}", GetFFmpegError(result), codec->name);
		return false;
	}

	return true;
}

void WebmDecoder::Clear()
{
	ResetAudioState();
	av_packet_free(&m_alphaPacket);
	av_packet_free(&m_packet);
	av_frame_free(&m_audioFrame);
	av_frame_free(&m_alphaFrame);
	av_frame_free(&m_videoFrame);
	avcodec_free_context(&m_audioDecoder);
	avcodec_free_context(&m_alphaDecoder);
	avcodec_free_context(&m_videoDecoder);
	avformat_close_input(&m_formatContext);

	m_videoStream = nullptr;
	m_audioStream = nullptr;
	m_videoStreamIndex = -1;
	m_audioStreamIndex = -1;
	m_minimumVideoTimestamp = AV_NOPTS_VALUE;
	m_minimumAudioTimestamp = AV_NOPTS_VALUE;
}

bool WebmDecoder::SeekFrame(uint32_t frame)
{
	AVRational frameRate = av_guess_frame_rate(m_formatContext, m_videoStream, nullptr);
	if(frameRate.num <= 0 || frameRate.den <= 0)
		return false;

	int64_t timestamp = av_rescale_q(frame, av_inv_q(frameRate), m_videoStream->time_base);
	if(m_videoStream->start_time != AV_NOPTS_VALUE)
		timestamp += m_videoStream->start_time;

	int result = av_seek_frame(m_formatContext, m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
	if(result < 0)
	{
		PluginLogError("Could not seek WebM video: {}", GetFFmpegError(result));
		return false;
	}

	avcodec_flush_buffers(m_videoDecoder);
	m_minimumVideoTimestamp = timestamp;
	if(m_alphaDecoder)
	{
		avcodec_flush_buffers(m_alphaDecoder);
	}
	if(m_audioDecoder)
	{
		avcodec_flush_buffers(m_audioDecoder);
		ResetAudioState();
		m_minimumAudioTimestamp = av_rescale_q(timestamp, m_videoStream->time_base, m_audioStream->time_base);
	}
	return true;
}

bool WebmDecoder::PopAudioFrame(DecodedAudioFrame_t& frame)
{
	if(m_audioFrames.empty())
		return false;

	frame = std::move(m_audioFrames.front());
	m_audioFrames.pop_front();
	return true;
}

bool WebmDecoder::ReadVideoFrame()
{
	for(;;)
	{
		int result = ReceiveFrames();
		if(result >= 0)
			return true;
		if(result == AVERROR_EOF)
			return false;
		if(result != AVERROR(EAGAIN))
			return false;

		if((result = ProcessPacket()) >= 0)
			continue;
		if(result != AVERROR_EOF)
			return false;
		if(!StartDecoderDrain())
			return false;
	}
}

int WebmDecoder::ReceiveFrames()
{
	if(m_audioDecoder)
	{
		if(!ReceiveAudioFrames())
			return AVERROR_INVALIDDATA;
	}

	for(;;)
	{
		int result = avcodec_receive_frame(m_videoDecoder, m_videoFrame);
		if(result < 0)
		{
			if(result != AVERROR(EAGAIN) && result != AVERROR_EOF)
				PluginLogError("Could not receive WebM video frame: {}", GetFFmpegError(result));
			return result;
		}

		if(m_alphaDecoder)
		{
			result = avcodec_receive_frame(m_alphaDecoder, m_alphaFrame);
			if(result < 0)
			{
				PluginLogError("Could not receive matching WebM alpha frame: {}", GetFFmpegError(result));
				return AVERROR_INVALIDDATA;
			}
		}

		int64_t timestamp = m_videoFrame->best_effort_timestamp;
		if(m_minimumVideoTimestamp != AV_NOPTS_VALUE && timestamp != AV_NOPTS_VALUE)
			if(timestamp < m_minimumVideoTimestamp)
				continue;
		m_minimumVideoTimestamp = AV_NOPTS_VALUE;

		break;
	}

	return 0;
}

int WebmDecoder::ProcessPacket()
{
	for(;;)
	{
		int result = av_read_frame(m_formatContext, m_packet);
		if(result < 0)
		{
			if(result != AVERROR_EOF)
				PluginLogError("Could not read WebM packet: {}", GetFFmpegError(result));
			return result;
		}

		if(m_packet->stream_index == m_videoStreamIndex)
		{
			result = SubmitVideoPacket(m_packet);
			av_packet_unref(m_packet);
			if(result < 0)
				return AVERROR_EXIT;
			return 0;
		}

		if(m_packet->stream_index == m_audioStreamIndex)
		{
			result = SubmitAudioPacket(m_packet);
			av_packet_unref(m_packet);
			if(result < 0)
				return AVERROR_EXIT;
			return 0;
		}

		av_packet_unref(m_packet);
	}
}

int WebmDecoder::SubmitVideoPacket(const AVPacket* packet)
{
	int result = avcodec_send_packet(m_videoDecoder, packet);
	if(result < 0)
	{
		PluginLogError("Could not submit WebM video packet: {}", GetFFmpegError(result));
		return result;
	}

	if(!m_alphaDecoder)
		return result;
	return SubmitAlphaPacket(packet);
}

int WebmDecoder::SubmitAlphaPacket(const AVPacket* packet)
{
	size_t alphaSize = 0;
	const uint8_t* alphaData = av_packet_get_side_data(packet, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &alphaSize);
	constexpr size_t HEADER_SIZE = 8;
	if(!alphaData || alphaSize <= HEADER_SIZE || AV_RB64(alphaData) != 1)
	{
		PluginLogError("WebM alpha track is missing BlockAdditional ID 1 for a video frame");
		return AVERROR_INVALIDDATA;
	}

	int result = av_new_packet(m_alphaPacket, static_cast<int>(alphaSize - HEADER_SIZE));
	if(result < 0)
	{
		PluginLogError("Could not allocate WebM alpha packet: {}", GetFFmpegError(result));
		return result;
	}

	memcpy(m_alphaPacket->data, alphaData + HEADER_SIZE, alphaSize - HEADER_SIZE);
	m_alphaPacket->pts = packet->pts;
	m_alphaPacket->dts = packet->dts;
	m_alphaPacket->duration = packet->duration;
	m_alphaPacket->pos = packet->pos;
	m_alphaPacket->flags = packet->flags;
	m_alphaPacket->time_base = packet->time_base;
	result = avcodec_send_packet(m_alphaDecoder, m_alphaPacket);
	av_packet_unref(m_alphaPacket);
	if(result < 0)
		PluginLogError("Could not submit WebM alpha packet: {}", GetFFmpegError(result));
	return result;
}

int WebmDecoder::SubmitAudioPacket(const AVPacket* packet)
{
	int result = avcodec_send_packet(m_audioDecoder, packet);
	if(result < 0)
		PluginLogError("Could not submit WebM audio packet: {}", GetFFmpegError(result));
	return result;
}

bool WebmDecoder::StartDecoderDrain()
{
	int result = avcodec_send_packet(m_videoDecoder, nullptr);
	if(result < 0 && result != AVERROR_EOF)
		return Fail("Could not flush WebM video decoder: {}", GetFFmpegError(result));

	if(m_alphaDecoder)
	{
		result = avcodec_send_packet(m_alphaDecoder, nullptr);
		if(result < 0 && result != AVERROR_EOF)
			return Fail("Could not flush WebM alpha decoder: {}", GetFFmpegError(result));
	}

	if(m_audioDecoder)
	{
		result = avcodec_send_packet(m_audioDecoder, nullptr);
		if(result < 0 && result != AVERROR_EOF)
			return Fail("Could not flush WebM audio decoder: {}", GetFFmpegError(result));
	}

	return true;
}

bool WebmDecoder::ReceiveAudioFrames()
{
	for(;;)
	{
		av_frame_unref(m_audioFrame);
		int result = avcodec_receive_frame(m_audioDecoder, m_audioFrame);
		if(result < 0)
		{
			if(result == AVERROR(EAGAIN))
				return true;

			if(result == AVERROR_EOF)
			{
				swr_free(&m_audioResampler);
				m_outputAudioChannels = 0;
				m_minimumAudioTimestamp = AV_NOPTS_VALUE;
				return true;
			}

			PluginLogError("Could not receive WebM audio frame: {}", GetFFmpegError(result));
			return false;
		}

		if(m_audioFrame->nb_samples <= 0)
			continue;
		int inputRate = m_audioFrame->sample_rate;
		if(inputRate != SAMPLE_RATE_HZ)
			return Fail("WebM uses unsupported audio sample rate: expected={}, actual={}", SAMPLE_RATE_HZ, inputRate);

		if(m_minimumAudioTimestamp != AV_NOPTS_VALUE && m_audioFrame->best_effort_timestamp != AV_NOPTS_VALUE)
		{
			int64_t timestamp = m_audioFrame->best_effort_timestamp;
			int64_t duration = av_rescale_q(m_audioFrame->nb_samples, AVRational{1, inputRate}, m_audioStream->time_base);
			if(timestamp + duration < m_minimumAudioTimestamp)
				continue;

			m_minimumAudioTimestamp = AV_NOPTS_VALUE;
		}

		if(!m_audioResampler)
		{
			if(!InitializeAudioResampler(m_audioFrame))
				return false;
		}

		int outputCapacity = m_audioFrame->nb_samples;
		DecodedAudioFrame_t output;
		output.sampleRate = SAMPLE_RATE_HZ;
		output.channels = static_cast<uint16_t>(m_outputAudioChannels);
		output.samples.resize(static_cast<size_t>(outputCapacity) * output.channels);

		uint8_t* outputData = (uint8_t*)output.samples.data();
		const uint8_t* inputData[2] = {m_audioFrame->extended_data[0], nullptr};
		if(m_audioFrame->ch_layout.nb_channels > 1)
			inputData[1] = m_audioFrame->extended_data[1];

		int sampleCount = swr_convert(m_audioResampler, &outputData, outputCapacity, inputData, m_audioFrame->nb_samples);
		if(sampleCount < 0)
		{
			PluginLogError("Could not convert WebM audio frame: {}", GetFFmpegError(sampleCount));
			return false;
		}
		output.samples.resize(static_cast<size_t>(sampleCount) * output.channels);
		if(output.samples.empty())
			continue;
		m_audioFrames.push_back(std::move(output));
	}
}

bool WebmDecoder::InitializeAudioResampler(const AVFrame* frame)
{
	m_outputAudioChannels = frame->ch_layout.nb_channels;

	int result = swr_alloc_set_opts2(&m_audioResampler, &frame->ch_layout, AV_SAMPLE_FMT_S16, SAMPLE_RATE_HZ, &frame->ch_layout,
									 static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
	if(result < 0)
		goto cleanup;
	if((result = swr_init(m_audioResampler)) < 0)
		goto cleanup;
	return true;

cleanup:
	swr_free(&m_audioResampler);
	return Fail("Could not initialize WebM audio resampler: {}", GetFFmpegError(result));
}

void WebmDecoder::ResetAudioState()
{
	swr_free(&m_audioResampler);
	m_audioFrames.clear();
	m_outputAudioChannels = 0;
}

const AVFrame* WebmDecoder::GetVideoFrame() const
{
	return m_videoFrame;
}

const AVFrame* WebmDecoder::GetAlphaFrame() const
{
	return m_alphaFrame;
}

int WebmDecoder::GetWidth() const
{
	return m_videoDecoder->width;
}

int WebmDecoder::GetHeight() const
{
	return m_videoDecoder->height;
}

bool WebmDecoder::HasAudio() const
{
	return m_audioDecoder;
}

bool WebmDecoder::HasAlpha() const
{
	return m_alphaDecoder;
}

uint32_t WebmDecoder::GetFrameCount() const
{
	AVRational frameRate = av_guess_frame_rate(m_formatContext, m_videoStream, nullptr);
	if(frameRate.num <= 0 || frameRate.den <= 0)
	{
		PluginLogError("WebM has invalid frame rate: {}/{}", frameRate.num, frameRate.den);
		return 0;
	}

	int64_t duration = m_videoStream->duration;
	if(duration <= 0)
	{
		if(m_formatContext->duration <= 0)
		{
			PluginLogError("WebM has invalid duration: stream_duration={}, container_duration={}", duration, m_formatContext->duration);
			return 0;
		}

		duration = av_rescale_q(m_formatContext->duration, AVRational{1, AV_TIME_BASE}, m_videoStream->time_base);
		if(duration <= 0)
		{
			PluginLogError("WebM duration conversion failed: stream_duration={}, container_duration={}", duration, m_formatContext->duration);
			return 0;
		}
	}

	int64_t frameCount = av_rescale_q(duration, m_videoStream->time_base, av_inv_q(frameRate));
	if(frameCount <= 0)
	{
		PluginLogError("WebM has invalid frame count: frame_count={}, duration={}, time_base={}/{}, frame_rate={}/{}", frameCount, duration,
					   m_videoStream->time_base.num, m_videoStream->time_base.den, frameRate.num, frameRate.den);
		return 0;
	}
	if(frameCount > UINT32_MAX)
	{
		PluginLogError("WebM frame count exceeds Bink limit: frame_count={}, limit={}", frameCount, UINT32_MAX);
		return 0;
	}

	return static_cast<uint32_t>(frameCount);
}

void WebmDecoder::GetFrameRate(uint32_t& num, uint32_t& den) const
{
	AVRational frameRate = av_guess_frame_rate(m_formatContext, m_videoStream, nullptr);
	num = frameRate.num;
	den = frameRate.den;
}
