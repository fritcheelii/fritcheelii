// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc/svc_nx2_oot_poll.h"

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

} // namespace Kernel::Svc
