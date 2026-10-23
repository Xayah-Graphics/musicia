#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

import std;
import musicia.audio;

namespace musicia {
    static void audioError(int status) {
        if (status >= 0) return;
        char detail[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(status, detail, sizeof(detail));
        throw std::runtime_error(detail);
    }

    AudioClip AudioClip::read(const std::filesystem::path& path) {
        AudioClip clip{.path = path};
        const auto utf8 = path.u8string();
        AVFormatContext* input{};
        audioError(avformat_open_input(&input, reinterpret_cast<const char*>(utf8.c_str()), nullptr, nullptr));
        const auto releaseInput = [](AVFormatContext* value) { avformat_close_input(&value); };
        const std::unique_ptr<AVFormatContext, decltype(releaseInput)> inputOwner(input);
        audioError(avformat_find_stream_info(input, nullptr));
        const AVCodec* codec{};
        const int stream = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        audioError(stream);
        const auto releaseDecoder = [](AVCodecContext* value) { avcodec_free_context(&value); };
        const std::unique_ptr<AVCodecContext, decltype(releaseDecoder)> decoder(avcodec_alloc_context3(codec));
        audioError(avcodec_parameters_to_context(decoder.get(), input->streams[stream]->codecpar));
        audioError(avcodec_open2(decoder.get(), codec, nullptr));
        clip.sampleRate = decoder->sample_rate;
        clip.channels = decoder->ch_layout.nb_channels;
        const auto releaseResampler = [](SwrContext* value) { swr_free(&value); };
        SwrContext* rawSource{};
        audioError(swr_alloc_set_opts2(&rawSource, &decoder->ch_layout, AV_SAMPLE_FMT_FLT, clip.sampleRate,
            &decoder->ch_layout, decoder->sample_fmt, clip.sampleRate, 0, nullptr));
        const std::unique_ptr<SwrContext, decltype(releaseResampler)> source(rawSource);
        audioError(swr_init(source.get()));
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        SwrContext* rawAnalysis{};
        audioError(swr_alloc_set_opts2(&rawAnalysis, &mono, AV_SAMPLE_FMT_FLT, 24000,
            &decoder->ch_layout, decoder->sample_fmt, clip.sampleRate, 0, nullptr));
        const std::unique_ptr<SwrContext, decltype(releaseResampler)> analysis(rawAnalysis);
        audioError(swr_init(analysis.get()));
        const auto releasePacket = [](AVPacket* value) { av_packet_free(&value); };
        const auto releaseFrame = [](AVFrame* value) { av_frame_free(&value); };
        const std::unique_ptr<AVPacket, decltype(releasePacket)> packet(av_packet_alloc());
        const std::unique_ptr<AVFrame, decltype(releaseFrame)> frame(av_frame_alloc());
        bool draining = false;
        while (true) {
            int received = avcodec_receive_frame(decoder.get(), frame.get());
            if (received == AVERROR_EOF) break;
            if (received == AVERROR(EAGAIN)) {
                if (draining) throw std::runtime_error("Decoder requested input after end of stream.");
                int read{};
                do {
                    av_packet_unref(packet.get());
                    read = av_read_frame(input, packet.get());
                    if (read == AVERROR_EOF) break;
                    audioError(read);
                } while (packet->stream_index != stream);
                draining = read == AVERROR_EOF;
                audioError(avcodec_send_packet(decoder.get(), draining ? nullptr : packet.get()));
                continue;
            }
            audioError(received);
            for (int output = 0; output < 2; ++output) {
                auto* converter = output ? analysis.get() : source.get();
                auto& samples = output ? clip.analysis : clip.samples;
                const auto channels = output ? 1u : clip.channels;
                const auto offset = samples.size();
                const auto capacity = swr_get_out_samples(converter, frame->nb_samples);
                samples.resize(offset + static_cast<std::size_t>(capacity) * channels);
                auto* destination = reinterpret_cast<std::uint8_t*>(samples.data() + offset);
                const int count = swr_convert(converter, &destination, capacity,
                    const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
                audioError(count);
                samples.resize(offset + static_cast<std::size_t>(count) * channels);
            }
            av_frame_unref(frame.get());
        }
        for (int output = 0; output < 2; ++output) {
            auto* converter = output ? analysis.get() : source.get();
            auto& samples = output ? clip.analysis : clip.samples;
            const auto channels = output ? 1u : clip.channels;
            int count{};
            do {
                const auto offset = samples.size();
                const auto capacity = swr_get_out_samples(converter, 0);
                samples.resize(offset + static_cast<std::size_t>(capacity) * channels);
                auto* destination = reinterpret_cast<std::uint8_t*>(samples.data() + offset);
                count = swr_convert(converter, &destination, capacity, nullptr, 0);
                audioError(count);
                samples.resize(offset + static_cast<std::size_t>(count) * channels);
            } while (count);
        }
        clip.frames = clip.samples.size() / clip.channels;
        return clip;
    }

    std::vector<float> AudioClip::resample(std::uint32_t rate, std::uint32_t outputChannels) const {
        AVChannelLayout inputLayout{}, outputLayout{};
        av_channel_layout_default(&inputLayout, channels);
        av_channel_layout_default(&outputLayout, outputChannels);
        SwrContext* raw{};
        const int allocated = swr_alloc_set_opts2(&raw, &outputLayout, AV_SAMPLE_FMT_FLT, rate,
            &inputLayout, AV_SAMPLE_FMT_FLT, sampleRate, 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        av_channel_layout_uninit(&outputLayout);
        audioError(allocated);
        const auto release = [](SwrContext* value) { swr_free(&value); };
        const std::unique_ptr<SwrContext, decltype(release)> converter(raw);
        audioError(swr_init(converter.get()));
        const int capacity = swr_get_out_samples(converter.get(), static_cast<int>(frames));
        std::vector<float> result(static_cast<std::size_t>(capacity) * outputChannels);
        auto* output = reinterpret_cast<std::uint8_t*>(result.data());
        const auto* input = reinterpret_cast<const std::uint8_t*>(samples.data());
        int count = swr_convert(converter.get(), &output, capacity, &input, static_cast<int>(frames));
        audioError(count);
        output += static_cast<std::size_t>(count) * outputChannels * sizeof(float);
        const int tail = swr_convert(converter.get(), &output, capacity - count, nullptr, 0);
        audioError(tail);
        result.resize(static_cast<std::size_t>(count + tail) * outputChannels);
        return result;
    }
}
