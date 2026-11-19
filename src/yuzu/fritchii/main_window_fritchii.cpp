// SPDX-FileCopyrightText: Copyright 2026 FritCheeLii
// SPDX-License-Identifier: GPL-3.0-or-later

// MainWindow's Fritchii minimalist UI setup on top of CheezyEmu 12's engine.

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>

#include "common/settings.h"
#include "core/core.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "ui_main.h"
#include "yuzu/bootmanager.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/fritchii/game_controls.h"
#include "yuzu/fritchii/home.h"
#include "yuzu/game/game_list.h"
#include "yuzu/main_window.h"
#include "yuzu/multiplayer/state.h"
#include "yuzu/util/clickable_label.h"

void MainWindow::SetUpFritchiiUi() {
    if (!FritchiiUi::Enabled()) {
        return;
    }

    constexpr u64 OcarinaOfTime = 0x0400486024DEC000ULL;
    const auto delete_shader_cache = [this](u64 program_id) {
        const QString title = tr("Delete Shader Cache");
        if (emulation_running || boot_in_progress) {
            QMessageBox::information(this, title, tr("Stop the game first."));
            return;
        }
        QMessageBox box(QMessageBox::Warning, title,
                        program_id == OcarinaOfTime ? tr("Delete OoT's shader cache?")
                                                    : tr("Delete this game's shader cache?"),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr("Only if the game crashes or looks wrong because of it. The "
                                  "shaders are prepared again on the next start."));
        QPushButton* remove = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
        box.setDefaultButton(box.addButton(QMessageBox::Cancel));
        box.exec();
        if (box.clickedButton() == remove) {
            QtCommon::Game::RemoveAllTransferableShaderCaches(program_id);
        }
    };

    FritchiiUi::RestructureMenus(
        *ui, *this,
        {.export_log = {},
         .whats_new = {},
         .delete_shader_cache = [delete_shader_cache] { delete_shader_cache(OcarinaOfTime); }});

    // Clean single minimalist status bar: hide Cheezy's extra status buttons so only
    // GRAPHICS | HANDHELD/DOCKED | VOLUME remain on the left and FPS on the right.
    for (QWidget* extra : {static_cast<QWidget*>(graphics_menu_button),
                           static_cast<QWidget*>(status_details_button),
                           static_cast<QWidget*>(renderer_status_button),
                           static_cast<QWidget*>(gpu_accuracy_button),
                           static_cast<QWidget*>(filter_status_button),
                           static_cast<QWidget*>(aa_status_button),
                           static_cast<QWidget*>(post_shader_status_button),
                           static_cast<QWidget*>(upscale_status_button),
                           static_cast<QWidget*>(frame_gen_status_button),
                           static_cast<QWidget*>(refresh_button)}) {
        if (extra) {
            extra->hide();
        }
    }
    if (multiplayer_state && multiplayer_state->GetStatusText()) {
        multiplayer_state->GetStatusText()->hide();
    }
    FritchiiUi::GraphicsHooks graphics{
        .refresh =
            [this] {
                UpdateStatusButtons();
#ifdef HAS_RESHADE
                UpdatePostShaderText();
#endif
            },
        .renderer_locked = [this] { return renderer_status_button && !renderer_status_button->isEnabled(); },
        .open_settings = [this] { OnConfigure(); },
#ifdef HAS_RESHADE
        .open_effects = [this] { OnPostProcessingShaders(); },
#endif
    };
    statusBar()->insertPermanentWidget(0, FritchiiUi::MakeGraphicsButton(this, graphics));

    // Ocarina of Time 60 FPS / 30 FPS live toggle button right next to GRAPHICS.
    auto* const oot_fps_button = new QPushButton(this);
    oot_fps_button->setObjectName(QStringLiteral("OotFpsStatusBarButton"));
    oot_fps_button->setCheckable(true);
    oot_fps_button->setFocusPolicy(Qt::NoFocus);
    oot_fps_button->setToolTip(
        tr("Ocarina of Time frame rate. Click to switch between 60 FPS and "
           "30 FPS (lower-end PCs); it takes effect at once."));
    const auto update_oot_fps = [oot_fps_button]() {
        const bool fps30 = Settings::values.oot_30fps.GetValue();
        oot_fps_button->setChecked(fps30);
        oot_fps_button->setText(fps30 ? QObject::tr("30 FPS") : QObject::tr("60 FPS"));
    };
    update_oot_fps();
    connect(oot_fps_button, &QPushButton::clicked, this, [this, update_oot_fps]() {
        Settings::values.oot_30fps.SetValue(!Settings::values.oot_30fps.GetValue());
        update_oot_fps();
        OnSaveConfig();
    });
    statusBar()->insertPermanentWidget(1, oot_fps_button);

    // Fritchii Home screen + left slim icon rail + top-right Saves & Credits buttons.
    FritchiiUi::HomeHooks home{
        .play = [this](const QString& path, u64 program_id) { OnGameListLoadFile(path, program_id); },
        .add_folder = [this] { OnGameListAddDirectory(); },
        .open_settings = [this] { OnConfigure(); },
        .game_settings =
            [this](u64 program_id, const std::string& path) {
                OpenPerGameConfiguration(program_id, path);
            },
        .delete_shader_cache = delete_shader_cache,
        .game_menu = {},
        .game_list_empty = [this] { return game_list->IsEmpty(); },
        .single_window = [this] { return ui->action_Single_Window_Mode->isChecked(); },
        .emulation_running = [this] { return emulation_running; },
        .model = game_list->GetModel(),
        .help_menu = ui->menu_Help,
        .classic_menus = {ui->menu_File, ui->menu_Emulation, ui->menu_View, ui->menu_Tools,
                          ui->menu_Help},
    };
    fritchii_home = new FritchiiUi::Home(this, ui->horizontalLayout, game_list,
                                         game_list_placeholder, std::move(home));
    connect(this, &MainWindow::EmulationStarting, fritchii_home,
            &FritchiiUi::Home::OnEmulationStarting);

    // Floating top hover controls during gameplay (menu bar is hidden by FritchiiUi::Home).
    game_controls_strip = new FritchiiUi::GameControls(
        this, {.render = [this] { return static_cast<QWidget*>(render_window); },
               .running = [] { return QtCommon::emu_thread && QtCommon::emu_thread->IsRunning(); },
               .pause = ui->action_Pause,
               .restart = ui->action_Restart,
               .stop = ui->action_Stop,
               .fullscreen = ui->action_Fullscreen,
               .menus = {ui->menu_File, ui->menu_Emulation, ui->menu_View, ui->menu_Tools,
                         ui->menu_Help}});
    connect(this, &MainWindow::EmulationStarting, game_controls_strip,
            &FritchiiUi::GameControls::Start);
    connect(this, &MainWindow::EmulationStopping, game_controls_strip,
            &FritchiiUi::GameControls::Stop);
}
