// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel::Svc {

/// Ocarina of Time: svc 0xF3, placed by the built-in patch "Resource poll backoff"
/// when enabled in Test Lab or via EDEN_NX2_OOT_POLL_BACKOFF=1. Replaces busy-spin
/// rescanning with a 500 us sleep to free CPU cycles.
bool CallNx2OotPollBackoff(Core::System& system, u32 imm);

/// Ocarina of Time 1.1.1: svc 0xF4, placed by the built-in patch "Neighbour Map Preload Hold"
/// (ported from NX2Saria changebank_hold). Holds the preloaded neighbouring map for 2 s so
/// turning in overlapping load radii (e.g. Hyrule Market) does not repeatedly cancel & restart loads.
bool CallNx2OotChangebankHold(Core::System& system, u32 imm, std::span<u64, 8> args);

} // namespace Kernel::Svc
