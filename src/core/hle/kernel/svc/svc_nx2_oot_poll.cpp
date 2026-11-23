// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc/svc_nx2_oot_poll.h"
#include "core/memory.h"

namespace Kernel::Svc {
namespace {

constexpr u64 OotProgramId = 0x0400486024DEC000ULL;
constexpr u32 PollSvc = 0xF3;
constexpr s64 DefaultBackoffUs = 500;

bool PollBackoffActive() {
    if (const char* v = std::getenv("EDEN_NX2_OOT_POLL_BACKOFF"); v != nullptr && *v != '\0') {
        return *v != '0';
    }
    return Settings::values.test_poll_backoff.GetValue();
}

} // Anonymous namespace

bool CallNx2OotPollBackoff(Core::System& system, u32 imm) {
    if (imm != PollSvc) {
        return false;
    }
    auto& process = GetCurrentProcess(system.Kernel());
    if (!process.Is64Bit() || process.GetProgramId() != OotProgramId) {
        return false;
    }
    const s64 us = PollBackoffActive() ? DefaultBackoffUs : 0;
    static std::atomic<u64> calls{};
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 100000 == 0) {
        LOG_INFO(Kernel_SVC, "OoT resource poll backoff (Test Lab): {} rescans, now {} us each", n,
                 us);
    }
    if (us > 0) {
        SleepThread(system, us * 1000);
    }
    return true;
}

namespace {

constexpr u32 ChangebankSvc = 0xF4;
constexpr std::size_t CbUnits = 16;
constexpr std::size_t CbUnit0 = 0x408;
constexpr std::size_t CbUnitSize = 0x128;
constexpr std::size_t CbUnitPath = 0x10;
constexpr std::size_t CbUnitPathSize = 0x18;
constexpr std::size_t CbUnitPathBuf = 0x1C;
constexpr std::size_t CbPathBufSize = 0x100;
constexpr std::size_t CbMgrSpan = CbUnit0 + CbUnits * CbUnitSize;
constexpr u64 CbGapNs = 500'000'000ULL;
constexpr u64 CbLogEveryNs = 60'000'000'000ULL;

struct CbUnit {
    bool in_use{};
    u64 map{};
};

struct CbState {
    u64 mgr{};
    u64 last_ns{};
    s32 held{-1};
    u64 held_map{};
    bool have_cand{};
    u64 cand_map{};
    u64 cand_since{};
    u64 game_last_map{};
    u64 asked_last_map{};
};

struct CbStats {
    u64 updates{};
    u64 flips{};
    u64 holds{};
    u64 absorbed{};
    u64 taken{};
    u64 held_updates{};
    u64 resets{};
    u64 asked{};
};

u64 CbFnv1a(const char* s, std::size_t n) {
    u64 h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < n && s[i] != '\0'; ++i) {
        h = (h ^ static_cast<u8>(s[i])) * 0x100000001b3ULL;
    }
    return h != 0 ? h : 1;
}

bool ChangebankHoldActive() {
    if (const char* v = std::getenv("FRITCHII_CHANGEBANK_HOLD"); v != nullptr && *v != '\0') {
        return *v != '0';
    }
    return Settings::values.test_changebank_hold.GetValue();
}

u64 ChangebankHoldDurationNs() {
    constexpr long default_ms = 2000;
    long ms = default_ms;
    if (const char* v = std::getenv("FRITCHII_CHANGEBANK_HOLD_MS"); v != nullptr && *v != '\0') {
        char* end = nullptr;
        const long parsed = std::strtol(v, &end, 10);
        if (end != v && *end == '\0') {
            ms = std::clamp(parsed, 0L, 60000L);
        }
    }
    return static_cast<u64>(ms) * 1'000'000ULL;
}

void CbAdopt(CbState& s, s32 unit, const std::array<CbUnit, CbUnits>& u) {
    s.held = unit;
    s.held_map = unit >= 0 ? u[static_cast<std::size_t>(unit)].map : 0;
    s.have_cand = false;
}

s32 CbPick(CbState& s, CbStats& st, u64 mgr, const std::array<CbUnit, CbUnits>& u, s32 game,
           bool direct, u64 now, u64 hold_ns) {
    if (game >= static_cast<s32>(CbUnits) || game < -1) {
        game = -1;
    }
    const u64 gmap = game >= 0 ? u[static_cast<std::size_t>(game)].map : 0;
    ++st.updates;
    const bool fresh = s.mgr != mgr || s.last_ns == 0 || (now - s.last_ns) > CbGapNs;
    if (!fresh && gmap != s.game_last_map) {
        ++st.flips;
    }
    s.game_last_map = gmap;
    s.last_ns = now;
    if (fresh) {
        if (s.mgr == mgr && s.held >= 0) {
            ++st.resets;
        }
        s.mgr = mgr;
        CbAdopt(s, game, u);
        return game;
    }
    if (s.held >= 0) {
        const auto& hu = u[static_cast<std::size_t>(s.held)];
        if (!hu.in_use || hu.map != s.held_map) {
            ++st.resets;
            CbAdopt(s, game, u);
            return game;
        }
    }
    if (hold_ns == 0 || direct || s.held < 0 || gmap == s.held_map) {
        if (s.have_cand && gmap == s.held_map) {
            ++st.absorbed;
        }
        CbAdopt(s, game, u);
        return game;
    }
    if (!s.have_cand) {
        s.have_cand = true;
        s.cand_since = now;
        ++st.holds;
    } else if (s.cand_map != gmap) {
        ++st.holds;
    }
    s.cand_map = gmap;
    if ((now - s.cand_since) >= hold_ns) {
        ++st.taken;
        CbAdopt(s, game, u);
        return game;
    }
    ++st.held_updates;
    return s.held;
}

