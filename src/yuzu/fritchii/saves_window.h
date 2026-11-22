// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>

#include <QString>

class QWidget;

namespace FritchiiUi {

struct OotSlotBrief {
    int slot = 1;
    QString name;
    bool has_data = false;
    bool active = false;
    QString date_str;
};

/// The Saves window (Fritchii v5.8.3): export the saves to a .zip, import them from one, or copy
/// them between Fritchii, Eden (%APPDATA%\eden) and Lichii 150+ (%APPDATA%\lichii). Saves that
/// get replaced are backed up first. `emulation_running` blocks it while a game runs.
void ShowSavesWindow(QWidget* parent, const std::function<bool()>& emulation_running);

/// Lists all 8 Zelda OoT Multi-Save slots (seeding Slot 1 from active NAND data if needed).
std::vector<OotSlotBrief> ListOotSaveSlots();

/// Switches the active Zelda OoT Multi-Save slot (1..8), backing up the previous active slot.
bool SwitchOotSaveSlot(int target_slot);

/// Prompts the user to pick one of the 8 Zelda OoT save slots before launching (unless skipped).
/// When `force_show` is true, always displays the picker; when `launch_mode` is false, selects
/// the active slot without launching the game. Returns true if accepted/proceeding, false if cancelled.
bool ChooseOotSaveSlotBeforePlay(QWidget* parent, const std::function<bool()>& emulation_running,
                                 bool force_show = false, bool launch_mode = true);

/// Exports all emulator logs (including log/history/) to a user-chosen .zip file.
void ExportLogsZip(QWidget* parent);

} // namespace FritchiiUi
