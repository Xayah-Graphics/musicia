export module music.timeline;

import std;

export namespace music {
    struct Peak {
        float low{};
        float high{};
        float meanSquare{};
    };

    struct PeakLevel {
        std::uint64_t blockFrames{};
        std::vector<Peak> values;
    };

    struct Timeline {
        double first{};
        double count{1};

        void fit(std::uint64_t frames) {
            first = 0;
            count = static_cast<double>(frames);
        }

        void zoom(double factor, double anchor, std::uint64_t frames) {
            const double fixed = first + count * anchor;
            count              = std::clamp(count * factor, std::min(32.0, static_cast<double>(frames)), static_cast<double>(frames));
            first              = std::clamp(fixed - count * anchor, 0.0, static_cast<double>(frames) - count);
        }

        void pan(double frames, std::uint64_t total) {
            first = std::clamp(first + frames, 0.0, static_cast<double>(total) - count);
        }
    };
}
