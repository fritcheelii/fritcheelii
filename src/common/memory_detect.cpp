// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#ifdef __linux__
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#endif

#include <atomic>
#include <chrono>
#include <new>

#include "common/logging.h"
#include "common/memory_detect.h"

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

ProcessMemoryUsage GetProcessMemoryUsage() {
    ProcessMemoryUsage usage{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        usage.commit = counters.PrivateUsage;
        usage.working_set = counters.WorkingSetSize;
        usage.private_working_set = counters.PrivateWorkingSetSize;
    }
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        usage.system_commit = u64{performance.CommitTotal} * performance.PageSize;
        usage.system_commit_limit = u64{performance.CommitLimit} * performance.PageSize;
    }
#elif defined(__linux__)
    std::ifstream status{"/proc/self/status"};
    for (std::string line; std::getline(status, line);) {
        const auto field = [&line](std::string_view key) -> u64 {
            if (!line.starts_with(key)) {
                return 0;
            }
            return std::strtoull(line.c_str() + key.size(), nullptr, 10) << 10;
        };
        usage.commit = (std::max)(usage.commit, field("VmData:"));
        usage.working_set = (std::max)(usage.working_set, field("VmRSS:"));
        usage.private_working_set = (std::max)(usage.private_working_set, field("RssAnon:"));
    }
#endif
    return usage;
}

std::optional<u64> GetSystemCommitRoom() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullAvailPageFile;
    }
#elif defined(__APPLE__)
    // macOS: memory it can hand out without swapping (free, inactive and purgeable pages). A
    // tester's M1 Pro (16 GB) was killed during the shader precache with no guard at all.
    const mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page_size = 0;
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) ==
            KERN_SUCCESS &&
        host_page_size(host, &page_size) == KERN_SUCCESS) {
        return (u64{vm.free_count} + vm.inactive_count + vm.purgeable_count) * page_size;
    }
#elif defined(__linux__)
    // Linux: MemAvailable, what the kernel can hand out without swapping.
    std::ifstream meminfo{"/proc/meminfo"};
    std::string key;
    u64 kib = 0;
    while (meminfo >> key >> kib) {
        meminfo.ignore(64, '\n');
        if (key == "MemAvailable:") {
            return kib * 1024;
        }
    }
#endif
    return std::nullopt;
}

#ifdef _WIN32
namespace {
using CommitClock = std::chrono::steady_clock;
constexpr std::chrono::seconds CommitWaitLimit{5};
constexpr DWORD CommitRetryMs = 50;
constexpr u64 NewHandlerCommitRoom = u64{4} << 30;

bool IsOutOfCommit(DWORD error) {
    return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY ||
           error == ERROR_OUTOFMEMORY || error == ERROR_NO_SYSTEM_RESOURCES ||
           error == ERROR_PAGEFILE_QUOTA;
}

void OnNewRefused() {
    thread_local bool in_log = false;
    thread_local CommitClock::time_point first{};
    thread_local CommitClock::time_point last{};
    static std::atomic<int> told{0};
    const CommitClock::time_point now = CommitClock::now();
    if (last == CommitClock::time_point{} || now - last > std::chrono::milliseconds{250}) {
        first = now;
    }
    last = now;
    const std::optional<u64> room = GetSystemCommitRoom();
    if (!room || *room > NewHandlerCommitRoom) {
        throw std::bad_alloc{};
    }
    if (now - first >= CommitWaitLimit) {
        if (!in_log) {
            in_log = true;
            LOG_CRITICAL(Common_Memory,
                         "Out of memory: C++ allocation refused for 5 s at Windows commit limit "
                         "({} MiB left).",
                         *room >> 20);
            in_log = false;
        }
        throw std::bad_alloc{};
    }
    if (!in_log && told.fetch_add(1, std::memory_order_relaxed) < 8) {
        in_log = true;
        LOG_WARNING(Common_Memory,
                    "C++ allocation refused at Windows commit limit ({} MiB left): retrying up to 5 s",
                    *room >> 20);
        in_log = false;
    }
    Sleep(CommitRetryMs);
}
} // Anonymous namespace

bool CommitPagesWaiting(void* address, u64 bytes, bool quiet) {
    if (VirtualAlloc(address, static_cast<SIZE_T>(bytes), MEM_COMMIT, PAGE_READWRITE) != nullptr)
        [[likely]] {
        return true;
    }
    DWORD error = GetLastError();
    if (!IsOutOfCommit(error)) {
        return false;
    }
    static std::atomic<int> told{0};
    const bool tell = !quiet && told.fetch_add(1, std::memory_order_relaxed) < 8;
    if (tell) {
        LOG_WARNING(Common_Memory,
                    "Committing {} KiB refused at Windows commit limit (error {}, {} MiB left): "
                    "retrying up to 5 s",
                    bytes >> 10, error, GetSystemCommitRoom().value_or(0) >> 20);
    }
    const CommitClock::time_point start = CommitClock::now();
    while (CommitClock::now() - start < CommitWaitLimit) {
        Sleep(CommitRetryMs);
        if (VirtualAlloc(address, static_cast<SIZE_T>(bytes), MEM_COMMIT, PAGE_READWRITE) !=
            nullptr) {
            return true;
        }
        error = GetLastError();
        if (!IsOutOfCommit(error)) {
            break;
        }
    }
    return false;
}
#endif

void InstallCommitLimitNewHandler() {
#ifdef _WIN32
    std::set_new_handler(&OnNewRefused);
#endif
}

} // namespace Common
