export module minidaw.document;
import std;

export namespace minidaw {
    inline constexpr int lowest_pitch = 21, highest_pitch = 108;
    enum struct Voice { treble, bass };
    struct Note {
        std::uint64_t id{};
        double beat{}, length{1};
        int pitch{60}, velocity{96};
        Voice voice{Voice::treble};
        bool operator==(const Note&) const = default;
    };
    struct Sequence {
        std::vector<Note> notes;
        int tonic{}, bpm{120}, beats{4}, beat_unit{4};
        bool minor{};
        std::uint64_t next_id{1};
        bool operator==(const Sequence&) const = default;
    };
    std::pair<double, double> sustain_range(double beat, double bar_length);

    struct Document {
        Sequence sequence;
        std::vector<Sequence> undo, redo;
        double first_beat{}, pixels_per_beat{84}, row_height{38}, top_degree{42};
        int pitch(int degree) const;
        int degree(int pitch) const;
        std::wstring label(int degree) const;
        void checkpoint();
        void history(bool forward);
        bool change_key(int tonic, bool minor);
        void midi(const std::filesystem::path& path, bool sustain) const;
    };
}
