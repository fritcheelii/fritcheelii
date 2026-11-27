// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <functional>
#include <vector>
#include <algorithm>
#include <array>

#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStringLiteral>
#include <QtCore/qobjectdefs.h>
#include <qabstractbutton.h>
#include <qboxlayout.h>
#include <qcombobox.h>
#include <qcoreevent.h>
#include <qglobal.h>
#include <qgridlayout.h>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure_graphics.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/nvidia_dlss_panel.h"
#include "yuzu/configuration/shared_widget.h"

#ifdef HAS_RESHADE
#include "yuzu/configuration/configure_post_processing.h"
#endif

ConfigureGraphics::ConfigureGraphics(
    const Core::System& system_, std::vector<VkDeviceInfo::Record>& records_,
    const std::function<void()>& expose_compute_option_,
    const std::function<void(Settings::AspectRatio, Settings::ResolutionSetup)>&
        update_aspect_ratio_,
    std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : ConfigurationShared::Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphics>()},
      records{records_}, expose_compute_option{expose_compute_option_},
      update_aspect_ratio{update_aspect_ratio_}, system{system_},
      combobox_translations{builder.ComboboxTranslations()} {
    vulkan_device = Settings::values.vulkan_device.GetValue();
    RetrieveVulkanDevices();

    ui->setupUi(this);

    Setup(builder);

    for (const auto& device : vulkan_devices) {
        vulkan_device_combobox->addItem(device);
    }

    UpdateBackgroundColorButton(QColor::fromRgb(Settings::values.bg_red.GetValue(),
                                                Settings::values.bg_green.GetValue(),
                                                Settings::values.bg_blue.GetValue()));
    UpdateAPILayout();
    PopulateVSyncModeSelection(false); //< must happen after UpdateAPILayout

    // VSync setting needs to be determined after populating the VSync combobox
    const auto vsync_mode_setting = Settings::values.vsync_mode.GetValue();
    const auto vsync_mode = VSyncSettingToMode(vsync_mode_setting);
    int index{};
    for (const auto mode : vsync_mode_combobox_enum_map) {
        if (mode == vsync_mode) {
            break;
        }
        index++;
    }
    if (static_cast<unsigned long>(index) < vsync_mode_combobox_enum_map.size()) {
        vsync_mode_combobox->setCurrentIndex(index);
    }

    connect(api_combobox, qOverload<int>(&QComboBox::activated), this, [this] {
        UpdateAPILayout();
        PopulateVSyncModeSelection(false);
    });
    connect(vulkan_device_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int device) {
                UpdateDeviceSelection(device);
                PopulateVSyncModeSelection(false);
            });
    connect(ui->bg_button, &QPushButton::clicked, this, [this] {
        const QColor new_bg_color = QColorDialog::getColor(bg_color);
        if (!new_bg_color.isValid()) {
            return;
        }
        UpdateBackgroundColorButton(new_bg_color);
    });

    const auto& update_screenshot_info = [this, &builder]() {
        const auto& combobox_enumerations = builder.ComboboxTranslations().at(
            Settings::EnumMetadata<Settings::AspectRatio>::Index());
        const auto ratio_index = aspect_ratio_combobox->currentIndex();
        const auto ratio =
            static_cast<Settings::AspectRatio>(combobox_enumerations[ratio_index].first);

        const auto& combobox_enumerations_resolution = builder.ComboboxTranslations().at(
            Settings::EnumMetadata<Settings::ResolutionSetup>::Index());
        const auto res_index = resolution_combobox->currentIndex();
        const auto setup = static_cast<Settings::ResolutionSetup>(
            combobox_enumerations_resolution[res_index].first);

        update_aspect_ratio(ratio, setup);
    };

    connect(aspect_ratio_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            update_screenshot_info);
    connect(resolution_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            update_screenshot_info);

    api_combobox->setEnabled(!UISettings::values.has_broken_vulkan && api_combobox->isEnabled());
    ui->api_widget->setEnabled(
        (!UISettings::values.has_broken_vulkan || Settings::IsConfiguringGlobal()) &&
        ui->api_widget->isEnabled());

    if (Settings::IsConfiguringGlobal()) {
        ui->bg_widget->setEnabled(Settings::values.bg_red.UsingGlobal());
    }
}

