// SPDX-FileCopyrightText: Copyright 2026 FritCheeLii
// SPDX-License-Identifier: GPL-3.0-or-later

// MainWindow's Fritchii minimalist UI setup on top of CheezyEmu 12's engine.

#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>

#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "ui_main.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "yuzu/bootmanager.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/fritchii/game_controls.h"
#include "yuzu/fritchii/home.h"
#include "yuzu/game/game_list.h"
#include "yuzu/main_window.h"
#include "yuzu/multiplayer/state.h"
#include "yuzu/render/performance_overlay.h"
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
            QtCommon::Game::RemoveVulkanDriverPipelineCache(program_id);
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
        .save_config = [this] { OnSaveConfig(); },
        .restart_game = [this] { ui->action_Restart->trigger(); },
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

    // Benchmark / Performance Overlay menu button right next to 60 FPS / GRAPHICS.
    auto* const overlay_button = new QPushButton(QObject::tr("OVERLAY"), this);
    overlay_button->setObjectName(QStringLiteral("TogglableStatusBarButton"));
    overlay_button->setCheckable(true);
    overlay_button->setFocusPolicy(Qt::NoFocus);
    overlay_button->setToolTip(
        tr("Benchmark & Performance Overlay (Ctrl+V): toggle on/off, position, compact mode, "
           "FPS graph, RAM/VRAM, CPU Single-Core / GPU W & °C, and hardware banner."));
    const auto update_overlay_btn = [this, overlay_button]() {
        overlay_button->setChecked(ui->action_Show_Performance_Overlay->isChecked());
    };
    update_overlay_btn();
    connect(ui->action_Show_Performance_Overlay, &QAction::toggled, this,
            [update_overlay_btn](bool) { update_overlay_btn(); });
    connect(overlay_button, &QPushButton::clicked, this, [this, overlay_button, update_overlay_btn] {
        update_overlay_btn();
        auto& uis = UISettings::values;
        QMenu menu(overlay_button);

        const auto refresh_overlay = [this]() {
            if (perf_overlay) {
                perf_overlay->RefreshFromSettings();
            }
            OnSaveConfig();
        };

        QAction* const show_act =
            menu.addAction(QObject::tr("Show Benchmark Overlay (Ctrl+V)"), [this, update_overlay_btn] {
                const bool next = !ui->action_Show_Performance_Overlay->isChecked();
                ui->action_Show_Performance_Overlay->setChecked(next);
                UISettings::values.show_perf_overlay.SetValue(next);
                if (perf_overlay) {
                    perf_overlay->setVisible(next);
                }
                update_overlay_btn();
                OnSaveConfig();
            });
        show_act->setCheckable(true);
        show_act->setChecked(ui->action_Show_Performance_Overlay->isChecked());

        menu.addSeparator();

        QMenu* const position = menu.addMenu(QObject::tr("Position"));
        auto* const places = new QActionGroup(position);
        const std::pair<int, QString> anchors[]{
            {0, QObject::tr("Top left")},      {1, QObject::tr("Top centre")},
            {2, QObject::tr("Top right")},     {3, QObject::tr("Bottom left")},
            {4, QObject::tr("Bottom centre")}, {5, QObject::tr("Bottom right")},
        };
        for (const auto& [choice, name] : anchors) {
            QAction* const act = position->addAction(name, [choice, refresh_overlay] {
                UISettings::values.perf_overlay_anchor.SetValue(choice);
                refresh_overlay();
            });
            act->setCheckable(true);
            act->setChecked(uis.perf_overlay_anchor.GetValue() == choice);
            places->addAction(act);
        }

        const auto add_toggle = [&menu, refresh_overlay](const QString& name,
                                                         Settings::Setting<bool>& setting) {
            QAction* const act = menu.addAction(name, [&setting, refresh_overlay] {
                setting.SetValue(!setting.GetValue());
                refresh_overlay();
            });
            act->setCheckable(true);
            act->setChecked(setting.GetValue());
        };
        add_toggle(QObject::tr("Compact (FPS && Frame Time only)"), uis.perf_overlay_compact);
        add_toggle(QObject::tr("Frame Rate History Graph"), uis.perf_overlay_graph);
        add_toggle(QObject::tr("RAM && VRAM Usage"), uis.perf_overlay_memory);
        {
            QAction* const details_act =
                menu.addAction(QObject::tr("Details (CPU Single Core • GPU W/°C)"), [refresh_overlay] {
                    PerformanceOverlay::ShowDetailsFlag() = !PerformanceOverlay::ShowDetailsFlag();
                    refresh_overlay();
                });
            details_act->setCheckable(true);
            details_act->setChecked(PerformanceOverlay::ShowDetailsFlag());
        }
        {
            QAction* const hw_act =
                menu.addAction(QObject::tr("Hardware Info Banner (CPU | GPU | RAM)"), [refresh_overlay] {
                    PerformanceOverlay::ShowHwBannerFlag() = !PerformanceOverlay::ShowHwBannerFlag();
                    refresh_overlay();
                });
            hw_act->setCheckable(true);
            hw_act->setChecked(PerformanceOverlay::ShowHwBannerFlag());
        }

        QMenu* const opacity = menu.addMenu(QObject::tr("Background Opacity"));
        auto* const levels = new QActionGroup(opacity);
        for (const int percent : {100, 85, 70, 50, 30}) {
            QAction* const act =
                opacity->addAction(QObject::tr("%1%").arg(percent), [percent, refresh_overlay] {
                    UISettings::values.perf_overlay_opacity.SetValue(percent);
                    refresh_overlay();
                });
            act->setCheckable(true);
            act->setChecked(uis.perf_overlay_opacity.GetValue() == percent);
            levels->addAction(act);
        }

        const QSize sz = menu.sizeHint();
        menu.exec(overlay_button->mapToGlobal(QPoint(0, -sz.height())));
    });
    statusBar()->insertPermanentWidget(2, overlay_button);

    // Fritchii Home screen + left slim icon rail + top-right Saves, Shader Cache & Credits buttons.
    FritchiiUi::HomeHooks home{
        .play =
            [this](const QString& path, u64 program_id) {
                VideoCommon::ShaderWarm::SetStandaloneBuildMode(
                    VideoCommon::ShaderWarm::StandaloneBuildMode::None);
                OnGameListLoadFile(path, program_id);
            },
        .add_folder = [this] { OnGameListAddDirectory(); },
        .open_settings = [this] { OnConfigure(); },
        .game_settings =
            [this](u64 program_id, const std::string& path) {
                OpenPerGameConfiguration(program_id, path);
            },
        .delete_shader_cache = delete_shader_cache,
        .build_shader_cache =
            [this](const QString& path, u64 program_id, u8 mode) {
                VideoCommon::ShaderWarm::SetStandaloneBuildMode(
                    mode == 2 ? VideoCommon::ShaderWarm::StandaloneBuildMode::FullCache
                              : VideoCommon::ShaderWarm::StandaloneBuildMode::FastEssential);
                Settings::values.use_disk_shader_cache.SetValue(true);
                if (Settings::values.precache_shaders.GetValue() ==
                    Settings::PrecacheShaders::Off) {
                    Settings::values.precache_shaders.SetValue(Settings::PrecacheShaders::Always);
                }
                OnGameListLoadFile(path, program_id);
            },
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
