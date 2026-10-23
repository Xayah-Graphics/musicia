export module editor.waveform;

import std;
import musicia.audio;

export namespace editor {
    struct Peak {
        float low{}, high{}, meanSquare{};
    };
    struct PeakLevel {
        std::uint64_t blockFrames{};
        std::vector<Peak> values;
    };
    std::vector<PeakLevel> waveform(const musicia::AudioClip& clip);
}
