module musicia.analysis;

import std;

namespace musicia {
    void Analysis::save(const std::filesystem::path& directory) const {
        std::filesystem::create_directories(directory);
        std::ofstream json(directory / "analysis.json");
        json.exceptions(std::ios::badbit | std::ios::failbit);
        json << std::setprecision(17);
        json << "{\n\"duration\":" << duration << ",\"bpm\":" << bpm << ",\"elapsed\":" << elapsed << ",\n\"beats\":[";
        std::ofstream beat_file(directory / "beat.lab"), downbeat_file(directory / "downbeat.lab");
        beat_file.exceptions(std::ios::badbit | std::ios::failbit);
        downbeat_file.exceptions(std::ios::badbit | std::ios::failbit);
        beat_file << std::setprecision(17);
        downbeat_file << std::setprecision(17);
        for (std::size_t i = 0; i < beats.size(); ++i) {
            const auto& beat = beats[i];
            if (i) json << ',';
            json << std::format("[{:.17g},{},{},{}]", beat.time, beat.position, beat.numerator, beat.denominator);
            beat_file << std::format("{:.17g}\t{}\t{}\t{}\n", beat.time, beat.position, beat.numerator, beat.denominator);
            if (beat.position == 1) downbeat_file << beat.time << '\n';
        }
        json << ']';
        const std::array<std::pair<std::string_view, const std::vector<Interval>*>, 3> tracks{{{"chord", &chords}, {"key", &keys}, {"structure", &sections}}};
        for (const auto& [name, intervals] : tracks) {
            std::ofstream lab(directory / (std::string(name) + ".lab"));
            lab.exceptions(std::ios::badbit | std::ios::failbit);
            lab << std::setprecision(17);
            json << ",\n" << std::quoted(std::string(name)) << ":[";
            for (std::size_t i = 0; i < intervals->size(); ++i) {
                const auto& interval = (*intervals)[i];
                if (i) json << ',';
                json << '[' << interval.start << ',' << interval.end << ',' << std::quoted(interval.label) << ']';
                lab << interval.start << '\t' << interval.end << '\t' << interval.label << '\n';
            }
            json << ']';
        }
        std::ofstream melody(directory / "melody_full.lab");
        melody.exceptions(std::ios::badbit | std::ios::failbit);
        melody << std::setprecision(17);
        json << ",\n\"notes\":[";
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto& note = notes[i];
            if (i) json << ',';
            json << std::format("[{:.17g},{:.17g},{},{}]", note.start, note.end, note.pitch, note.track);
            melody << std::format("{:.17g}\t{:.17g}\t{}\t{}\n", note.start, note.end, note.pitch, note.track);
        }
        json << "],\n\"tokens\":[";
        for (std::size_t window = 0; window < tokens.size(); ++window) {
            if (window) json << ',';
            json << '[';
            for (std::size_t i = 0; i < tokens[window].size(); ++i) {
                if (i) json << ',';
                json << tokens[window][i];
            }
            json << ']';
        }
        json << "]\n}\n";
    }
}

