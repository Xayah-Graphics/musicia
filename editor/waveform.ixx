export module editor.waveform;

import std;
import musicia.audio;

export namespace editor {
    struct Peak {
        float low{}, high{}, mean_square{};
    };
    struct PeakLevel {
        std::uint64_t block_frames{};
        std::vector<Peak> values;
    };
    std::vector<PeakLevel> waveform(const musicia::AudioClip& clip);
}
