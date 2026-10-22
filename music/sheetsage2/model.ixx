export module music.sheetsage2;

import std;
import music.analysis;
import music.sheetsage2.network;

export namespace music {
    struct SheetSage2 {
        explicit SheetSage2(const std::filesystem::path& directory);
        Analysis analyze(std::span<const float> samples, AnalysisProgress& progress, std::stop_token stop = {});

    private:
        sheetsage2::Network network;
        std::vector<std::string> structures, keys, chords, spellings;
        std::vector<int> durations;
    };
}

