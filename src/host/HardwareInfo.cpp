
#include "host/HardwareInfo.h"

#include <SKSE/SKSE.h>
#include <cpuinfo.h>
#include <dxgi.h>

#include <array>

#include "host/Platform.h"

#ifdef _WIN32
    #include <Windows.h>
#endif

namespace Magelight::Utils {

    namespace {
        /// cpuinfo is initialised once per process; every reader goes through here.
        bool EnsureCpuInfo() {
            static std::once_flag initFlag;
            static bool initOk = false;
            std::call_once(initFlag, []() {
                initOk = cpuinfo_initialize();
                if (!initOk) {
                    SKSE::log::warn("cpuinfo_initialize() failed, falling back to CPUID routine");
                } else {
                    std::atexit(cpuinfo_deinitialize);
                }
            });
            return initOk;
        }
    }  // namespace

    std::string GetCPUBrand() {
        // cpuinfo library provides a richer view of the system.  We init once
        // and remember the result; if initialization fails we drop back to the
        // CPUID routine below.
        const bool initOk = EnsureCpuInfo();
        if (initOk) {
            // Query the first processor package name.  This is typically the
            // human-readable model string.
            const struct cpuinfo_processor* procs = cpuinfo_get_processors();
            if (procs && procs->package && procs->package->name[0]) {
                return std::string(procs->package->name);
            }
            // otherwise fall back to manual path
        }
        // fall through to manual method if init failed or no usable info

        // fallback: use CPUID leafs 0x80000002..4 to build brand string
        std::array<int, 4> cpuInfo;
        char brand[0x40] = {0};
        for (int i = 0; i < 3; ++i) {
            __cpuid(cpuInfo.data(), 0x80000002 + i);
            memcpy(brand + i * 16, cpuInfo.data(), 16);
        }
        std::string ret = brand;
        if (ret.empty()) {
            return "Unknown CPU";
        }
        return ret;
    }

    uint64_t GetTotalPhysicalMemory() {
        MEMORYSTATUSEX state = {};
        state.dwLength = sizeof(state);
        if (!GlobalMemoryStatusEx(&state)) {
            SKSE::log::warn("GetTotalPhysicalMemory: GlobalMemoryStatusEx failed ({}).", GetLastError());
            return 0;
        }
        return state.ullTotalPhys;
    }

    uint32_t GetPhysicalCoreCount() {
        if (!EnsureCpuInfo()) {
            return 0;
        }
        return static_cast<uint32_t>(cpuinfo_get_cores_count());
    }

    std::string GetOSVersion() {
        // GetVersionEx is subject to the compatibility shim database and lies on
        // anything without a matching manifest; RtlGetVersion reports the real
        // build. It is not in any import library, so it is resolved by name.
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) {
            return {};
        }
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion) {
            return {};
        }

        RTL_OSVERSIONINFOW info = {};
        info.dwOSVersionInfoSize = sizeof(info);
        if (rtlGetVersion(&info) != 0) {
            return {};
        }
        // major.minor.build only — the UBR (the fourth component) is finer than
        // anything here needs and is pure entropy.
        return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
    }

    bool GetGPUInfo(GPUInfo& out) {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            return false;
        }

        bool found = false;
        GPUInfo best;
        // Every adapter, not adapter 0: on Optimus and switchable-graphics
        // laptops adapter 0 is routinely the Intel iGPU, and under RDP it is the
        // Microsoft Basic Render Driver. Largest DedicatedVideoMemory wins.
        for (UINT index = 0;; ++index) {
            IDXGIAdapter1* adapter = nullptr;
            // Any failure ends the walk: EnumAdapters1 answers DXGI_ERROR_NOT_FOUND
            // once the list is exhausted, and no other failure is recoverable.
            if (FAILED(factory->EnumAdapters1(index, &adapter)) || !adapter) {
                break;
            }

            DXGI_ADAPTER_DESC1 desc;
            hr = adapter->GetDesc1(&desc);
            adapter->Release();
            if (FAILED(hr)) {
                continue;
            }
            // The software rasteriser is not a GPU and would win nothing anyway,
            // but skipping it keeps a 0-VRAM entry from being the only candidate.
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }

            const uint64_t vram = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            if (found && vram <= best.vram) {
                continue;
            }
            best.vram = vram;
            best.vendorId = static_cast<uint32_t>(desc.VendorId);
            best.deviceId = static_cast<uint32_t>(desc.DeviceId);
            std::string nameResult = ::Magelight::WideToUtf8(desc.Description);
            best.name = nameResult.empty() ? std::string("<unknown>") : nameResult;
            found = true;
        }
        factory->Release();

        if (!found) {
            return false;
        }
        out = best;
        return true;
    }

    void LogHardwareInfo() {
        std::string cpu = GetCPUBrand();
        uint64_t memBytes = GetTotalPhysicalMemory();
        double memGB = memBytes > 0 ? static_cast<double>(memBytes) / (1024.0 * 1024.0 * 1024.0) : 0.0;
        SKSE::log::info("Hardware: OS={}, CPU='{}' ({} physical cores), RAM={:.2f}GB", GetOSVersion(), cpu, GetPhysicalCoreCount(), memGB);

        GPUInfo gpu;
        if (GetGPUInfo(gpu)) {
            double vramGB = gpu.vram > 0 ? static_cast<double>(gpu.vram) / (1024.0 * 1024.0 * 1024.0) : 0.0;
            SKSE::log::info("Hardware: GPU='{}' [{:04X}:{:04X}] ({:.2f} GB)", gpu.name, gpu.vendorId, gpu.deviceId, vramGB);
        } else {
            SKSE::log::info("Hardware: GPU info unavailable");
        }
    }

}  // namespace Magelight::Utils