void ConfigureGraphics::PopulateVSyncModeSelection(bool use_setting) {
    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        vsync_mode_combobox->setEnabled(false);
        return;
    }
    vsync_mode_combobox->setEnabled(true);

    const int current_index = //< current selected vsync mode from combobox
        vsync_mode_combobox->currentIndex();
    const auto current_mode = //< current selected vsync mode as a VkPresentModeKHR
        current_index == -1 || use_setting
            ? VSyncSettingToMode(Settings::values.vsync_mode.GetValue())
            : vsync_mode_combobox_enum_map[current_index];
    int index{};
    const int device{vulkan_device_combobox->currentIndex()}; //< current selected Vulkan device

    const auto& present_modes = //< relevant vector of present modes for the selected device or API
        backend == Settings::RendererBackend::Vulkan && device > -1 ? device_present_modes[device]
                                                                    : default_present_modes;

    vsync_mode_combobox->clear();
    vsync_mode_combobox_enum_map.clear();
    vsync_mode_combobox_enum_map.reserve(present_modes.size());
    for (const auto present_mode : present_modes) {
        const auto mode_name = TranslateVSyncMode(present_mode, backend);
        if (mode_name.isEmpty()) {
            continue;
        }

        vsync_mode_combobox->insertItem(index, mode_name);
        vsync_mode_combobox_enum_map.push_back(present_mode);
        if (present_mode == current_mode) {
            vsync_mode_combobox->setCurrentIndex(index);
        }
        index++;
    }

    if (!Settings::IsConfiguringGlobal()) {
        vsync_restore_global_button->setVisible(!Settings::values.vsync_mode.UsingGlobal());

        const Settings::VSyncMode global_vsync_mode = Settings::values.vsync_mode.GetValue(true);
        vsync_restore_global_button->setEnabled(
            ((backend == Settings::RendererBackend::OpenGL_GLSL ||
              backend == Settings::RendererBackend::OpenGL_GLASM ||
              backend == Settings::RendererBackend::OpenGL_SPIRV) &&
             (global_vsync_mode == Settings::VSyncMode::Immediate ||
              global_vsync_mode == Settings::VSyncMode::Fifo)) ||
            backend == Settings::RendererBackend::Vulkan);
    }
}

void ConfigureGraphics::UpdateVsyncSetting() const {
    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        return;
    }

    const auto mode = vsync_mode_combobox_enum_map[vsync_mode_combobox->currentIndex()];
    const auto vsync_mode = PresentModeToSetting(mode);
    Settings::values.vsync_mode.SetValue(vsync_mode);
}

void ConfigureGraphics::UpdateDeviceSelection(int device) {
    if (device == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan) {
        vulkan_device = device;
    }
}

ConfigureGraphics::~ConfigureGraphics() = default;

void ConfigureGraphics::SetConfiguration() {}

