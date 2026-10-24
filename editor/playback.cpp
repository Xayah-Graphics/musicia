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
    void playback_error(HRESULT status) {
        if (FAILED(status)) throw std::runtime_error(std::format("WASAPI failed: 0x{:08X}", static_cast<unsigned long>(status)));
    }

    Playback::~Playback() {
        close();
    }

    void Playback::open(musicia::AudioClip next) {
        close();
        clip = std::move(next);
        clock.update(0, 0, 0);
        requested = 0;
        failed = false;
        overloaded = false;
        for (auto& peak : output_peaks) peak.store(0.f, std::memory_order_relaxed);
        error.clear();
        command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!command) throw std::runtime_error("Unable to create audio command event.");
        std::promise<void> initialized;
        auto ready = initialized.get_future();
        thread = std::jthread([this, initialized = std::move(initialized)](std::stop_token stop) mutable {
            bool signaled = false;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HANDLE samples_ready{}, scheduling{};
            try {
                playback_error(com);
                Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
                playback_error(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
                Microsoft::WRL::ComPtr<IMMDevice> endpoint;
                playback_error(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint));
                Microsoft::WRL::ComPtr<IAudioClient3> client;
                playback_error(endpoint->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &client));
                WAVEFORMATEX* raw_format{};
                playback_error(client->GetMixFormat(&raw_format));
                const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> format(raw_format, CoTaskMemFree);
                sample_rate = format->nSamplesPerSec;
                UINT32 fundamental{}, minimum{}, maximum{};
                playback_error(client->GetSharedModeEnginePeriod(format.get(), &period, &fundamental, &minimum, &maximum));
                playback_error(client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, format.get(), nullptr));
                samples_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!samples_ready) throw std::runtime_error("Unable to create audio buffer event.");
                playback_error(client->SetEventHandle(samples_ready));
                UINT32 capacity{};
                playback_error(client->GetBufferSize(&capacity));
                Microsoft::WRL::ComPtr<IAudioRenderClient> render;
                Microsoft::WRL::ComPtr<IAudioClock> device_clock;
                playback_error(client->GetService(IID_PPV_ARGS(&render)));
                playback_error(client->GetService(IID_PPV_ARGS(&device_clock)));
                UINT64 frequency{};
                playback_error(device_clock->GetFrequency(&frequency));
                auto pcm = clip.resample(format->nSamplesPerSec, format->nChannels);
                const std::uint64_t total = pcm.size() / format->nChannels;
                std::uint64_t offset{}, origin{};
                bool running = false;
                float gain{};
                float target_db = master_db.load(std::memory_order_relaxed);
                float target_gain = target_db == minimum_master_db ? 0.f : std::pow(10.f, target_db / 20.f);
                float master_gain = target_gain, gain_step{};
                unsigned gain_frames{};
                DWORD task{};
                scheduling = AvSetMmThreadCharacteristicsW(L"Audio", &task);
                initialized.set_value();
                signaled = true;
                const HANDLE events[]{command, samples_ready};
                while (!stop.stop_requested()) {
                    auto target = requested.load();
                    const bool active = playing.load();
                    if (target != std::numeric_limits<std::uint64_t>::max() || (running && !active)) {
                        UINT64 position{};
                        if (running) {
                            playback_error(device_clock->GetPosition(&position, nullptr));
                            playback_error(client->Stop());
                        }
                        const auto frame = target != std::numeric_limits<std::uint64_t>::max() ? target : std::min(clip.frames, origin + position * clip.sample_rate / frequency);
                        playback_error(client->Reset());
                        clock.update(static_cast<double>(frame), 0, 0);
                        if (target != std::numeric_limits<std::uint64_t>::max()) requested.compare_exchange_strong(target, std::numeric_limits<std::uint64_t>::max());
                        origin = frame;
                        offset = frame * format->nSamplesPerSec / clip.sample_rate;
                        running = false;
                        gain = 0;
                    }
                    if (active) {
                        UINT32 padding{};
                        playback_error(client->GetCurrentPadding(&padding));
                        const UINT32 available = capacity - padding;
                        const auto count = static_cast<UINT32>(std::min<std::uint64_t>(available, total - std::min(total, offset)));
                        if (count) {
                            BYTE* buffer{};
                            playback_error(render->GetBuffer(count, &buffer));
                            auto* destination = reinterpret_cast<float*>(buffer);
                            const float step = 1.f / (format->nSamplesPerSec * .003f);
                            const float requested_db = master_db.load(std::memory_order_relaxed);
                            if (requested_db != target_db) {
                                target_db = requested_db;
                                target_gain = target_db == minimum_master_db ? 0.f : std::pow(10.f, target_db / 20.f);
                                gain_frames = sample_rate / 50;
                                gain_step = (target_gain - master_gain) / static_cast<float>(gain_frames);
                            }
                            std::array<float, 2> block_peaks{};
                            for (UINT32 frame = 0; frame < count; ++frame) {
                                gain = std::min(1.f, gain + step);
                                if (gain_frames) {
                                    master_gain += gain_step;
                                    if (--gain_frames == 0) master_gain = target_gain;
                                }
                                for (UINT32 channel = 0; channel < format->nChannels; ++channel) {
                                    const float sample = pcm[(offset + frame) * format->nChannels + channel] * gain * master_gain;
                                    destination[frame * format->nChannels + channel] = sample;
                                    if (channel < 2) block_peaks[channel] = std::max(block_peaks[channel], std::abs(sample));
                                }
                            }
                            playback_error(render->ReleaseBuffer(count, 0));
                            for (std::size_t channel = 0; channel < block_peaks.size(); ++channel) {
                                auto previous = output_peaks[channel].load(std::memory_order_relaxed);
                                while (previous < block_peaks[channel] && !output_peaks[channel].compare_exchange_weak(previous, block_peaks[channel], std::memory_order_relaxed)) {}
                            }
                            if (std::ranges::max(block_peaks) > 1.f) overloaded.store(true, std::memory_order_relaxed);
                            offset += count;
                        }
                        if (!running && count) {
                            playback_error(client->Start());
                            running = true;
                        }
                        UINT64 position{}, timestamp{};
                        playback_error(device_clock->GetPosition(&position, &timestamp));
                        const double frame = std::min(static_cast<double>(clip.frames), origin + static_cast<double>(position) * clip.sample_rate / frequency);
                        clock.update(frame, timestamp, running && position ? clip.sample_rate : 0.);
                        if (offset == total && padding == 0 && !count) {
                            if (running) playback_error(client->Stop());
                            running = false;
                            playing = false;
                            clock.update(static_cast<double>(clip.frames), 0, 0);
                        }
                    }
                    WaitForMultipleObjects(2, events, FALSE, INFINITE);
                }
                if (running) playback_error(client->Stop());
            } catch (...) {
                if (!signaled) initialized.set_exception(std::current_exception());
                else {
                    try { throw; } catch (const std::exception& exception) { error = exception.what(); }
                    playing = false;
                    failed.store(true, std::memory_order_release);
                }
            }
            if (scheduling) AvRevertMmThreadCharacteristics(scheduling);
            if (samples_ready) CloseHandle(samples_ready);
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
        SetEvent(command);
    }

    std::uint64_t Playback::position() const {
        const auto target = requested.load();
        return target == std::numeric_limits<std::uint64_t>::max() ? static_cast<std::uint64_t>(std::clamp(clock.position(playing.load()), 0., static_cast<double>(clip.frames))) : target;
    }
}
