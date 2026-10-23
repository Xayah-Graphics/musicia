module musicia.sheetsage2;

import std;
import musicia.analysis;
import musicia.sheetsage2.network;

namespace musicia {
    struct Event {
        int step{}, global_step{};
        double time{};
        std::array<std::vector<int>, 6> fields;
        std::vector<Note> notes;
    };

    struct Grammar {
        bool shift{true};
        int run{}, payload{}, field{-1}, incomplete{};

        bool allows(int token) const {
            if (token == 2) return payload > 0;
            if (token >= 260 && token < 517) return (payload > 0 || shift) && run < 4;
            if (incomplete == 1) return token >= 30709 && token < 30965;
            if (incomplete == 2) return token >= 31398 && token < 31678;
            if (token >= 517 && token < 30517) return field < 0;
            if (token >= 30517 && token < 30965) return field < 1;
            if (token >= 30965 && token < 30988) return field < 2;
            if (token >= 30988 && token < 31012) return field < 3;
            if (token >= 31037 && token < 31398) return field < 4;
            return token >= 31398 && token < 31654 && field <= 5;
        }

        void update(int token) {
            if (token == 2) return;
            if (token >= 260 && token < 517) {
                if (!shift && payload) {
                    payload = 0;
                    field = -1;
                    incomplete = 0;
                }
                shift = true;
                ++run;
                return;
            }
            shift = false;
            run = 0;
            ++payload;
            incomplete = 0;
            if (token < 30517) field = 0;
            else if (token < 30709) { field = 1; incomplete = 1; }
            else if (token < 30965) field = 1;
            else if (token < 30988) field = 2;
            else if (token < 31012) field = 3;
            else if (token < 31398) field = 4;
            else if (token < 31654) { field = 5; incomplete = 2; }
            else field = 5;
        }
    };

