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
    static void audio_error(int status) {
        if (status >= 0) return;
        char detail[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(status, detail, sizeof(detail));
        throw std::runtime_error(detail);
    }

    AudioClip AudioClip::read(const std::filesystem::path& path) {
        AudioClip clip{.path = path};
        const auto utf8 = path.u8string();
        AVFormatContext* input{};
        audio_error(avformat_open_input(&input, reinterpret_cast<const char*>(utf8.c_str()), nullptr, nullptr));
        const auto release_input = [](AVFormatContext* value) { avformat_close_input(&value); };
        const std::unique_ptr<AVFormatContext, decltype(release_input)> input_owner(input);
        audio_error(avformat_find_stream_info(input, nullptr));
        const AVCodec* codec{};
        const int stream = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        audio_error(stream);
        const auto release_decoder = [](AVCodecContext* value) { avcodec_free_context(&value); };
        const std::unique_ptr<AVCodecContext, decltype(release_decoder)> decoder(avcodec_alloc_context3(codec));
        audio_error(avcodec_parameters_to_context(decoder.get(), input->streams[stream]->codecpar));
        audio_error(avcodec_open2(decoder.get(), codec, nullptr));
        clip.sample_rate = decoder->sample_rate;
        clip.channels = decoder->ch_layout.nb_channels;
        const auto release_resampler = [](SwrContext* value) { swr_free(&value); };
        SwrContext* raw_source{};
        audio_error(swr_alloc_set_opts2(&raw_source, &decoder->ch_layout, AV_SAMPLE_FMT_FLT, clip.sample_rate,
            &decoder->ch_layout, decoder->sample_fmt, clip.sample_rate, 0, nullptr));
        const std::unique_ptr<SwrContext, decltype(release_resampler)> source(raw_source);
        audio_error(swr_init(source.get()));
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        SwrContext* raw_analysis{};
        audio_error(swr_alloc_set_opts2(&raw_analysis, &mono, AV_SAMPLE_FMT_FLT, 24000,
            &decoder->ch_layout, decoder->sample_fmt, clip.sample_rate, 0, nullptr));
        const std::unique_ptr<SwrContext, decltype(release_resampler)> analysis(raw_analysis);
        audio_error(swr_init(analysis.get()));
        const auto release_packet = [](AVPacket* value) { av_packet_free(&value); };
        const auto release_frame = [](AVFrame* value) { av_frame_free(&value); };
        const std::unique_ptr<AVPacket, decltype(release_packet)> packet(av_packet_alloc());
        const std::unique_ptr<AVFrame, decltype(release_frame)> frame(av_frame_alloc());
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
                    audio_error(read);
                } while (packet->stream_index != stream);
                draining = read == AVERROR_EOF;
                audio_error(avcodec_send_packet(decoder.get(), draining ? nullptr : packet.get()));
                continue;
            }
            audio_error(received);
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
                audio_error(count);
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
                audio_error(count);
                samples.resize(offset + static_cast<std::size_t>(count) * channels);
            } while (count);
        }
        clip.frames = clip.samples.size() / clip.channels;
        return clip;
    }

    std::vector<float> AudioClip::resample(std::uint32_t rate, std::uint32_t output_channels) const {
        AVChannelLayout input_layout{}, output_layout{};
        av_channel_layout_default(&input_layout, channels);
        av_channel_layout_default(&output_layout, output_channels);
        SwrContext* raw{};
        const int allocated = swr_alloc_set_opts2(&raw, &output_layout, AV_SAMPLE_FMT_FLT, rate,
            &input_layout, AV_SAMPLE_FMT_FLT, sample_rate, 0, nullptr);
        av_channel_layout_uninit(&input_layout);
        av_channel_layout_uninit(&output_layout);
        audio_error(allocated);
        const auto release = [](SwrContext* value) { swr_free(&value); };
        const std::unique_ptr<SwrContext, decltype(release)> converter(raw);
        audio_error(swr_init(converter.get()));
        const int capacity = swr_get_out_samples(converter.get(), static_cast<int>(frames));
        std::vector<float> result(static_cast<std::size_t>(capacity) * output_channels);
        auto* output = reinterpret_cast<std::uint8_t*>(result.data());
        const auto* input = reinterpret_cast<const std::uint8_t*>(samples.data());
        int count = swr_convert(converter.get(), &output, capacity, &input, static_cast<int>(frames));
        audio_error(count);
        output += static_cast<std::size_t>(count) * output_channels * sizeof(float);
        const int tail = swr_convert(converter.get(), &output, capacity - count, nullptr, 0);
        audio_error(tail);
        result.resize(static_cast<std::size_t>(count + tail) * output_channels);
        return result;
    }
}
