export module musicia.audio;

import std;

export namespace musicia {
    extern "C++" struct AudioClip {
        std::filesystem::path path;
        std::uint32_t sample_rate{}, channels{};
        std::uint64_t frames{};
        std::vector<float> samples;
        std::vector<float> analysis;

        static AudioClip read(const std::filesystem::path& path);
        std::vector<float> resample(std::uint32_t rate, std::uint32_t output_channels) const;
    };
}
