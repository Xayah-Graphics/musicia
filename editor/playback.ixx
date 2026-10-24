module;

#include <windows.h>

export module editor.playback;

import std;
import editor.timing;
import musicia.audio;

export namespace editor {
    inline constexpr float minimum_master_db{-60}, maximum_master_db{6};

    struct Playback {
        musicia::AudioClip clip;
        std::atomic<bool> playing{};
        std::atomic<bool> failed{};
        std::atomic<float> master_db{};
        std::array<std::atomic<float>, 2> output_peaks{};
        std::atomic<bool> overloaded{};
        unsigned sample_rate{}, period{};
        std::string error;
        HANDLE command{};

        Playback() = default;
        ~Playback();
        Playback(const Playback&) = delete;
        Playback& operator=(const Playback&) = delete;
        void open(musicia::AudioClip next);
        void close();
        void seek(std::uint64_t frame);
        std::uint64_t position() const;

    private:
        std::jthread thread;
        std::atomic<std::uint64_t> requested{std::numeric_limits<std::uint64_t>::max()};
        ui::PlaybackClock clock;
    };
}
