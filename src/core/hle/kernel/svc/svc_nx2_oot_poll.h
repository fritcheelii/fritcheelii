// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel::Svc {

/// Ocarina of Time: svc 0xF3, placed by the built-in patch "Resource poll backoff"
/// when enabled in Test Lab or via EDEN_NX2_OOT_POLL_BACKOFF=1. Replaces busy-spin
/// rescanning with a 500 us sleep to free CPU cycles.
bool CallNx2OotPollBackoff(Core::System& system, u32 imm);

} // namespace Kernel::Svc