void ConfigureGraphics::Setup(const ConfigurationShared::Builder& builder) {
    QLayout* api_layout = ui->api_widget->layout();
    QWidget* api_grid_widget = new QWidget(this);
    QVBoxLayout* api_grid_layout = new QVBoxLayout(api_grid_widget);
    api_grid_layout->setContentsMargins(0, 0, 0, 0);
    api_layout->addWidget(api_grid_widget);

    QLayout& graphics_layout = *ui->graphics_widget->layout();

    std::map<u32, QWidget*> hold_graphics;
    std::vector<QWidget*> hold_api;
    QCheckBox* frame_gen_checkbox = nullptr;
    QComboBox* fg_type_combo = nullptr;
    QCheckBox* amd_fsr4_cb = nullptr;
    QCheckBox* nvidia_dlss_cb = nullptr;
    QCheckBox* game_dlss_cb = nullptr;

    for (const auto setting : Settings::values.linkage.by_category[Settings::Category::Renderer]) {
#ifndef HAS_RESHADE
        if (setting->Id() == Settings::values.post_shader_enabled.Id()) {
            continue;
        }
#endif
#ifndef EGLANTINE_STREAMLINE
        // Only builds with NVIDIA Streamline (Windows) can use it.
        if (setting->Id() == Settings::values.nvidia_dlss.Id()) {
            continue;
        }
#endif

        ConfigurationShared::Widget* widget = [&]() {
            if (setting->Id() == Settings::values.hdr_reference_white.Id() ||
                setting->Id() == Settings::values.hdr_peak_luminance.Id()) {
                return builder.BuildWidget(setting, apply_funcs,
                                           ConfigurationShared::RequestType::SpinBox, true, 1.0f,
                                           nullptr, tr("nits"));
            } else if (setting->Id() == Settings::values.fsr_sharpening_slider.Id()) {
                // FSR needs a reversed slider and a 0.5 multiplier
                return builder.BuildWidget(
                    setting, apply_funcs, ConfigurationShared::RequestType::ReverseSlider, true,
                    0.5f, nullptr, tr("%", "FSR/SGSR sharpening percentage (e.g. 50%)"));
            } else if (setting->Id() == Settings::values.frame_rate_limit.Id()) {
                return builder.BuildWidget(setting, apply_funcs,
                                           ConfigurationShared::RequestType::Slider, true, 1.0f,
                                           nullptr, tr(" Hz", "Frame rate limit (e.g. 120 Hz)"));
            } else {
                return builder.BuildWidget(setting, apply_funcs);
            }
        }();

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        if (setting->Id() == Settings::values.hdr_mode.Id() ||
            setting->Id() == Settings::values.hdr_swapchain.Id() ||
            setting->Id() == Settings::values.hdr_reference_white.Id() ||
            setting->Id() == Settings::values.hdr_peak_luminance.Id()) {
            hdr_widgets.push_back(widget);
        }

        if (setting->Id() == Settings::values.renderer_backend.Id()) {
            // Add the renderer combobox now so it's at the top
            api_grid_layout->addWidget(widget);
            api_combobox = widget->combobox;
            api_restore_global_button = widget->restore_button;
            if (!Settings::IsConfiguringGlobal()) {
                api_restore_global_button->connect(api_restore_global_button,
                                                   &QAbstractButton::clicked,
                                                   [this](bool) { UpdateAPILayout(); });
                // Detach API's restore button and place it where we want
                // Lets us put it on the side, and it will automatically scale if there's a
                // second combobox (vulkan_device)
                widget->layout()->removeWidget(api_restore_global_button);
                api_layout->addWidget(api_restore_global_button);
            }
        } else if (setting->Id() == Settings::values.vulkan_device.Id()) {
            // Keep track of vulkan_device's combobox so we can populate it
            hold_api.push_back(widget);
            vulkan_device_combobox = widget->combobox;
            vulkan_device_widget = widget;
        } else if (setting->Id() == Settings::values.vsync_mode.Id()) {
            // Keep track of vsync_mode's combobox so we can populate it
            vsync_mode_combobox = widget->combobox;

            // Since vsync is populated at runtime, we have to manually set up the button for
            // restoring the global setting.
            if (!Settings::IsConfiguringGlobal()) {
                QPushButton* restore_button =
                    ConfigurationShared::Widget::CreateRestoreGlobalButton(
                        Settings::values.vsync_mode.UsingGlobal(), widget);
                restore_button->setEnabled(true);
                widget->layout()->addWidget(restore_button);

                restore_button->connect(restore_button, &QAbstractButton::clicked,
                                        [restore_button, this](bool) {
                                            Settings::values.vsync_mode.SetGlobal(true);
                                            PopulateVSyncModeSelection(true);

                                            restore_button->setVisible(false);
                                        });

                std::function<void()> set_non_global = [restore_button, this]() {
                    Settings::values.vsync_mode.SetGlobal(false);
                    UpdateVsyncSetting();
                    restore_button->setVisible(true);
                };
                widget->combobox->connect(widget->combobox,
                                          QOverload<int>::of(&QComboBox::activated),
                                          [set_non_global]() { set_non_global(); });
                vsync_restore_global_button = restore_button;
            }
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.aspect_ratio.Id()) {
            // Keep track of the aspect ratio combobox to update other UI tabs that need it
            aspect_ratio_combobox = widget->combobox;
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.resolution_setup.Id()) {
            // Keep track of the resolution combobox to update other UI tabs that need it
            resolution_combobox = widget->combobox;
            hold_graphics.emplace(setting->Id(), widget);
#ifdef HAS_RESHADE
        } else if (setting->Id() == Settings::values.post_shader_enabled.Id()) {
            QPushButton* post_shader_button = new QPushButton(tr("Configure Effects..."), widget);
            post_shader_button->setVisible(widget->checkbox->isChecked());

            connect(post_shader_button, &QAbstractButton::clicked, this, [this]() {
                ConfigurePostProcessing dialog(this);
                dialog.exec();
            });
            connect(widget->checkbox, &QCheckBox::toggled, post_shader_button,
                    &QWidget::setVisible);

            QBoxLayout* row = qobject_cast<QBoxLayout*>(widget->layout());
            row->insertWidget(1, post_shader_button);

            hold_graphics.emplace(setting->Id(), widget);
#endif
#ifdef EGLANTINE_STREAMLINE
        } else if (setting->Id() == Settings::values.nvidia_dlss.Id()) {
            nvidia_dlss_cb = widget->checkbox;
            hold_graphics.emplace(setting->Id(), widget);
#endif
        } else if (setting->Id() == Settings::values.amd_fsr4.Id()) {
            amd_fsr4_cb = widget->checkbox;
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.game_dlss_network.Id()) {
            game_dlss_cb = widget->checkbox;
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.frame_gen.Id()) {
            frame_gen_checkbox = widget->checkbox;
            hold_graphics.emplace(setting->Id(), widget);
        } else if (setting->Id() == Settings::values.frame_gen_type.Id()) {
            fg_type_combo = widget->combobox;
            hold_graphics.emplace(setting->Id(), widget);
        } else {
            hold_graphics.emplace(setting->Id(), widget);
        }
    }

    auto find_w = [&](u32 id) -> QWidget* {
        const auto it = hold_graphics.find(id);
        return it != hold_graphics.end() ? it->second : nullptr;
    };
    QWidget* amd_fsr4_w = find_w(Settings::values.amd_fsr4.Id());
    QWidget* nvidia_dlss_w = find_w(Settings::values.nvidia_dlss.Id());
    QWidget* game_dlss_w = find_w(Settings::values.game_dlss_network.Id());
    QWidget* dump_flow_w = find_w(Settings::values.frame_gen_dump_flow.Id());
    for (QWidget* hidden_w : {amd_fsr4_w, nvidia_dlss_w, game_dlss_w, dump_flow_w}) {
        if (hidden_w != nullptr) {
            hidden_w->setVisible(false);
        }
    }

    // 1) Top box: Unified Ocarina of Time Upscaling Method & 3D Render Scale
    const std::array oot_scale_ids{
        Settings::values.oot_fsr_preset.Id(),
        Settings::values.oot_docked_output.Id(),
        Settings::values.oot_anti_aliasing.Id(),
    };
    const std::array upscale_hidden_ids{
        Settings::values.amd_fsr4.Id(),
        Settings::values.nvidia_dlss.Id(),
        Settings::values.game_dlss_network.Id(),
        Settings::values.frame_gen_dump_flow.Id(),
    };
    const std::array fg_ids{
        Settings::values.frame_gen.Id(),
        Settings::values.frame_gen_type.Id(),
        Settings::values.amd_frame_gen_version.Id(),
        Settings::values.frame_gen_multiplier.Id(),
        Settings::values.frame_gen_target_rate.Id(),
        Settings::values.frame_gen_flow_scale_auto.Id(),
        Settings::values.frame_gen_flow_scale.Id(),
        Settings::values.frame_gen_queue_target.Id(),
    };

    {
        auto* oot_scale_box =
            new QGroupBox(tr("Ocarina of Time — Upscaling && 3D Render Scale"), this);
        auto* box_layout = new QVBoxLayout(oot_scale_box);

        auto* method_row = new QWidget(oot_scale_box);
        auto* method_layout = new QBoxLayout(QBoxLayout::LeftToRight, method_row);
        method_layout->setContentsMargins(0, 0, 0, 0);
        auto* method_label = new QLabel(tr("Upscaling Method:"), method_row);
        method_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* method_combo = new QComboBox(method_row);
        method_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        enum UpscaleChoice {
            UpscaleNone = 0,
            UpscaleFsr4 = 1,
            UpscaleNvidiaDlss = 2,
            UpscaleGameDlss = 3,
        };
        method_combo->addItem(tr("None — 3D Render Scale Only (Sharpest / Highest FPS / 4 GB)"),
                              UpscaleNone);
        if (amd_fsr4_cb != nullptr) {
            method_combo->addItem(
                tr("AMD FSR 4 / 3.1 (Smooth / Medium FPS — AMD / Intel / GTX / RTX)"),
                UpscaleFsr4);
        }
        if (game_dlss_cb != nullptr) {
            method_combo->addItem(
                tr("Game's Built-in DLSS (Emulated — High Sharpness, Any GPU)"),
                UpscaleGameDlss);
        }
#ifdef EGLANTINE_STREAMLINE
        if (nvidia_dlss_cb != nullptr) {
            method_combo->addItem(
                tr("NVIDIA DLSS (RTX GPUs Only — Sharp & Smooth on Tensor Cores)"),
                UpscaleNvidiaDlss);
        }
#endif
        method_layout->addWidget(method_label, 1);
        method_layout->addWidget(method_combo, 1);
        box_layout->addWidget(method_row);

#ifdef EGLANTINE_STREAMLINE
        NvidiaDlssPanel* dlss_panel = new NvidiaDlssPanel(oot_scale_box);
        box_layout->addWidget(dlss_panel);
#endif

        auto* upscale_hint = new QLabel(oot_scale_box);
        upscale_hint->setWordWrap(true);
        upscale_hint->setContentsMargins(0, 2, 0, 8);
        upscale_hint->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
        box_layout->addWidget(upscale_hint);

        const auto update_upscale_ui = [=](int choice) {
            if (amd_fsr4_cb != nullptr) {
                amd_fsr4_cb->setChecked(choice == UpscaleFsr4);
            }
            if (nvidia_dlss_cb != nullptr) {
                nvidia_dlss_cb->setChecked(choice == UpscaleNvidiaDlss);
            }
            if (game_dlss_cb != nullptr) {
                game_dlss_cb->setChecked(choice == UpscaleGameDlss);
            }
#ifdef EGLANTINE_STREAMLINE
            dlss_panel->setVisible(choice == UpscaleNvidiaDlss);
#endif
            switch (choice) {
            case UpscaleFsr4:
                upscale_hint->setText(
                    tr("AMD FSR 4 / 3.1 reconstructs the 3D Render Scale below using temporal "
                       "motion vectors (requires Resolution 1X and extra VRAM/GPU; keep None on 4 "
                       "GB / GTX cards). Requires game restart."));
                break;
            case UpscaleNvidiaDlss:
                upscale_hint->setText(
                    tr("NVIDIA DLSS reconstructs the 3D Render Scale below on RTX Tensor Cores "
                       "(requires Resolution 1X and extra VRAM; does not work on GTX / AMD). "
                       "Requires game restart."));
                break;
            case UpscaleGameDlss:
                upscale_hint->setText(
                    tr("Runs the Switch 2 game's own emulated DLSS network. Delivers high "
                       "sharpness on any GPU (including GTX / AMD / Intel) with a small extra GPU "
                       "cost (~5 FPS on 4 GB GPUs). Requires game restart."));
                break;
            default:
                upscale_hint->setText(
                    tr("Adjusts internal 3D resolution on ALL GPUs (NVIDIA, AMD, Intel) with zero "
                       "extra VRAM cost. Performance (50%) is recommended for 4 GB GPUs. Requires "
                       "game restart."));
                break;
            }
            upscale_hint->adjustSize();
            upscale_hint->updateGeometry();
        };

        int initial_choice = UpscaleNone;
        if (nvidia_dlss_cb != nullptr && nvidia_dlss_cb->isChecked()) {
            initial_choice = UpscaleNvidiaDlss;
        } else if (amd_fsr4_cb != nullptr && amd_fsr4_cb->isChecked()) {
            initial_choice = UpscaleFsr4;
        } else if (game_dlss_cb != nullptr && game_dlss_cb->isChecked()) {
            initial_choice = UpscaleGameDlss;
        }
        const int idx = method_combo->findData(initial_choice);
        method_combo->setCurrentIndex(idx >= 0 ? idx : 0);
        update_upscale_ui(method_combo->currentData().toInt());

        connect(method_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [method_combo, update_upscale_ui](int) {
                    update_upscale_ui(method_combo->currentData().toInt());
                });

        for (const u32 id : oot_scale_ids) {
            if (const auto it = hold_graphics.find(id); it != hold_graphics.end()) {
                box_layout->addWidget(it->second);
            }
        }
        graphics_layout.addWidget(oot_scale_box);
    }

    // 2) Built-in Frame Generation (right below Upscaling & 3D Render Scale)
    QGroupBox* fg_box = nullptr;
    for (const u32 id : fg_ids) {
        const auto it = hold_graphics.find(id);
        if (it == hold_graphics.end()) {
            continue;
        }
        if (fg_box == nullptr) {
            fg_box = new QGroupBox(tr("Frame Generation (Built-in)"), this);
            auto* box_layout = new QVBoxLayout(fg_box);
            auto* hint = new QLabel(
                tr("Duplicates visual smoothness by inserting intermediate frames. Uses extra "
                   "GPU/VRAM and works best above 30 real FPS — keep OFF if the game runs below 30 "
                   "real FPS or if base FPS drops when enabled (common on 4 GB GPUs)."),
                fg_box);
            hint->setWordWrap(true);
            box_layout->addWidget(hint);
            graphics_layout.addWidget(fg_box);
        }
        fg_box->layout()->addWidget(it->second);
    }

    // Hide rather than disable, a disabled widget would read as using the global value
    {
        std::vector<std::pair<u32, QWidget*>> fg_sub_widgets;
        for (const u32 id : {Settings::values.frame_gen_type.Id(),
                             Settings::values.amd_frame_gen_version.Id(),
                             Settings::values.frame_gen_multiplier.Id(),
                             Settings::values.frame_gen_target_rate.Id(),
                             Settings::values.frame_gen_flow_scale_auto.Id(),
                             Settings::values.frame_gen_flow_scale.Id(),
                             Settings::values.frame_gen_queue_target.Id()}) {
            if (const auto it = hold_graphics.find(id); it != hold_graphics.end()) {
                fg_sub_widgets.emplace_back(id, it->second);
            }
        }
        const auto update_fg_visibility = [frame_gen_checkbox, fg_type_combo, fg_sub_widgets]() {
            const bool fg_on = frame_gen_checkbox == nullptr || frame_gen_checkbox->isChecked();
            const bool is_amd_fsr =
                fg_type_combo != nullptr &&
                fg_type_combo->currentData().toInt() == static_cast<int>(Settings::FrameGenType::AmdFsr);
            for (const auto& [id, w] : fg_sub_widgets) {
                const bool visible =
                    fg_on && (id != Settings::values.amd_frame_gen_version.Id() || is_amd_fsr);
                w->setVisible(visible);
            }
        };
        update_fg_visibility();
        if (frame_gen_checkbox != nullptr) {
            connect(frame_gen_checkbox, &QCheckBox::toggled, this,
                    [update_fg_visibility](bool) { update_fg_visibility(); });
        }
        if (fg_type_combo != nullptr) {
            connect(fg_type_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [update_fg_visibility](int) { update_fg_visibility(); });
        }
    }

    for (const auto& [id, widget] : hold_graphics) {
        if (std::ranges::find(oot_scale_ids, id) == oot_scale_ids.end() &&
            std::ranges::find(upscale_hidden_ids, id) == upscale_hidden_ids.end() &&
            std::ranges::find(fg_ids, id) == fg_ids.end()) {
            graphics_layout.addWidget(widget);
        }
    }

    // Keep hidden settings widgets owned by graphics_layout so apply_funcs saves them cleanly.
    for (QWidget* hidden_w : {amd_fsr4_w, nvidia_dlss_w, game_dlss_w, dump_flow_w}) {
        if (hidden_w != nullptr) {
            graphics_layout.addWidget(hidden_w);
        }
    }

    for (auto widget : hold_api) {
        api_grid_layout->addWidget(widget);
    }

    // Background color is too specific to build into the new system, so we manage it here
    // (3 settings, all collected into a single widget with a QColor to manage on top)
    if (Settings::IsConfiguringGlobal()) {
        apply_funcs.push_back([this](bool powered_on) {
            Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
            Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
            Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
        });
    } else {
        QPushButton* bg_restore_button = ConfigurationShared::Widget::CreateRestoreGlobalButton(
            Settings::values.bg_red.UsingGlobal(), ui->bg_widget);
        ui->bg_widget->layout()->addWidget(bg_restore_button);

        bg_restore_button->connect(bg_restore_button, &QAbstractButton::clicked,
                                   [bg_restore_button, this](bool) {
                                       const int r = Settings::values.bg_red.GetValue(true);
                                       const int g = Settings::values.bg_green.GetValue(true);
                                       const int b = Settings::values.bg_blue.GetValue(true);
                                       UpdateBackgroundColorButton(QColor::fromRgb(r, g, b));

                                       bg_restore_button->setVisible(false);
                                       bg_restore_button->setEnabled(false);
                                   });

        ui->bg_button->connect(ui->bg_button, &QAbstractButton::clicked, [bg_restore_button](bool) {
            bg_restore_button->setVisible(true);
            bg_restore_button->setEnabled(true);
        });

        apply_funcs.push_back([bg_restore_button, this](bool powered_on) {
            const bool using_global = !bg_restore_button->isEnabled();
            Settings::values.bg_red.SetGlobal(using_global);
            Settings::values.bg_green.SetGlobal(using_global);
            Settings::values.bg_blue.SetGlobal(using_global);
            if (!using_global) {
                Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
                Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
                Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
            }
        });
    }
}

