module;

#include <windows.h>

export module editor.timing;

import std;

export namespace ui {
    struct FrameClock {
        bool dirty{true}, continuous{};
        HMONITOR monitor{};
        double elapsed{};

        FrameClock() {
            QueryPerformanceFrequency(&frequency);
            timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer) throw std::runtime_error("Unable to create the frame timer.");
        }

        ~FrameClock() {
            CloseHandle(timer);
        }

        FrameClock(const FrameClock&) = delete;
        FrameClock& operator=(const FrameClock&) = delete;

        bool wait(HWND window) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const auto current = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            if (current != monitor) {
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                GetMonitorInfoW(current, &info);
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (!EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0)) throw std::runtime_error("Unable to query the display refresh rate.");
                interval = frequency.QuadPart / mode.dmDisplayFrequency;
                monitor = current;
                next = now.QuadPart;
            }
            if ((!dirty && !continuous) || IsIconic(window) || !IsWindowVisible(window)) {
                last = 0;
                MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                return false;
            }
            if (now.QuadPart >= next) {
                elapsed = last ? static_cast<double>(now.QuadPart - last) / frequency.QuadPart : 0.;
                last = now.QuadPart;
                next += ((now.QuadPart - next) / interval + 1) * interval;
                dirty = false;
                return true;
            }
            const LARGE_INTEGER due{.QuadPart = -std::max<LONGLONG>(1, (next - now.QuadPart) * 10000000 / frequency.QuadPart)};
            if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) throw std::runtime_error("Unable to schedule the next frame.");
            if (MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED) throw std::runtime_error("Unable to wait for the next frame.");
            return false;
        }

    private:
        HANDLE timer{};
        LARGE_INTEGER frequency{};
        LONGLONG interval{}, next{}, last{};
    };

    struct PlaybackClock {
        PlaybackClock() {
            QueryPerformanceFrequency(&frequency);
        }

        void update(double position, std::uint64_t timestamp, double speed) {
            version.fetch_add(1);
            anchor.store(position);
            time.store(timestamp);
            rate.store(speed);
            version.fetch_add(1);
        }

        double position(bool advancing) const {
            double base{}, speed{};
            std::uint64_t timestamp{};
            for (;;) {
                const auto before = version.load();
                if (before & 1) continue;
                base = anchor.load();
                timestamp = time.load();
                speed = rate.load();
                if (before == version.load()) break;
            }
            if (!advancing || speed == 0) return base;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsed = static_cast<double>(now.QuadPart) / frequency.QuadPart - static_cast<double>(timestamp) / 10000000.;
            return base + std::max(0., elapsed) * speed;
        }

    private:
        LARGE_INTEGER frequency{};
        std::atomic<std::uint64_t> version{}, time{};
        std::atomic<double> anchor{}, rate{};
    };
}
