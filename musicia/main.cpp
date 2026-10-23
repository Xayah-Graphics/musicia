import std;
import musicia.audio;
import musicia.analysis;
import musicia.sheetsage2;

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        const auto clip = musicia::AudioClip::read(std::filesystem::path(argv[1]));
        std::println("Audio: {} Hz, {} channels, {:.3f} seconds", clip.sampleRate, clip.channels, static_cast<double>(clip.frames) / clip.sampleRate);
        const auto directory = argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path(MUSICIA_MODEL_DIRECTORY);
        musicia::SheetSage2 model(directory);
        musicia::AnalysisProgress progress;
        const auto result = model.analyze(clip.analysis, progress);
        result.save(std::filesystem::path(argv[2]));
        std::println("Analysis: {:.3f} s; {:.3f} BPM; {} beats; {} chord intervals; {} notes", result.elapsed, result.bpm, result.beats.size(), result.chords.size(), result.notes.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
