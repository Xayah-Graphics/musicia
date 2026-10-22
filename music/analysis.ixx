export module music.analysis;

import std;

export namespace music {
    struct Beat {
        double time{};
        int position{}, numerator{}, denominator{};
    };

    struct Interval {
        double start{}, end{};
        std::string label;
    };

    struct Note {
        double start{}, end{};
        int pitch{}, track{};
    };

    struct Analysis {
        double duration{}, bpm{}, elapsed{};
        std::vector<Beat> beats;
        std::vector<Interval> chords, keys, sections;
        std::vector<Note> notes;
        std::vector<std::vector<int>> tokens;

        void save(const std::filesystem::path& directory) const;
    };

    enum struct AnalysisStage { loading, encoding, decoding, complete };

    struct AnalysisProgress {
        std::atomic<AnalysisStage> stage{AnalysisStage::loading};
        std::atomic<int> window{}, windows{}, tokens{};
    };
}

