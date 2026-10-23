module;

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <wrl/client.h>

module editor.playback;

import std;
import musicia.audio;

namespace editor {
    void playbackError(HRESULT status) {
        if (FAILED(status)) throw std::runtime_error(std::format("WASAPI failed: 0x{:08X}", static_cast<unsigned long>(status)));
    }

    Playback::~Playback() {
        close();
    }

    void Playback::open(musicia::AudioClip next) {
        close();
        clip = std::move(next);
        cursor = 0;
        requested = 0;
        failed = false;
        error.clear();
        command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!command) throw std::runtime_error("Unable to create audio command event.");
        std::promise<void> initialized;
        auto ready = initialized.get_future();
        thread = std::jthread([this, initialized = std::move(initialized)](std::stop_token stop) mutable {
            bool signaled = false;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HANDLE samplesReady{}, scheduling{};
            try {
                playbackError(com);
                Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
                playbackError(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
                Microsoft::WRL::ComPtr<IMMDevice> endpoint;
                playbackError(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint));
                Microsoft::WRL::ComPtr<IAudioClient3> client;
                playbackError(endpoint->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client));
                WAVEFORMATEX* rawFormat{};
                playbackError(client->GetMixFormat(&rawFormat));
                const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(rawFormat, CoTaskMemFree);
                UINT32 period{}, fundamental{}, minimum{}, maximum{};
                playbackError(client->GetSharedModeEnginePeriod(format.get(), &period, &fundamental, &minimum, &maximum));
                playbackError(client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, format.get(), nullptr));
                samplesReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!samplesReady) throw std::runtime_error("Unable to create audio buffer event.");
                playbackError(client->SetEventHandle(samplesReady));
                UINT32 capacity{};
                playbackError(client->GetBufferSize(&capacity));
                Microsoft::WRL::ComPtr<IAudioRenderClient> render;
                Microsoft::WRL::ComPtr<IAudioClock> clock;
                playbackError(client->GetService(IID_PPV_ARGS(&render)));
                playbackError(client->GetService(IID_PPV_ARGS(&clock)));
                UINT64 frequency{};
                playbackError(clock->GetFrequency(&frequency));
                auto pcm = clip.resample(format->nSamplesPerSec, format->nChannels);
                const std::uint64_t total = pcm.size() / format->nChannels;
                std::uint64_t offset{}, origin{};
                bool running = false;
                float gain{};
                DWORD task{};
                scheduling = AvSetMmThreadCharacteristicsW(L"Audio", &task);
                initialized.set_value();
                signaled = true;
                const HANDLE events[]{command, samplesReady};
                while (!stop.stop_requested()) {
                    const auto target = requested.exchange(std::numeric_limits<std::uint64_t>::max());
                    const bool active = playing.load();
                    if (target != std::numeric_limits<std::uint64_t>::max() || (running && !active)) {
                        UINT64 position{};
                        if (running) {
                            playbackError(clock->GetPosition(&position, nullptr));
                            playbackError(client->Stop());
                        }
                        const auto frame = target != std::numeric_limits<std::uint64_t>::max() ? target : std::min(clip.frames, origin + position * clip.sampleRate / frequency);
                        playbackError(client->Reset());
                        cursor = frame;
                        origin = frame;
                        offset = frame * format->nSamplesPerSec / clip.sampleRate;
                        running = false;
                        gain = 0;
                    }
                    if (active) {
                        UINT32 padding{};
                        playbackError(client->GetCurrentPadding(&padding));
                        const UINT32 available = capacity - padding;
                        const auto count = static_cast<UINT32>(std::min<std::uint64_t>(available, total - std::min(total, offset)));
                        if (count) {
                            BYTE* buffer{};
                            playbackError(render->GetBuffer(count, &buffer));
                            auto* destination = reinterpret_cast<float*>(buffer);
                            const float step = 1.f / (format->nSamplesPerSec * .003f);
                            for (UINT32 frame = 0; frame < count; ++frame) {
                                gain = std::min(1.f, gain + step);
                                for (UINT32 channel = 0; channel < format->nChannels; ++channel) destination[frame * format->nChannels + channel] = pcm[(offset + frame) * format->nChannels + channel] * gain;
                            }
                            playbackError(render->ReleaseBuffer(count, 0));
                            offset += count;
                        }
                        if (!running && count) {
                            playbackError(client->Start());
                            running = true;
                        }
                        UINT64 position{};
                        playbackError(clock->GetPosition(&position, nullptr));
                        cursor = std::min(clip.frames, origin + position * clip.sampleRate / frequency);
                        if (offset == total && padding == 0 && !count) {
                            if (running) playbackError(client->Stop());
                            running = false;
                            playing = false;
                            cursor = clip.frames;
                        }
                    }
                    WaitForMultipleObjects(2, events, FALSE, INFINITE);
                }
                if (running) playbackError(client->Stop());
            } catch (...) {
                if (!signaled) initialized.set_exception(std::current_exception());
                else {
                    try { throw; } catch (const std::exception& exception) { error = exception.what(); }
                    playing = false;
                    failed.store(true, std::memory_order_release);
                }
            }
            if (scheduling) AvRevertMmThreadCharacteristics(scheduling);
            if (samplesReady) CloseHandle(samplesReady);
            if (SUCCEEDED(com)) CoUninitialize();
        });
        ready.get();
    }

    void Playback::close() {
        playing = false;
        if (thread.joinable()) {
            thread.request_stop();
            SetEvent(command);
            thread.join();
        }
        if (command) CloseHandle(std::exchange(command, nullptr));
    }

    void Playback::seek(std::uint64_t frame) {
        requested = frame;
        cursor = frame;
        SetEvent(command);
    }

    std::uint64_t Playback::position() const {
        const auto target = requested.load();
        return target == std::numeric_limits<std::uint64_t>::max() ? cursor.load() : target;
    }
}
