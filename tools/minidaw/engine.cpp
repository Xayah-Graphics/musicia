module;
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

module minidaw.engine;
import std;
import minidaw.document;

namespace minidaw {
    struct PlaybackSpan {
        std::int64_t first{}, last{};
        double beat{}, step{};
    };

    Engine::Engine(HWND owner, std::filesystem::path directory)
        : owner(owner),
          directory(std::move(directory)) {}
    Engine::~Engine() {
        stop();
        if (view) {
            view->removed();
            view->setFrame(nullptr);
            view = nullptr;
        }
        if (plugin_window) DestroyWindow(plugin_window);
        if (component && ready) component->setActive(false);
        if (controller) controller->setComponentHandler(nullptr);
        processor  = nullptr;
        controller = nullptr;
        component  = nullptr;
        provider.reset();
        module.reset();
        if (audio_event) CloseHandle(audio_event);
        if (stop_event) CloseHandle(stop_event);
    }
    void Engine::initialize() {
        const auto began = std::chrono::steady_clock::now();
        std::string failure;
        module = VST3::Hosting::Module::create("C:/Program Files/Common Files/VST3/Kontakt 8.vst3", failure);
        if (!module) throw std::runtime_error(failure);
        module->getFactory().setHostContext(static_cast<Steinberg::Vst::IHostApplication*>(this));
        Steinberg::Vst::PluginContextFactory::instance().setPluginContext(static_cast<Steinberg::Vst::IHostApplication*>(this));
        const auto classes = module->getFactory().classInfos();
        const auto found   = std::ranges::find_if(classes, [](const auto& info) { return info.category() == kVstAudioEffectClass; });
        if (found == classes.end()) throw std::runtime_error("Kontakt does not expose an audio component.");
        provider = std::make_unique<Steinberg::Vst::PlugProvider>(module->getFactory(), *found, true);
        if (!provider->initialize()) throw std::runtime_error("Kontakt initialization failed.");
        component  = provider->getComponentPtr();
        controller = provider->getControllerPtr();
        processor  = Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor>(component);
        controller->setComponentHandler(this);
        for (const auto suffix : {"component", "controller"}) {
            const auto path = directory / (std::string("piano.") + suffix);
            if (!std::filesystem::exists(path)) continue;
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            input.exceptions(std::ios::badbit | std::ios::failbit);
            std::vector<char> bytes(static_cast<std::size_t>(input.tellg()));
            input.seekg(0);
            input.read(bytes.data(), bytes.size());
            Steinberg::MemoryStream state(bytes.data(), bytes.size());
            if (std::string_view(suffix) == "component") {
                if (component->setState(&state) != Steinberg::kResultOk) throw std::runtime_error("Kontakt could not restore its piano state.");
                state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                controller->setComponentState(&state);
            } else controller->setState(&state);
        }
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        Microsoft::WRL::ComPtr<IMMDevice> endpoint;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
        if (SUCCEEDED(result)) result = endpoint->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client);
        if (FAILED(result)) throw std::runtime_error(std::format("WASAPI initialization failed: {:08x}", static_cast<unsigned>(result)));
        WAVEFORMATEX* format{};
        result = client->GetMixFormat(&format);
        if (FAILED(result)) throw std::runtime_error("Cannot read the Windows audio format.");
        const bool floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (!floating || format->wBitsPerSample != 32 || format->nChannels < 2) {
            CoTaskMemFree(format);
            throw std::runtime_error("MiniDAW requires a stereo-or-wider float32 Windows audio endpoint.");
        }
        sample_rate = format->nSamplesPerSec;
        channels   = format->nChannels;
        UINT32 default_period{}, fundamental{}, minimum{}, maximum{};
        result = client->GetSharedModeEnginePeriod(format, &default_period, &fundamental, &minimum, &maximum);
        if (FAILED(result)) {
            CoTaskMemFree(format);
            throw std::runtime_error("Cannot query the audio device period.");
        }
        period = std::clamp(((256u + fundamental - 1) / fundamental) * fundamental, minimum, maximum);
        if (SUCCEEDED(result)) result = client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, format, nullptr);
        CoTaskMemFree(format);
        if (FAILED(result)) throw std::runtime_error(std::format("Low-latency WASAPI stream failed: {:08x}", static_cast<unsigned>(result)));
        client->GetBufferSize(&buffer_frames);
        audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stop_event  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        client->SetEventHandle(audio_event);
        result = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(result)) throw std::runtime_error("Cannot acquire WASAPI render buffers.");
        result = client->GetService(IID_PPV_ARGS(&device_clock));
        if (SUCCEEDED(result)) result = device_clock->GetFrequency(&clock_frequency);
        if (FAILED(result)) throw std::runtime_error("Cannot acquire the WASAPI playback clock.");
        const int outputs = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
        std::vector<Steinberg::Vst::SpeakerArrangement> arrangement(outputs);
        for (int i = 0; i < outputs; ++i) processor->getBusArrangement(Steinberg::Vst::kOutput, i, arrangement[i]);
        arrangement[0] = Steinberg::Vst::SpeakerArr::kStereo;
        if (processor->setBusArrangements(nullptr, 0, arrangement.data(), outputs) != Steinberg::kResultOk) throw std::runtime_error("Kontakt rejected stereo output.");
        for (int i = 0; i < outputs; ++i) component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, i == 0);
        component->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, true);
        Steinberg::Vst::ProcessSetup setup{Steinberg::Vst::kRealtime, Steinberg::Vst::kSample32, static_cast<Steinberg::int32>(buffer_frames), static_cast<double>(sample_rate)};
        if (processor->setupProcessing(setup) != Steinberg::kResultOk) throw std::runtime_error("Kontakt rejected the processing format.");
        data.prepare(*component, buffer_frames, Steinberg::Vst::kSample32);
        data.processMode            = Steinberg::Vst::kRealtime;
        data.symbolicSampleSize     = Steinberg::Vst::kSample32;
        data.inputEvents            = &events;
        data.inputParameterChanges  = &input_parameters;
        data.outputParameterChanges = &output_parameters;
        component->setActive(true);
        map_sustain();
        ready        = true;
        load_seconds  = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        restart_flags = 0;
        start();
    }
    void Engine::publish(const Sequence& sequence) {
        std::lock_guard lock(exchange);
        pending = sequence;
        dirty   = true;
    }
    double Engine::position() const {
        const double current_position = clock.position(true);
        const double target = locating.load();
        const double requested = seek.load();
        return requested >= 0 ? requested : target >= 0 ? target : current_position;
    }
    void Engine::start() {
        ResetEvent(stop_event);
        worker = std::jthread([this] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            DWORD index{};
            const HANDLE priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
            std::array<bool, 128> active{};
            double beat = position();
            std::deque<PlaybackSpan> spans;
            std::int64_t locating_frame{};
            int last_preview{-1}, audition_frames{};
            bool pedal{}, release_pedal{true}, was_running{};
            std::int64_t total_frames{};
            float target_db = master_db.load(std::memory_order_relaxed);
            float target_gain = target_db == minimum_master_db ? 0.f : std::pow(10.f, target_db / 20.f);
            float gain = target_gain, gain_step{};
            unsigned gain_frames{};
            const auto emit = [&](int pitch, bool on, int offset, int velocity) {
                Steinberg::Vst::Event event{};
                event.busIndex     = 0;
                event.sampleOffset = offset;
                event.ppqPosition  = beat;
                event.type         = on ? Steinberg::Vst::Event::kNoteOnEvent : Steinberg::Vst::Event::kNoteOffEvent;
                if (on) event.noteOn = {0, static_cast<Steinberg::int16>(pitch), 0, velocity / 127.f, 0, pitch};
                else event.noteOff = {0, static_cast<Steinberg::int16>(pitch), 0, pitch, 0};
                events.addEvent(event);
                active[pitch] = on;
            };
            processor->setProcessing(true);
            bool device_started{};
            try {
                const std::array handles{stop_event, audio_event};
                while (!device_started || WaitForMultipleObjects(2, handles.data(), FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                    UINT32 padding{};
                    auto result = client->GetCurrentPadding(&padding);
                    if (FAILED(result)) throw std::runtime_error("The audio device stopped responding.");
                    const auto count = buffer_frames - padding;
                    if (!count) continue;
                    input_parameters.clearQueue();
                    output_parameters.clearQueue();
                    events.clear();
                    if (exchange.try_lock()) {
                        if (dirty) {
                            std::swap(current, pending);
                            dirty = false;
                        }
                        for (const auto& [id, value] : parameters) {
                            if (id == sustain_parameter) continue;
                            Steinberg::int32 queue_index{}, point_index{};
                            input_parameters.addParameterData(id, queue_index)->addPoint(0, value, point_index);
                        }
                        parameters.clear();
                        exchange.unlock();
                    }
                    const bool running = playing;
                    double requested = seek.load();
                    bool reset{};
                    if (requested >= 0) {
                        locating = requested;
                        locating_frame = total_frames;
                        beat = requested;
                        seek.compare_exchange_strong(requested, -1);
                        reset = true;
                        release_pedal = true;
                    }
                    const bool sustain = automatic_sustain;
                    if (running != was_running) release_pedal = true;
                    was_running = running;
                    const bool held = preview_held;
                    const int audition = preview.exchange(-1);
                    if (audition == -2 || running) audition_frames = 0;
                    if (audition >= 0 && !running) {
                        if (active[audition]) emit(audition, false, 0, 0);
                        last_preview    = audition;
                        audition_frames = sample_rate / 3;
                    }
                    const double step = current.bpm / (60.0 * sample_rate);
                    const PlaybackSpan span{total_frames, total_frames + count, beat, running ? step : 0.};
                    const double bar_length = current.beats * 4.0 / current.beat_unit;
                    Steinberg::Vst::ProcessContext context{};
                    context.state = Steinberg::Vst::ProcessContext::kTempoValid | Steinberg::Vst::ProcessContext::kTimeSigValid;
                    if (running) context.state |= Steinberg::Vst::ProcessContext::kPlaying;
                    context.sampleRate           = sample_rate;
                    context.projectTimeMusic     = beat;
                    context.projectTimeSamples   = static_cast<Steinberg::int64>(beat / step);
                    context.continousTimeSamples = total_frames;
                    context.tempo                = current.bpm;
                    context.timeSigNumerator     = current.beats;
                    context.timeSigDenominator   = current.beat_unit;
                    data.processContext          = &context;
                    Steinberg::int32 pedal_queue_index{}, pedal_point_index{};
                    auto* pedal_queue = input_parameters.addParameterData(sustain_parameter, pedal_queue_index);
                    pedal_queue->addPoint(0, pedal ? 1.0 : 0.0, pedal_point_index);
                    const auto set_pedal = [&](bool down, Steinberg::int32 offset) {
                        if (down == pedal) return;
                        if (offset > 0) pedal_queue->addPoint(offset - 1, pedal ? 1.0 : 0.0, pedal_point_index);
                        pedal_queue->addPoint(offset, down ? 1.0 : 0.0, pedal_point_index);
                        pedal = down;
                    };
                    double next_pedal_boundary = beat;
                    double next_boundary = beat;
                    for (UINT32 frame = 0; frame < count; ++frame) {
                        if (release_pedal) {
                            set_pedal(false, static_cast<Steinberg::int32>(frame));
                            release_pedal = false;
                            next_pedal_boundary = beat;
                        } else if (frame == 0 || (running && beat >= next_pedal_boundary)) {
                            const auto [down, up] = sustain_range(beat, bar_length);
                            set_pedal(running && sustain && beat < up, static_cast<Steinberg::int32>(frame));
                            next_pedal_boundary = beat < up ? up : down + bar_length;
                        }
                        const bool preview_ends = audition_frames == 1;
                        if (frame == 0 || (running && beat >= next_boundary) || preview_ends) {
                            std::array<int, 128> desired{};
                            std::array<bool, 128> retrigger{};
                            next_boundary = std::numeric_limits<double>::infinity();
                            if (running)
                                for (const auto& note : current.notes) {
                                    if (note.beat <= beat && note.beat + note.length > beat) desired[note.pitch] = std::max(desired[note.pitch], note.velocity);
                                    if (note.beat <= beat && note.beat > beat - step) retrigger[note.pitch] = true;
                                    if (note.beat > beat) next_boundary = std::min(next_boundary, note.beat);
                                    if (note.beat + note.length > beat) next_boundary = std::min(next_boundary, note.beat + note.length);
                                }
                            if (audition_frames > 1) desired[last_preview] = preview_velocity;
                            for (int pitch = 0; pitch < 128; ++pitch) {
                                if (active[pitch] && (reset || retrigger[pitch] || !desired[pitch])) emit(pitch, false, frame, 0);
                                if (!active[pitch] && desired[pitch]) emit(pitch, true, frame, desired[pitch]);
                            }
                            reset = false;
                        }
                        if (audition_frames > 0 && !held) --audition_frames;
                        if (running) beat += step;
                    }
                    data.numSamples = count;
                    for (int bus = 0; bus < data.numOutputs; ++bus) {
                        data.outputs[bus].silenceFlags = 0;
                        for (int channel = 0; channel < data.outputs[bus].numChannels; ++channel) std::fill_n(data.outputs[bus].channelBuffers32[channel], count, 0.f);
                    }
                    if (processor->process(data) != Steinberg::kResultOk) throw std::runtime_error("Kontakt audio processing failed.");
                    BYTE* bytes{};
                    result = render->GetBuffer(count, &bytes);
                    if (FAILED(result)) throw std::runtime_error("Cannot acquire the audio output buffer.");
                    auto* output = reinterpret_cast<float*>(bytes);
                    const float requested_db = master_db.load(std::memory_order_relaxed);
                    if (requested_db != target_db) {
                        target_db = requested_db;
                        target_gain = target_db == minimum_master_db ? 0.f : std::pow(10.f, target_db / 20.f);
                        gain_frames = sample_rate / 50;
                        gain_step = (target_gain - gain) / static_cast<float>(gain_frames);
                    }
                    std::array<float, 2> block_peaks{};
                    for (UINT32 frame = 0; frame < count; ++frame) {
                        if (gain_frames) {
                            gain += gain_step;
                            if (--gain_frames == 0) gain = target_gain;
                        }
                        for (unsigned channel = 0; channel < channels; ++channel) {
                            const float sample = channel < 2 ? data.outputs[0].channelBuffers32[channel][frame] * gain : 0.f;
                            output[frame * channels + channel] = sample;
                            if (channel < 2) block_peaks[channel] = std::max(block_peaks[channel], std::abs(sample));
                        }
                    }
                    result = render->ReleaseBuffer(count, 0);
                    if (FAILED(result)) throw std::runtime_error("Cannot submit the audio output buffer.");
                    if (!device_started) {
                        result = client->Start();
                        if (FAILED(result)) throw std::runtime_error("Cannot start the audio output stream.");
                        device_started = true;
                    }
                    for (std::size_t channel = 0; channel < block_peaks.size(); ++channel) {
                        auto previous = output_peaks[channel].load(std::memory_order_relaxed);
                        while (previous < block_peaks[channel] && !output_peaks[channel].compare_exchange_weak(previous, block_peaks[channel], std::memory_order_relaxed)) {}
                    }
                    if (std::ranges::max(block_peaks) > 1.f) overloaded.store(true, std::memory_order_relaxed);
                    if (!spans.empty() && spans.back().step == span.step && requested < 0) spans.back().last = span.last;
                    else spans.push_back(span);
                    total_frames += count;
                    UINT64 device_position{}, timestamp{};
                    result = device_clock->GetPosition(&device_position, &timestamp);
                    if (FAILED(result)) throw std::runtime_error("Cannot read the WASAPI playback clock.");
                    const double audible_frame = static_cast<double>(device_position) * sample_rate / clock_frequency;
                    while (spans.size() > 1 && audible_frame >= spans[1].first) spans.pop_front();
                    const auto& audible = spans.front();
                    const double elapsed_frames = std::clamp(audible_frame - audible.first, 0., static_cast<double>(audible.last - audible.first));
                    const double audible_beat = audible.beat + elapsed_frames * audible.step;
                    const double end_beat = audible.beat + (audible.last - audible.first) * audible.step;
                    clock.update(audible_beat, timestamp, device_position ? audible.step * sample_rate : 0., end_beat);
                    if (audible_frame >= locating_frame) locating = -1;
                }
            } catch (const std::exception& exception) {
                error  = exception.what();
                failed = true;
                PostMessageW(owner, WM_APP + 2, 0, 0);
            }
            clock.update(position(), 0, 0);
            events.clear();
            for (int pitch = 0; pitch < 128; ++pitch)
                if (active[pitch]) emit(pitch, false, 0, 0);
            data.numSamples = 1;
            input_parameters.clearQueue();
            Steinberg::int32 pedal_queue_index{}, pedal_point_index{};
            input_parameters.addParameterData(sustain_parameter, pedal_queue_index)->addPoint(0, 0.0, pedal_point_index);
            data.processContext = nullptr;
            processor->process(data);
            processor->setProcessing(false);
            client->Stop();
            client->Reset();
            if (priority) AvRevertMmThreadCharacteristics(priority);
            CoUninitialize();
        });
    }
    void Engine::stop() {
        if (!worker.joinable()) return;
        SetEvent(stop_event);
        worker.join();
    }
    void Engine::show_plugin() {
        if (plugin_window) {
            ShowWindow(plugin_window, SW_SHOW);
            SetForegroundWindow(plugin_window);
            return;
        }
        view = Steinberg::owned(controller->createView(Steinberg::Vst::ViewType::kEditor));
        if (!view) throw std::runtime_error("Kontakt has no native editor.");
        const WNDCLASSW type{.lpfnWndProc = plugin_procedure, .hInstance = GetModuleHandleW(nullptr), .hCursor = LoadCursorW(nullptr, IDC_ARROW), .lpszClassName = L"MiniDAW.Kontakt"};
        RegisterClassW(&type);
        Steinberg::ViewRect size{};
        view->getSize(&size);
        RECT rect{0, 0, size.getWidth(), size.getHeight()};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        RECT parent{};
        GetWindowRect(owner, &parent);
        plugin_window = CreateWindowW(type.lpszClassName, L"MiniDAW · Kontakt 8", WS_OVERLAPPEDWINDOW, parent.left + 40, parent.top + 40, rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, type.hInstance, this);
        view->setFrame(this);
        if (view->attached(plugin_window, Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk) throw std::runtime_error("Cannot attach Kontakt's editor.");
        ShowWindow(plugin_window, SW_SHOW);
    }
    void Engine::save_state() {
        if (!ready) return;
        stop();
        for (const auto suffix : {"component", "controller"}) {
            Steinberg::MemoryStream stream;
            const auto result = std::string_view(suffix) == "component" ? component->getState(&stream) : controller->getState(&stream);
            if (result != Steinberg::kResultOk) {
                start();
                throw std::runtime_error("Kontakt could not save its piano state.");
            }
            const auto path      = directory / (std::string("piano.") + suffix);
            const auto temporary = path.wstring() + L".writing";
            std::ofstream output(temporary, std::ios::binary);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.write(stream.getData(), stream.getSize());
            output.close();
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Could not save piano state.");
        }
        start();
    }
    void Engine::idle() {
        const int flags = restart_flags.exchange(0);
        if (flags & (Steinberg::Vst::kIoChanged | Steinberg::Vst::kReloadComponent | Steinberg::Vst::kMidiCCAssignmentChanged)) {
            stop();
            if (flags & (Steinberg::Vst::kIoChanged | Steinberg::Vst::kReloadComponent)) {
                component->setActive(false);
                data.prepare(*component, buffer_frames, Steinberg::Vst::kSample32);
                component->setActive(true);
            }
            map_sustain();
            start();
        }
    }
    LRESULT CALLBACK Engine::plugin_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
        auto* self = reinterpret_cast<Engine*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Engine*>(reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_CLOSE) {
            ShowWindow(window, SW_HIDE);
            SetForegroundWindow(self->owner);
            SetFocus(self->owner);
            PostMessageW(self->owner, WM_APP + 3, 0, 0);
            return 0;
        }
        if (self && self->view && message == WM_SIZE) {
            Steinberg::ViewRect rect{0, 0, LOWORD(l_param), HIWORD(l_param)};
            self->view->onSize(&rect);
        }
        return DefWindowProcW(window, message, w_param, l_param);
    }
    Steinberg::tresult PLUGIN_API Engine::queryInterface(const Steinberg::TUID requested, void** object) {
        if (Steinberg::FUnknownPrivate::iidEqual(requested, Steinberg::Vst::IComponentHandler::iid)) *object = static_cast<Steinberg::Vst::IComponentHandler*>(this);
        else if (Steinberg::FUnknownPrivate::iidEqual(requested, Steinberg::IPlugFrame::iid)) *object = static_cast<Steinberg::IPlugFrame*>(this);
        else return HostApplication::queryInterface(requested, object);
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API Engine::addRef() {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API Engine::release() {
        return 1;
    }
    Steinberg::tresult PLUGIN_API Engine::getName(Steinberg::Vst::String128 name) {
        std::memcpy(name, u"MiniDAW", sizeof(u"MiniDAW"));
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API Engine::beginEdit(Steinberg::Vst::ParamID) {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API Engine::performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) {
        std::lock_guard lock(exchange);
        parameters.emplace_back(id, value);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API Engine::endEdit(Steinberg::Vst::ParamID) {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API Engine::restartComponent(Steinberg::int32 flags) {
        restart_flags.fetch_or(flags);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API Engine::resizeView(Steinberg::IPlugView* target, Steinberg::ViewRect* size) {
        RECT rect{0, 0, size->getWidth(), size->getHeight()};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(plugin_window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
        target->onSize(size);
        return Steinberg::kResultOk;
    }
    void Engine::map_sustain() {
        const Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping> mapping(controller);
        if (!mapping || mapping->getMidiControllerAssignment(0, 0, Steinberg::Vst::kCtrlSustainOnOff, sustain_parameter) != Steinberg::kResultTrue) throw std::runtime_error("Kontakt does not expose sustain (CC64) on MIDI channel 1.");
    }
}