const QString ConfigureGraphics::TranslateVSyncMode(VkPresentModeKHR mode,
                                                    Settings::RendererBackend backend) const {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return (backend == Settings::RendererBackend::OpenGL_GLSL ||
                backend == Settings::RendererBackend::OpenGL_GLASM ||
                backend == Settings::RendererBackend::OpenGL_SPIRV)
                   ? tr("Off")
                   : QStringLiteral("Immediate (%1)").arg(tr("VSync Off"));
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return QStringLiteral("Mailbox (%1)").arg(tr("Recommended"));
    case VK_PRESENT_MODE_FIFO_KHR:
        return (backend == Settings::RendererBackend::OpenGL_GLSL ||
                backend == Settings::RendererBackend::OpenGL_GLASM ||
                backend == Settings::RendererBackend::OpenGL_SPIRV)
                   ? tr("On")
                   : QStringLiteral("FIFO (%1)").arg(tr("VSync On"));
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return QStringLiteral("FIFO Relaxed");
    default:
        return {};
    }
}

int ConfigureGraphics::FindIndex(u32 enumeration, int value) const {
    for (u32 i = 0; enumeration < combobox_translations.size() &&
                    i < combobox_translations.at(enumeration).size();
         i++)
        if (combobox_translations.at(enumeration)[i].first == u32(value))
            return i;
    return -1;
}