    SheetSage2::SheetSage2(const std::filesystem::path& directory) : network(directory) {
        structures = {"silence", "intro", "outro", "verse", "chorus", "bridge", "pre-chorus", "post-chorus", "interlude", "fade-out", "loop", "rap", "preshot", "irregular", "instrumental", "intro and verse", "pre-chorus and chorus", "verse and pre-chorus", "solo", "theme", "development", "variation", "pre-outro"};
        const std::array<std::string, 12> major{"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        const std::array<std::string, 12> minor{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};
        const std::array<std::string, 12> chromatic{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const std::array<std::string, 15> qualities{"maj", "min", "dim", "aug", "maj7", "min7", "7", "hdim7", "dim7", "minmaj7", "sus2", "sus4", "sus4(b7)", "maj6", "min6"};
        const std::array<std::vector<int>, 15> fifths{{{0, 4, 1}, {0, -3, 1}, {0, -3, -6}, {0, 4, 8}, {0, 4, 1, 5}, {0, -3, 1, -2}, {0, 4, 1, -2}, {0, -3, -6, -2}, {0, -3, -6, -9}, {0, -3, 1, 5}, {0, 2, 1}, {0, -1, 1}, {0, -1, 1, -2}, {0, 4, 1, 3}, {0, -3, 1, 3}}};
        constexpr std::array<int, 7> natural{0, 2, 4, 5, 7, 9, 11}, circle{1, 3, 5, 0, 2, 4, 6};
        constexpr std::array<int, 12> major_fifths{1, -4, 3, -2, 5, 0, 7, 2, -3, 4, -1, 6};
        constexpr std::string_view letters = "CDEFGAB";
        for (const auto& root : major) keys.push_back(root + ":major");
        for (const auto& root : minor) keys.push_back(root + ":minor");
        durations = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096};
        chords.push_back("N");
        for (int tonal = 0; tonal < 24; ++tonal) {
            spellings.push_back("N");
            const int key_position = major_fifths[(tonal % 12 + (tonal >= 12 ? 3 : 0)) % 12];
            for (int quality = 0; quality < 15; ++quality) {
                std::vector<std::string> inversions;
                if (quality == 0) inversions = {"/2", "/3", "/5"};
                else if (quality == 1) inversions = {"/2", "/b3", "/5"};
                else if (quality == 4) inversions = {"/3", "/5", "/7"};
                else if (quality == 5) inversions = {"/b3", "/5", "/b7"};
                else if (quality == 6) inversions = {"/3", "/5", "/b7"};
                inversions.emplace_back();
                for (int root = 0; root < 12; ++root) {
                    int best = 0, lowest = std::numeric_limits<int>::max();
                    for (int letter = 0; letter < 7; ++letter) {
                        const int accidental = (root - natural[letter] + 18) % 12 - 6;
                        const int position = circle[letter] + accidental * 7;
                        int score = std::max(0, std::abs(position - key_position - 2) - 3);
                        for (const int tone : fifths[quality]) score += std::max(0, std::abs(position + tone - key_position - 2) - 3);
                        if (score < lowest) { lowest = score; best = letter; }
                    }
                    const int accidental = (root - natural[best] + 18) % 12 - 6;
                    const auto spelling = std::string(1, letters[best]) + std::string(std::abs(accidental), accidental < 0 ? 'b' : '#');
                    for (const auto& inversion : inversions) {
                        if (!tonal) chords.push_back(chromatic[root] + ":" + qualities[quality] + inversion);
                        spellings.push_back(spelling + ":" + qualities[quality] + inversion);
                    }
                }
            }
        }
    }

    Analysis SheetSage2::analyze(std::span<const float> samples, AnalysisProgress& progress, std::stop_token stop) {
        const auto started = std::chrono::steady_clock::now();
        Analysis result{.duration = samples.size() / 24000.0};
        std::vector<Event> stitched;
        struct Window { double start{}, accepted{}, end{}; bool last{}; };
        std::vector<Window> windows;
        double start = 0, accepted = 0;
        while (true) {
            const bool last = start + 300 >= result.duration - 1e-6;
            const double end = last ? result.duration : start + 200;
            windows.push_back({start, accepted, end, last});
            if (last) break;
            accepted = end;
            start = std::min(start + 100, result.duration - 300);
        }
        progress.windows = static_cast<int>(windows.size());
        for (std::size_t w = 0; w < windows.size(); ++w) {
            if (stop.stop_requested()) throw std::runtime_error("Analysis cancelled.");
            const auto window = windows[w];
            progress.window = static_cast<int>(w + 1);
            progress.tokens = 0;
            std::vector<int> tokens{1, 4, 5, 6, 7, 9, 11, 3};
            int base = 0;
            if (w) {
                std::vector<Event> prefix;
                for (const auto& event : stitched)
                    if (event.time >= window.start - 1e-4 && event.time < window.accepted - 1e-4) prefix.push_back(event);
                std::ranges::sort(prefix, [](const Event& a, const Event& b) { return std::tie(a.global_step, a.time) < std::tie(b.global_step, b.time); });
                const auto first = std::ranges::find_if(prefix, [](const Event& event) { return !event.fields[0].empty() || !event.fields[1].empty(); });
                prefix.erase(prefix.begin(), first);
                if (prefix.empty()) throw std::runtime_error("No musical context for the next analysis window.");
                base = prefix.front().global_step;
                std::array<std::vector<int>, 6> context;
                int meter = -1;
                for (const auto& event : stitched) {
                    if (event.time > prefix.front().time + 1e-6) continue;
                    for (int field : {2, 3, 4}) if (!event.fields[field].empty()) context[field] = event.fields[field];
                    for (const auto token : event.fields[1]) if (token >= 30517 && token < 30709) meter = token;
                }
                for (int field : {2, 3, 4}) if (prefix.front().fields[field].empty()) prefix.front().fields[field] = context[field];
                auto& rhythm = prefix.front().fields[1];
                if (!rhythm.empty() && rhythm.front() >= 30709 && meter >= 0) rhythm.insert(rhythm.begin(), meter);
                int previous = 0;
                for (auto& event : prefix) {
                    event.step = std::max(0, event.global_step - base);
                    if (!event.fields[0].empty()) event.fields[0] = {517 + std::clamp(static_cast<int>(std::nearbyint((event.time - window.start) * 100)), 0, 29999)};
                    int shift = event.step - previous;
                    while (shift > 256) { tokens.push_back(516); shift -= 256; }
                    tokens.push_back(260 + shift);
                    previous = event.step;
                    for (const auto& field : event.fields) tokens.insert(tokens.end(), field.begin(), field.end());
                }
            }
            Grammar grammar;
            for (std::size_t i = 8; i < tokens.size(); ++i) grammar.update(tokens[i]);
            const auto offset = static_cast<std::size_t>(std::nearbyint(window.start * 24000));
            const int count = static_cast<int>(std::min<std::size_t>(7200000, samples.size() - offset));
            progress.stage = AnalysisStage::encoding;
            network.encode(samples.data() + offset, count);
            if (stop.stop_requested()) throw std::runtime_error("Analysis cancelled.");
            progress.stage = AnalysisStage::decoding;
            network.decode(tokens.data(), static_cast<int>(tokens.size()));
            const double stop_time = window.last ? std::min(300.0, result.duration - window.start) : 200.0;
            while (tokens.size() < 5120) {
                if (stop.stop_requested()) throw std::runtime_error("Analysis cancelled.");
                int selected = -1;
                float largest = -std::numeric_limits<float>::infinity();
                for (int token = 0; token < 31678; ++token) {
                    if (grammar.allows(token) && network.host_logits[token] > largest) {
                        largest = network.host_logits[token];
                        selected = token;
                    }
                }
                if (selected < 0) throw std::runtime_error("Decoder produced no finite musical token.");
                tokens.push_back(selected);
                grammar.update(selected);
                progress.tokens = static_cast<int>(tokens.size());
                if (selected == 2) break;
                if (selected >= 517 && selected < 30517 && (selected - 517) / 100.0 >= stop_time) {
                    tokens.push_back(2);
                    break;
                }
                network.decode(&selected, 1);
            }
            if (tokens.back() != 2) throw std::runtime_error("SheetSage2 exhausted its token context.");
            result.tokens.push_back(tokens);
            std::vector<Event> events;
            int step = 0;
            for (std::size_t i = 8; i < tokens.size() && tokens[i] != 2;) {
                while (tokens[i] >= 260 && tokens[i] < 517) step += tokens[i++] - 260;
                Event event{.step = step, .global_step = base + step};
                while (i < tokens.size() && tokens[i] != 2 && !(tokens[i] >= 260 && tokens[i] < 517)) {
                    const int token = tokens[i++];
                    const int field = token < 30517 ? 0 : token < 30965 ? 1 : token < 30988 ? 2 : token < 31012 ? 3 : token < 31398 ? 4 : 5;
                    event.fields[field].push_back(token);
                }
                events.push_back(std::move(event));
            }
            std::map<int, double> anchors;
            for (const auto& event : events) if (!event.fields[0].empty()) anchors[event.step] = (event.fields[0].front() - 517) / 100.0;
            if (anchors.size() < 2) throw std::runtime_error("SheetSage2 produced no usable musical time map.");
            std::vector<double> periods;
            for (auto i = std::next(anchors.begin()); i != anchors.end(); ++i) periods.push_back((i->second - std::prev(i)->second) / std::max(1, i->first - std::prev(i)->first));
            std::ranges::sort(periods);
            const double period = (periods[(periods.size() - 1) / 2] + periods[periods.size() / 2]) * .5;
            if (period <= 0) throw std::runtime_error("SheetSage2 produced a non-increasing musical time map.");
            const auto time_at = [&anchors, period](int position) {
                const auto next = anchors.lower_bound(position);
                if (next == anchors.begin()) return std::clamp(next->second + (position - next->first) * period, 0.0, 300.0);
                if (next == anchors.end()) return std::clamp(std::prev(next)->second + (position - std::prev(next)->first) * period, 0.0, 300.0);
                const auto previous = std::prev(next);
                return previous->second + static_cast<double>(position - previous->first) / (next->first - previous->first) * (next->second - previous->second);
            };
            for (auto& event : events) {
                event.time = window.start + time_at(event.step);
                if (event.time < window.accepted - 1e-4 || event.time >= window.end - 1e-4 || event.time >= result.duration - 1e-4) continue;
                const auto& melody = event.fields[5];
                for (std::size_t i = 0; i < melody.size();) {
                    const int pitch = melody[i++] - 31398;
                    const int duration = i < melody.size() && melody[i] >= 31654 ? melody[i++] - 31654 : 0;
                    event.notes.push_back({event.time, std::min(result.duration, std::max(event.time + .04, window.start + time_at(event.step + durations[duration]))), pitch % 128, pitch / 128});
                }
                stitched.push_back(std::move(event));
            }
        }
        std::ranges::sort(stitched, [](const Event& a, const Event& b) { return std::tie(a.time, a.global_step) < std::tie(b.time, b.global_step); });
        int numerator = 0, denominator = 0;
        for (const auto& event : stitched) {
            for (const int token : event.fields[1]) {
                if (token < 30709) {
                    const int meter = token - 30517;
                    numerator = meter / 6 + 1;
                    denominator = 1 << (meter % 6);
                } else {
                    const int position = (token - 30709) * denominator / 8 + 1;
                    result.beats.push_back({event.time, position, numerator, denominator});
                }
            }
            result.notes.insert(result.notes.end(), event.notes.begin(), event.notes.end());
            if (!event.fields[2].empty()) result.sections.push_back({event.time, result.duration, structures[event.fields[2][0] - 30965]});
            if (!event.fields[3].empty()) result.keys.push_back({event.time, result.duration, keys[event.fields[3][0] - 30988]});
            if (!event.fields[4].empty()) result.chords.push_back({event.time, result.duration, chords[event.fields[4][0] - 31037]});
        }
        for (auto* intervals : {&result.chords, &result.keys, &result.sections}) {
            for (std::size_t i = 0; i + 1 < intervals->size(); ++i) (*intervals)[i].end = (*intervals)[i + 1].start;
            std::erase_if(*intervals, [](const Interval& interval) { return interval.end <= interval.start; });
        }
        for (auto& chord : result.chords) {
            const double middle = (chord.start + chord.end) * .5;
            const auto key = std::ranges::lower_bound(result.keys, middle, {}, &Interval::end);
            const auto key_index = std::ranges::find(keys, (key == result.keys.end() ? result.keys.back() : *key).label) - keys.begin();
            const auto chord_index = std::ranges::find(chords, chord.label) - chords.begin();
            chord.label = spellings[key_index * 361 + chord_index];
        }
        std::ranges::sort(result.notes, [](const Note& a, const Note& b) { return std::tie(a.start, a.end, a.pitch, a.track) < std::tie(b.start, b.end, b.pitch, b.track); });
        std::vector<double> tempi;
        for (std::size_t i = 1; i < result.beats.size(); ++i) tempi.push_back(240.0 / (result.beats[i - 1].denominator * (result.beats[i].time - result.beats[i - 1].time)));
        std::ranges::sort(tempi);
        result.bpm = (tempi[(tempi.size() - 1) / 2] + tempi[tempi.size() / 2]) * .5;
        result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        progress.stage = AnalysisStage::complete;
        return result;
    }
}

