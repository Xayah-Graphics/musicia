module minidaw.document;
import std;

namespace minidaw {
    std::pair<double, double> sustain_range(double beat, double bar_length) {
        const double start = std::floor(beat / bar_length) * bar_length;
        return {start, start + bar_length - 1.0 / 16};
    }

    int Document::pitch(int degree) const {
        constexpr std::array major{0, 2, 4, 5, 7, 9, 11};
        constexpr std::array minor{0, 2, 3, 5, 7, 8, 10};
        const int octave = static_cast<int>(std::floor(degree / 7.0));
        return octave * 12 + sequence.tonic + (sequence.minor ? minor : major)[degree - octave * 7];
    }
    int Document::degree(int pitch) const {
        const int octave = static_cast<int>(std::floor((pitch - sequence.tonic) / 12.0));
        for (int i = 0; i < 7; ++i)
            if (this->pitch(octave * 7 + i) == pitch) return octave * 7 + i;
        throw std::runtime_error("The sequence contains an out-of-scale note.");
    }
    std::wstring Document::label(int degree) const {
        constexpr std::array names{L"C", L"C#", L"D", L"Eb", L"E", L"F", L"F#", L"G", L"Ab", L"A", L"Bb", L"B"};
        constexpr std::array solfege{L"Do", L"Re", L"Mi", L"Fa", L"Sol", L"La", L"Ti"};
        const int value = pitch(degree);
        return std::format(L"{}{}  ·  {}", names[value % 12], value / 12 - 1, solfege[(degree % 7 + (sequence.minor ? 5 : 0)) % 7]);
    }
    void Document::checkpoint() {
        undo.push_back(sequence);
        redo.clear();
    }
    void Document::history(bool forward) {
        auto& source      = forward ? redo : undo;
        auto& destination = forward ? undo : redo;
        if (source.empty()) return;
        destination.push_back(std::move(sequence));
        sequence = std::move(source.back());
        source.pop_back();
    }
    bool Document::change_key(int tonic, bool minor) {
        constexpr std::array major_scale{0, 2, 4, 5, 7, 9, 11};
        constexpr std::array minor_scale{0, 2, 3, 5, 7, 8, 10};
        auto notes = sequence.notes;
        for (auto& note : notes) {
            const int relative = degree(note.pitch);
            note.pitch = relative / 7 * 12 + tonic + (minor ? minor_scale : major_scale)[relative % 7];
            if (note.pitch < lowest_pitch || note.pitch > highest_pitch) return false;
        }
        checkpoint();
        sequence.tonic = tonic;
        sequence.minor = minor;
        sequence.notes = std::move(notes);
        return true;
    }
    void Document::midi(const std::filesystem::path& path, bool sustain) const {
        struct Event {
            int tick;
            int order;
            std::array<unsigned char, 3> bytes;
        };
        std::vector<Event> events;
        double end{};
        for (const auto& note : sequence.notes) {
            events.push_back({static_cast<int>(std::llround(note.beat * 960)), 3, {0x90, static_cast<unsigned char>(note.pitch), static_cast<unsigned char>(note.velocity)}});
            events.push_back({static_cast<int>(std::llround((note.beat + note.length) * 960)), 1, {0x80, static_cast<unsigned char>(note.pitch), 0}});
            end = std::max(end, note.beat + note.length);
        }
        events.push_back({0, 0, {0xb0, 64, 0}});
        if (sustain) {
            const double bar_length = sequence.beats * 4.0 / sequence.beat_unit;
            const int bars = static_cast<int>(std::ceil(end / bar_length));
            for (int bar = 0; bar < bars; ++bar) {
                const auto [down, up] = sustain_range(bar * bar_length, bar_length);
                events.push_back({static_cast<int>(std::llround(down * 960)), 2, {0xb0, 64, 127}});
                events.push_back({static_cast<int>(std::llround(up * 960)), 0, {0xb0, 64, 0}});
                end = std::max(end, up);
            }
        }
        events.push_back({static_cast<int>(std::llround(end * 960)), 0, {0xb0, 64, 0}});
        std::ranges::sort(events, [](const Event& a, const Event& b) { return std::tie(a.tick, a.order) < std::tie(b.tick, b.order); });
        const int tempo = 60000000 / sequence.bpm;
        const auto denominator = static_cast<unsigned char>(std::countr_zero(static_cast<unsigned>(sequence.beat_unit)));
        const auto clocks_per_beat = static_cast<unsigned char>(96 / sequence.beat_unit * (sequence.beat_unit == 8 && sequence.beats >= 6 ? 3 : 1));
        std::vector<unsigned char> track{0, 0xff, 0x51, 3, static_cast<unsigned char>(tempo >> 16), static_cast<unsigned char>(tempo >> 8), static_cast<unsigned char>(tempo), 0, 0xff, 0x58, 4, static_cast<unsigned char>(sequence.beats), denominator, clocks_per_beat, 8};
        int previous{};
        for (const auto& event : events) {
            unsigned value = event.tick - previous;
            std::array<unsigned char, 4> bytes{};
            int count = 1;
            bytes[3]  = value & 127;
            while ((value >>= 7) != 0) bytes[3 - count++] = static_cast<unsigned char>((value & 127) | 128);
            track.insert(track.end(), bytes.end() - count, bytes.end());
            track.insert(track.end(), event.bytes.begin(), event.bytes.end());
            previous = event.tick;
        }
        track.insert(track.end(), {0, 0xff, 0x2f, 0});
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        constexpr std::array<unsigned char, 18> header{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 3, 192, 'M', 'T', 'r', 'k'};
        stream.write(reinterpret_cast<const char*>(header.data()), header.size());
        const auto size = static_cast<std::uint32_t>(track.size());
        const auto big  = std::byteswap(size);
        stream.write(reinterpret_cast<const char*>(&big), 4);
        stream.write(reinterpret_cast<const char*>(track.data()), track.size());
    }
}