void ConfigureGraphics::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }

    UpdateVsyncSetting();

    Settings::values.vulkan_device.SetGlobal(true);
    auto const index = Settings::EnumMetadata<Settings::RendererBackend>::Index();
    if (Settings::IsConfiguringGlobal() ||
        (!Settings::IsConfiguringGlobal() && api_restore_global_button->isEnabled())) {
        auto backend =
            index >= combobox_translations.size() ||
                    size_t(api_combobox->currentIndex()) >= combobox_translations.at(index).size()
                ? Settings::values.renderer_backend.GetValue()
                : Settings::RendererBackend(
                      combobox_translations.at(index)[api_combobox->currentIndex()].first);
        switch (backend) {
        case Settings::RendererBackend::Vulkan:
            Settings::values.vulkan_device.SetGlobal(Settings::IsConfiguringGlobal());
            Settings::values.vulkan_device.SetValue(vulkan_device_combobox->currentIndex());
            break;
        case Settings::RendererBackend::OpenGL_GLSL:
        case Settings::RendererBackend::OpenGL_SPIRV:
        case Settings::RendererBackend::OpenGL_GLASM:
        case Settings::RendererBackend::Null:
            break;
        }
    }
}

void ConfigureGraphics::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphics::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphics::UpdateBackgroundColorButton(QColor color) {
    bg_color = color;

    QPixmap pixmap(ui->bg_button->size());
    pixmap.fill(bg_color);

    const QIcon color_icon(pixmap);
    ui->bg_button->setIcon(color_icon);
}

