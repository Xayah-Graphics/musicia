module;

#include <miniaudio.h>

export module music.audio;

import std;
export import music.timeline;

export namespace music {
    struct AudioClip {
        std::filesystem::path path;
        std::uint32_t sampleRate{};
        std::uint32_t channels{};
        std::uint64_t frames{};
        std::vector<float> samples;
        std::vector<PeakLevel> peaks;

        static AudioClip read(const std::filesystem::path& path);
    };

    struct Audio {
        AudioClip clip;
        std::atomic<bool> playing{false};

        Audio();
        ~Audio();
        Audio(const Audio&)            = delete;
        Audio& operator=(const Audio&) = delete;
        void open(AudioClip next);
        void close();
        void seek(std::uint64_t frame);
        std::uint64_t position() const;

    private:
        ma_context context{};
        ma_device device{};
        bool deviceOpen{};
        std::atomic<std::uint64_t> requestedFrame{std::numeric_limits<std::uint64_t>::max()};
        std::atomic<std::uint64_t> renderedFrame{};
        std::atomic<std::int64_t> callbackTime{};
        std::atomic<std::uint64_t> startedFrame{};
        std::atomic<bool> callbackActive{false};
        std::uint64_t readFrame{};
        std::uint64_t latencyFrames{};
        float gain{};
        bool wasActive{};

        static void output(ma_device* device, void* output, const void* input, ma_uint32 frames);
    };
}
