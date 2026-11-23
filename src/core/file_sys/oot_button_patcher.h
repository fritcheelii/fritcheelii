// SPDX-FileCopyrightText: Copyright 2026 FritCheeLii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

/// Generates patched OoT button icon / PC-layout `.zs` files in `style_output_dir`
/// directly from the user's own `romfs_dir` if they do not already exist on disk.
/// Returns true if all required files for `style_idx` (1..7) are present after the call.
bool EnsureOoTButtonIconsGenerated(const VirtualDir& romfs_dir,
                                   const std::filesystem::path& style_output_dir, int style_idx);

} // namespace FileSys
