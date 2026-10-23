module editor.waveform;

import std;
import musicia.audio;

namespace editor {
    std::vector<PeakLevel> waveform(const musicia::AudioClip& clip) {
        std::vector<PeakLevel> levels;
        levels.push_back({.blockFrames = 64, .values = std::vector<Peak>((clip.frames + 63) / 64 * clip.channels)});
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
                levels.front().values[block * clip.channels + channel] = peak;
            }
        }
        while (levels.back().values.size() > clip.channels) {
            const auto& previous = levels.back();
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
            levels.push_back(std::move(next));
        }
        return levels;
    }
}
