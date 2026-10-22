export module editor.timeline;

import std;

export namespace editor {
    struct Timeline {
        double first{};
        double count{1};

        double frame(double fraction) const {
            return first + count * fraction;
        }

        double fraction(double frame) const {
            return (frame - first) / count;
        }

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

        void reveal(double frame, std::uint64_t total) {
            if (frame < first || frame >= first + count) first = std::clamp(frame - count / 2, 0.0, static_cast<double>(total) - count);
        }
    };
}
