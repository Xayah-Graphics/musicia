export module musicia.sheetsage2;

import std;
import musicia.analysis;
import musicia.sheetsage2.network;

export namespace musicia {
    struct SheetSage2 {
        explicit SheetSage2(const std::filesystem::path& directory);
        Analysis analyze(std::span<const float> samples, AnalysisProgress& progress, std::stop_token stop = {});

    private:
        sheetsage2::Network network;
        std::vector<std::string> structures, keys, chords, spellings;
        std::vector<int> durations;
    };
}