void ConfigureGraphics::UpdateAPILayout() {
    bool runtime_lock = !system.IsPoweredOn();
    bool need_global = !(Settings::IsConfiguringGlobal() || api_restore_global_button->isEnabled());
    vulkan_device = Settings::values.vulkan_device.GetValue(need_global);
    vulkan_device_widget->setEnabled(!need_global && runtime_lock);

    const auto current_backend = GetCurrentGraphicsBackend();
    const bool is_vulkan = current_backend == Settings::RendererBackend::Vulkan;
    vulkan_device_widget->setVisible(is_vulkan);
    for (auto* widget : hdr_widgets) {
        widget->setVisible(is_vulkan);
    }
    if (is_vulkan && int(vulkan_device) < vulkan_device_combobox->count()) {
        vulkan_device_combobox->setCurrentIndex(vulkan_device);
    }
}

void ConfigureGraphics::RetrieveVulkanDevices() {
    vulkan_devices.clear();
    vulkan_devices.reserve(records.size());
    device_present_modes.clear();
    device_present_modes.reserve(records.size());
    for (const auto& record : records) {
        vulkan_devices.push_back(QString::fromStdString(record.name));
        device_present_modes.push_back(record.vsync_support);

        if (record.has_broken_compute) {
            expose_compute_option();
        }
    }
}

Settings::RendererBackend ConfigureGraphics::GetCurrentGraphicsBackend() const {
    const auto selected_backend = [&]() {
        auto const index = Settings::EnumMetadata<Settings::RendererBackend>::Index();
        if (!Settings::IsConfiguringGlobal() && !api_restore_global_button->isEnabled())
            return Settings::values.renderer_backend.GetValue(true);
        return index >= combobox_translations.size() || size_t(api_combobox->currentIndex()) >=
                                                            combobox_translations.at(index).size()
                   ? Settings::values.renderer_backend.GetValue()
                   : Settings::RendererBackend(
                         combobox_translations.at(index).at(api_combobox->currentIndex()).first);
    }();

    if (selected_backend == Settings::RendererBackend::Vulkan &&
        UISettings::values.has_broken_vulkan)
        return Settings::RendererBackend::OpenGL_GLSL;
    return selected_backend;
}