s32 CbStep(CbState& s, CbStats& st, u64 mgr, const std::array<CbUnit, CbUnits>& u, s32 game,
           bool direct, u64 now, u64 hold_ns) {
    const bool same = s.mgr == mgr && s.last_ns != 0 && (now - s.last_ns) <= CbGapNs;
    const s32 pick = CbPick(s, st, mgr, u, game, direct, now, hold_ns);
    const u64 map = (pick >= 0 && pick < static_cast<s32>(CbUnits))
                        ? u[static_cast<std::size_t>(pick)].map
                        : 0;
    if (same && map != s.asked_last_map) {
        ++st.asked;
    }
    s.asked_last_map = map;
    return pick;
}

template <typename T>
T ReadLe(const std::array<u8, CbMgrSpan>& buf, std::size_t offset) {
    T val{};
    std::memcpy(&val, buf.data() + offset, sizeof(T));
    return val;
}

} // Anonymous namespace

bool CallNx2OotChangebankHold(Core::System& system, u32 imm, std::span<u64, 8> args) {
    if (imm != ChangebankSvc) {
        return false;
    }
    auto& process = GetCurrentProcess(system.Kernel());
    if (!process.Is64Bit() || process.GetProgramId() != OotProgramId) {
        return false;
    }

    const u64 mgr = args[0];
    const s32 w15 = static_cast<s32>(static_cast<u32>(args[1]));
    const s32 w17 = static_cast<s32>(static_cast<u32>(args[2]));
    args[0] = static_cast<u32>(w15);

    if (!ChangebankHoldActive()) {
        return true;
    }
    if (w15 < -1 || w15 >= static_cast<s32>(CbUnits) ||
        w17 < -1 || w17 >= static_cast<s32>(CbUnits)) {
        return true;
    }

    const bool direct = (w15 < 0 && w17 >= 0);
    const s32 game = direct ? w17 : w15;

    auto& memory = system.ApplicationMemory();
    if (mgr == 0 || !memory.IsValidVirtualAddressRange(mgr, CbMgrSpan)) {
        return true;
    }

    std::array<u8, CbMgrSpan> buf{};
    if (!memory.ReadBlock(mgr, buf.data(), buf.size())) {
        return true;
    }

    std::array<CbUnit, CbUnits> units{};
    for (std::size_t k = 0; k < CbUnits; ++k) {
        const std::size_t uoff = CbUnit0 + k * CbUnitSize;
        const u8 use = buf[uoff];
        if (use > 1) {
            return true;
        }
        const u64 path_ptr = ReadLe<u64>(buf, uoff + CbUnitPath);
        const u32 path_size = ReadLe<u32>(buf, uoff + CbUnitPathSize);
        if (path_ptr != mgr + uoff + CbUnitPathBuf || path_size != CbPathBufSize) {
            return true;
        }
        units[k].in_use = (use == 1);
        if (use == 1 && buf[uoff + CbUnitPathBuf] != 0) {
            const char* p = reinterpret_cast<const char*>(buf.data() + uoff + CbUnitPathBuf);
            units[k].map = CbFnv1a(p, CbPathBufSize);
        }
    }

    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    const u64 hold_ns = ChangebankHoldDurationNs();

    static std::mutex mutex;
    static CbState state{};
    static CbStats stats{};
    static CbStats logged{};
    static u64 last_log_ns{};
    static bool startup_logged = false;

    s32 pick = game;
    {
        std::scoped_lock lock{mutex};
        if (!startup_logged) {
            startup_logged = true;
            LOG_INFO(Kernel_SVC,
                     "OoT changebank hold (NX2Saria): active, {} ms preload hold",
                     hold_ns / 1'000'000ULL);
        }
        pick = CbStep(state, stats, mgr, units, game, direct, now, hold_ns);
        if (last_log_ns == 0) {
            last_log_ns = now;
        } else if ((now - last_log_ns) >= CbLogEveryNs) {
            const u64 d_flips = stats.flips - logged.flips;
            const u64 d_holds = stats.holds - logged.holds;
            const u64 d_absorbed = stats.absorbed - logged.absorbed;
            const u64 d_asked = stats.asked - logged.asked;
            logged = stats;
            last_log_ns = now;
            if (d_flips > 0 || d_holds > 0) {
                LOG_INFO(Kernel_SVC,
                         "OoT changebank hold: {} flips -> {} asked ({} absorbed in {} ms hold)",
                         d_flips, d_asked, d_absorbed, hold_ns / 1'000'000ULL);
            }
        }
    }

    if (!direct && pick != game) {
        args[0] = static_cast<u32>(pick);
    }
    return true;
}

} // namespace Kernel::Svc
