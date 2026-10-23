module;
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

export module minidaw.engine;
import std;
import minidaw.document;

export namespace minidaw {
    inline constexpr float minimum_master_db{-60}, maximum_master_db{6};

    struct Engine final : Steinberg::Vst::HostApplication, Steinberg::Vst::IComponentHandler, Steinberg::IPlugFrame {
        std::atomic<bool> playing{}, failed{}, automatic_sustain{true};
        std::atomic<double> position{}, seek{-1};
        std::atomic<float> master_db{};
        std::array<std::atomic<float>, 2> output_peaks{};
        std::atomic<bool> overloaded{};
        std::atomic<int> preview{-1}, preview_velocity{96};
        std::atomic<bool> preview_held{};
        std::string error;
        bool ready{};
        double load_seconds{};
        unsigned sample_rate{}, period{};
        HWND owner{}, plugin_window{};
        std::filesystem::path directory;

        explicit Engine(HWND owner, std::filesystem::path directory);
        ~Engine();
        void initialize();
        void publish(const Sequence& sequence);
        void start();
        void stop();
        void show_plugin();
        void save_state();
        void idle();
        static LRESULT CALLBACK plugin_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
        Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID requested, void** object) override;
        Steinberg::uint32 PLUGIN_API addRef() override;
        Steinberg::uint32 PLUGIN_API release() override;
        Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override;
        Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override;
        Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;
        Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override;
        Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;
        Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* rect) override;

    private:
        void map_sustain();

        VST3::Hosting::Module::Ptr module;
        std::unique_ptr<Steinberg::Vst::PlugProvider> provider;
        Steinberg::IPtr<Steinberg::Vst::IComponent> component;
        Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
        Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
        Steinberg::IPtr<Steinberg::IPlugView> view;
        Microsoft::WRL::ComPtr<IAudioClient3> client;
        Microsoft::WRL::ComPtr<IAudioRenderClient> render;
        HANDLE audio_event{}, stop_event{};
        unsigned channels{}, buffer_frames{};
        std::jthread worker;
        std::mutex exchange;
        Sequence pending, current;
        bool dirty{};
        Steinberg::Vst::ParamID sustain_parameter{Steinberg::Vst::kNoParamId};
        std::atomic<int> restart_flags{};
        std::vector<std::pair<Steinberg::Vst::ParamID, double>> parameters;
        Steinberg::Vst::HostProcessData data;
        Steinberg::Vst::EventList events{8192};
        Steinberg::Vst::ParameterChanges input_parameters{1024}, output_parameters{1024};
    };
}
