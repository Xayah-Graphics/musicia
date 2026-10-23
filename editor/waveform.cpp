module editor.waveform;

import std;
import musicia.audio;

namespace editor {
    std::vector<PeakLevel> waveform(const musicia::AudioClip& clip) {
        std::vector<PeakLevel> levels;
        levels.push_back({.block_frames = 64, .values = std::vector<Peak>((clip.frames + 63) / 64 * clip.channels)});
        for (std::uint64_t block = 0; block < (clip.frames + 63) / 64; ++block) {
            for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                Peak peak{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
                double square_sum = 0;
                for (auto frame = block * 64; frame < std::min((block + 1) * 64, clip.frames); ++frame) {
                    const auto sample = clip.samples[frame * clip.channels + channel];
                    peak.low          = std::min(peak.low, sample);
                    peak.high         = std::max(peak.high, sample);
                    square_sum += static_cast<double>(sample) * sample;
                }
                peak.mean_square                                            = static_cast<float>(square_sum / (std::min((block + 1) * 64, clip.frames) - block * 64));
                levels.front().values[block * clip.channels + channel] = peak;
            }
        }
        while (levels.back().values.size() > clip.channels) {
            const auto& previous = levels.back();
            const auto blocks    = previous.values.size() / clip.channels;
            PeakLevel next{.block_frames = previous.block_frames * 2, .values = std::vector<Peak>((blocks + 1) / 2 * clip.channels)};
            for (std::size_t block = 0; block < blocks; block += 2) {
                for (std::uint32_t channel = 0; channel < clip.channels; ++channel) {
                    const auto a = previous.values[block * clip.channels + channel];
                    if (block + 1 == blocks) {
                        next.values[block / 2 * clip.channels + channel] = a;
                        continue;
                    }
                    const auto b                                     = previous.values[(block + 1) * clip.channels + channel];
                    const auto a_frames                               = std::min(previous.block_frames, clip.frames - block * previous.block_frames);
                    const auto b_frames                               = std::min(previous.block_frames, clip.frames - (block + 1) * previous.block_frames);
                    next.values[block / 2 * clip.channels + channel] = {std::min(a.low, b.low), std::max(a.high, b.high), static_cast<float>((static_cast<double>(a.mean_square) * a_frames + static_cast<double>(b.mean_square) * b_frames) / (a_frames + b_frames))};
                }
            }
            levels.push_back(std::move(next));
        }
        return levels;
    }
}
