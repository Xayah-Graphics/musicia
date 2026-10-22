module;

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

module music.audio;

import std;

namespace music {
    AudioClip AudioClip::read(const std::filesystem::path& path) {
        AudioClip clip{.path = path};
        ma_decoder decoder{};
        const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
#ifdef _WIN32
        const auto opened = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
        const auto opened = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
        if (opened != MA_SUCCESS) throw std::runtime_error(std::format("Unable to open audio: {}", ma_result_description(opened)));
        try {
            clip.sampleRate = decoder.outputSampleRate;
            clip.channels   = decoder.outputChannels;
            ma_uint64 frames{};
            const auto lengthResult = ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
            if (lengthResult != MA_SUCCESS) throw std::runtime_error(std::format("Unable to read audio length: {}", ma_result_description(lengthResult)));
            clip.samples.resize(frames * clip.channels);
            ma_uint64 decoded{};
            const auto result = ma_decoder_read_pcm_frames(&decoder, clip.samples.data(), frames, &decoded);
            if (result != MA_SUCCESS && result != MA_AT_END) throw std::runtime_error(std::format("Unable to decode audio: {}", ma_result_description(result)));
            clip.frames = decoded;
            clip.samples.resize(decoded * clip.channels);
        } catch (...) {
            ma_decoder_uninit(&decoder);
            throw;
        }
        ma_decoder_uninit(&decoder);

        clip.peaks.push_back({.blockFrames = 64, .values = std::vector<Peak>((clip.frames + 63) / 64 * clip.channels)});
        for (std::uint64_t block = 0; block < (clip.frames + 63) / 64; ++block) {
            for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                Peak peak{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
                double squareSum = 0;
                for (auto frame = block * 64; frame < std::min((block + 1) * 64, clip.frames); ++frame) {
                    const auto sample = clip.samples[frame * clip.channels + channel];
                    peak.low          = std::min(peak.low, sample);
                    peak.high         = std::max(peak.high, sample);
                    squareSum += static_cast<double>(sample) * sample;
                }
                peak.meanSquare                                            = static_cast<float>(squareSum / (std::min((block + 1) * 64, clip.frames) - block * 64));
                clip.peaks.front().values[block * clip.channels + channel] = peak;
            }
        }
        while (clip.peaks.back().values.size() > clip.channels) {
            const auto& previous = clip.peaks.back();
            const auto blocks    = previous.values.size() / clip.channels;
            PeakLevel next{.blockFrames = previous.blockFrames * 2, .values = std::vector<Peak>((blocks + 1) / 2 * clip.channels)};
            for (std::size_t block = 0; block < blocks; block += 2) {
                for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                    const auto a = previous.values[block * clip.channels + channel];
                    if (block + 1 == blocks) {
                        next.values[block / 2 * clip.channels + channel] = a;
                        continue;
                    }
                    const auto b                                     = previous.values[(block + 1) * clip.channels + channel];
                    const auto aFrames                               = std::min(previous.blockFrames, clip.frames - block * previous.blockFrames);
                    const auto bFrames                               = std::min(previous.blockFrames, clip.frames - (block + 1) * previous.blockFrames);
                    next.values[block / 2 * clip.channels + channel] = {std::min(a.low, b.low), std::max(a.high, b.high), static_cast<float>((static_cast<double>(a.meanSquare) * aFrames + static_cast<double>(b.meanSquare) * bFrames) / (aFrames + bFrames))};
                }
            }
            clip.peaks.push_back(std::move(next));
        }
        return clip;
    }

    Audio::Audio() {
#ifdef _WIN32
        constexpr auto backend = ma_backend_wasapi;
#else
        constexpr auto backend = ma_backend_pulseaudio;
#endif
        const auto result = ma_context_init(&backend, 1, nullptr, &context);
        if (result != MA_SUCCESS) throw std::runtime_error(std::format("Unable to initialize audio output: {}", ma_result_description(result)));
    }

    Audio::~Audio() {
        close();
        ma_context_uninit(&context);
    }

    void Audio::open(AudioClip next) {
        close();
        clip                            = std::move(next);
        readFrame                       = 0;
        renderedFrame                   = 0;
        startedFrame                    = 0;
        callbackActive                  = false;
        wasActive                       = false;
        requestedFrame                  = std::numeric_limits<std::uint64_t>::max();
        gain                            = 0;
        auto config                     = ma_device_config_init(ma_device_type_playback);
        config.playback.format          = ma_format_f32;
        config.playback.channels        = clip.channels;
        config.sampleRate               = clip.sampleRate;
        config.periodSizeInMilliseconds = 10;
        config.performanceProfile       = ma_performance_profile_low_latency;
        config.dataCallback             = output;
        config.pUserData                = this;
        const auto initialized          = ma_device_init(&context, &config, &device);
        if (initialized != MA_SUCCESS) throw std::runtime_error(std::format("Unable to open audio output: {}", ma_result_description(initialized)));
        deviceOpen         = true;
        latencyFrames      = static_cast<std::uint64_t>(device.playback.internalPeriodSizeInFrames) * device.playback.internalPeriods * clip.sampleRate / device.playback.internalSampleRate;
        const auto started = ma_device_start(&device);
        if (started != MA_SUCCESS) throw std::runtime_error(std::format("Unable to start audio output: {}", ma_result_description(started)));
    }

    void Audio::close() {
        playing = false;
        if (deviceOpen) ma_device_uninit(&device);
        deviceOpen     = false;
        callbackActive = false;
    }

    void Audio::seek(std::uint64_t frame) {
        requestedFrame.store(frame, std::memory_order_release);
    }

    std::uint64_t Audio::position() const {
        const auto requested = requestedFrame.load(std::memory_order_acquire);
        if (requested != std::numeric_limits<std::uint64_t>::max()) return requested;
        const auto rendered = renderedFrame.load(std::memory_order_acquire);
        if (!playing.load(std::memory_order_relaxed) || !callbackActive.load(std::memory_order_acquire)) return rendered;
        const auto now     = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto elapsed = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now - callbackTime.load(std::memory_order_relaxed))) * clip.sampleRate / 1'000'000'000;
        return std::max(startedFrame.load(std::memory_order_relaxed), std::min(rendered, rendered - std::min(rendered, latencyFrames) + elapsed));
    }

    void Audio::output(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto& audio          = *static_cast<Audio*>(device->pUserData);
        auto* samples        = static_cast<float*>(output);
        const auto requested = audio.requestedFrame.exchange(std::numeric_limits<std::uint64_t>::max(), std::memory_order_acq_rel);
        if (requested != std::numeric_limits<std::uint64_t>::max()) {
            audio.readFrame = requested;
            audio.gain      = 0;
        }
        const auto active = audio.playing.load(std::memory_order_relaxed);
        if (active && (!audio.wasActive || requested != std::numeric_limits<std::uint64_t>::max())) audio.startedFrame.store(audio.readFrame, std::memory_order_relaxed);
        const float step = 1.0f / (audio.clip.sampleRate * 0.003f);
        for (ma_uint32 frame = 0; frame < frames; ++frame) {
            const bool available = audio.readFrame < audio.clip.frames;
            audio.gain           = std::clamp(audio.gain + (active && available ? step : -step), 0.0f, 1.0f);
            for (std::uint32_t channel = 0; channel < audio.clip.channels; ++channel) samples[frame * audio.clip.channels + channel] = available ? audio.clip.samples[audio.readFrame * audio.clip.channels + channel] * audio.gain : 0.0f;
            if (available && (active || audio.gain > 0)) ++audio.readFrame;
        }
        if (audio.readFrame == audio.clip.frames) audio.playing.store(false, std::memory_order_relaxed);
        audio.callbackTime.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        audio.renderedFrame.store(audio.readFrame, std::memory_order_release);
        audio.callbackActive.store(active, std::memory_order_release);
        audio.wasActive = active;
    }
}
