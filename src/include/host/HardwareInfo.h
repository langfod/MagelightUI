#pragma once

#include <cstdint>
#include <string>


namespace Magelight::Utils {


    /// CPU brand string (e.g. "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz"); "Unknown CPU" on failure.
    std::string GetCPUBrand();

    /// Total physical memory in bytes, or zero on failure. Excludes hardware-reserved memory
    /// (a 64 GB machine reports ~61.6 GB), so bucket by nearest, not down.
    uint64_t GetTotalPhysicalMemory();

    /// Number of physical (not logical) CPU cores, or zero if cpuinfo is unavailable.
    uint32_t GetPhysicalCoreCount();

    /// Windows version as "major.minor.build" (e.g. "10.0.26100") via RtlGetVersion. Empty on failure.
    std::string GetOSVersion();

    /// One DXGI adapter's identity.
    struct GPUInfo {
        std::string name;           ///< UTF-8 adapter description (logging only)
        uint64_t    vram     = 0;   ///< DedicatedVideoMemory in bytes; 0 on iGPU/RDP
        uint32_t    vendorId = 0;   ///< PCI vendor id, e.g. 0x10DE
        uint32_t    deviceId = 0;   ///< PCI device id, e.g. 0x2684
    };

    /// Returns the DXGI adapter with the most dedicated VRAM (adapter 0 is often an iGPU or the RDP
    /// basic driver). False if DXGI is unavailable; a success may still report vram == 0 (unknown).
    bool GetGPUInfo(GPUInfo& out);

    /// Logs CPU, memory and GPU info at INFO level.
    void LogHardwareInfo();

}  // namespace Magelight::Utils
