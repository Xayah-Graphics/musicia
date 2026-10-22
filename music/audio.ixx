export module music.audio;

import std;

export namespace music {
    // The implementation includes FFmpeg's C headers outside a named module.
    extern "C++" struct AudioClip {
        std::filesystem::path path;
        std::uint32_t sampleRate{}, channels{};
        std::uint64_t frames{};
        std::vector<float> samples;
        std::vector<float> analysis;

        static AudioClip read(const std::filesystem::path& path);
        std::vector<float> resample(std::uint32_t rate, std::uint32_t outputChannels) const;
    };
}
