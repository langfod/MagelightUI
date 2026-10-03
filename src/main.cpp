// Magelight UI — SKSE plugin entry.
//
// Load order of operations: logging first, Papyrus natives registered
// unconditionally (scripts must find them even when the runtime is missing),
// crash telemetry armed, the Ultralight runtime preloaded (inert-not-crashy
// when absent), then the messaging listener: the input sink at the earliest
// device-manager moment, the Present hook at kDataLoaded, UI mode
// force-exited at every load boundary, world-ready + the version gate at
// kPostLoadGame/kNewGame. The host itself is Magelight.cpp; the exported
// API tables are MagelightApiExport.cpp.

#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <filesystem>
#include <format>

#include "Magelight.h"
#include "MagelightGameTask.h"
#include "MagelightPapyrus.h"
#include "host/HardwareInfo.h"

// Declaration version derives from CMake (PLUGIN_VERSION_MAJOR/MINOR/PATCH) —
// single source of truth, can't drift from the project version.
SKSEPluginInfo(
    .Version = { PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH, 0 },
    .Name = "Magelight",
    .Author = "Severause",
    .SupportEmail = "",
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = { 2, 0, 0, 0 }
)

static void InitializeLogging()
{
    std::filesystem::path logPath;
    auto skseLogDir = SKSE::log::log_directory();
    if (skseLogDir) {
        logPath = *skseLogDir / PLUGIN_NAME;
        logPath += ".log";
    } else {
        logPath = std::filesystem::path("Data/SKSE/Plugins") / PLUGIN_NAME;
        logPath += ".log";
    }

    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
        log->set_level(spdlog::level::trace);
        log->flush_on(spdlog::level::trace);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %T.%e] [%L] [%5t] [%21s:%4#] %v");
    } catch (const std::exception& e) {
        SKSE::stl::report_and_fail(std::format("Magelight: failed to create log file: {}", e.what()));
    }
}

static void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
{
    if (!msg) return;
    // The main thread, mid-load: a load-boundary exit's posts go through the pump (MagelightGameTask.h).
    Magelight::GameTask::Scope pumpScope;
    switch (msg->type) {
    case SKSE::MessagingInterface::kInputLoaded:
        // Earliest moment the device manager exists — see RegisterInputSink.
        Magelight::RegisterInputSink();
        break;
    case SKSE::MessagingInterface::kDataLoaded:
        // The game renderer exists well before kDataLoaded; hooking here keeps
        // us off the fragile early-init path. All Ultralight work happens
        // lazily on the RENDER thread from inside the hook (one thread owns
        // the web renderer, always).
        Magelight::InstallHook();
        Magelight::RegisterInputSink();
        break;
    case SKSE::MessagingInterface::kPreLoadGame:
        // Never carry UI mode (suspended controls, cursor menu) across a load.
        Magelight::ForceExitUIMode();
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        Magelight::ForceExitUIMode();
        Magelight::NotifyWorldReady();
        Magelight::ShowVersionGateIfNeeded();
        break;
    default:
        break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    InitializeLogging();
    SKSE::log::info("{} v{} loading (built {} {})", PLUGIN_NAME, PLUGIN_VERSION, __DATE__, __TIME__);
    SKSE::log::info(" Running on Skyrim v{}", REL::Module::get().version().string());
    Magelight::Utils::LogHardwareInfo();

    SKSE::Init(a_skse, false);
    Magelight::Papyrus::Register();   // before the runtime check: scripts must always find their natives

    Magelight::InstallCrashTelemetry();

    // Load the Ultralight runtime DLLs from Data/SKSE/Plugins/Magelight/ NOW,
    // before any delay-loaded import fires. If the runtime is missing the
    // plugin stays loaded but inert (logged), rather than taking SKSE down.
    if (!Magelight::PreloadRuntime()) {
        SKSE::log::error("Magelight: Ultralight runtime not found — overlay disabled this session");
        return true;
    }

    auto* messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnSKSEMessage);
    }

    SKSE::log::info("Magelight: loaded; awaiting kDataLoaded to install the Present hook");
    return true;
}
