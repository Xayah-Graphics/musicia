module;

#include <windows.h>

export module editor.playback;

import std;
import music.audio;

export namespace editor {
    struct Playback {
        music::AudioClip clip;
        std::atomic<bool> playing{};
        std::atomic<bool> failed{};
        std::string error;
        HANDLE command{};

        Playback() = default;
        ~Playback();
        Playback(const Playback&) = delete;
        Playback& operator=(const Playback&) = delete;
        void open(music::AudioClip next);
        void close();
        void seek(std::uint64_t frame);
        std::uint64_t position() const;

    private:
        std::jthread thread;
        std::atomic<std::uint64_t> requested{std::numeric_limits<std::uint64_t>::max()};
        std::atomic<std::uint64_t> cursor{};
    };
}
