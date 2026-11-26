// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure.h"
#include "yuzu/util/fit_window.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_debug_tab.h"
#include "yuzu/configuration/configure_dialog.h"
#include "yuzu/configuration/configure_filesystem.h"
#include "yuzu/configuration/configure_general.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_hotkeys.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_profile_manager.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/configure_ui.h"
#include "yuzu/configuration/configure_web.h"
#include "yuzu/hotkeys.h"

ConfigureDialog::ConfigureDialog(QWidget* parent, HotkeyRegistry& registry_,
                                 InputCommon::InputSubsystem* input_subsystem,
                                 std::vector<VkDeviceInfo::Record>& vk_device_records,
                                 Core::System& system_, bool enable_web_config)
    : QDialog(parent), ui{std::make_unique<Ui::ConfigureDialog>()}, registry(registry_),
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      applets_tab{std::make_unique<ConfigureApplets>(system_, nullptr, *builder, this)},
      audio_tab{std::make_unique<ConfigureAudio>(system_, nullptr, *builder, this)},
      cpu_tab{std::make_unique<ConfigureCpu>(system_, nullptr, *builder, this)},
      debug_tab_tab{std::make_unique<ConfigureDebugTab>(system_, this)},
      filesystem_tab{std::make_unique<ConfigureFilesystem>(this)},
      general_tab{std::make_unique<ConfigureGeneral>(system_, nullptr, *builder, this)},
      graphics_advanced_tab{
          std::make_unique<ConfigureGraphicsAdvanced>(system_, nullptr, *builder, this)},
      graphics_extensions_tab{
          std::make_unique<ConfigureGraphicsExtensions>(system_, nullptr, *builder, this)},
      ui_tab{std::make_unique<ConfigureUi>(system_, this)},
      graphics_tab{std::make_unique<ConfigureGraphics>(
          system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
          [this](Settings::AspectRatio ratio, Settings::ResolutionSetup setup) {
              ui_tab->UpdateScreenshotInfo(ratio, setup);
          },
          nullptr, *builder, this)},
      hotkeys_tab{std::make_unique<ConfigureHotkeys>(system_.HIDCore(), this)},
      input_tab{std::make_unique<ConfigureInput>(system_, this)},
      network_tab{std::make_unique<ConfigureNetwork>(system_, this)},
      profile_tab{std::make_unique<ConfigureProfileManager>(system_, this)},
      system_tab{std::make_unique<ConfigureSystem>(system_, nullptr, *builder, this)},
      web_tab{std::make_unique<ConfigureWeb>(this)} {
    Settings::SetConfiguringGlobal(true);

    ui->setupUi(this);
    setWindowTitle(tr("Settings"));

    ui->tabWidget->addTab(applets_tab.get(), tr("Applets"));
    ui->tabWidget->addTab(audio_tab.get(), tr("Audio"));
    ui->tabWidget->addTab(cpu_tab.get(), tr("CPU"));
    ui->tabWidget->addTab(debug_tab_tab.get(), tr("Debug"));
    ui->tabWidget->addTab(filesystem_tab.get(), tr("Filesystem"));
    ui->tabWidget->addTab(general_tab.get(), tr("General"));
    ui->tabWidget->addTab(graphics_tab.get(), tr("Graphics"));
    ui->tabWidget->addTab(graphics_advanced_tab.get(), tr("GraphicsAdvanced"));
    ui->tabWidget->addTab(graphics_extensions_tab.get(), tr("GraphicsExtra"));

    // FritCheeLii Extra+ (verified performance & 4 GB VRAM optimizations)
    graphics_extra_plus_tab = std::make_unique<QWidget>(this);
    graphics_extra_plus_tab->setAccessibleName(tr("Extra+"));
    {
        auto* outer = new QVBoxLayout(graphics_extra_plus_tab.get());
        outer->setContentsMargins(0, 0, 0, 0);
        auto* scroll = new QScrollArea(graphics_extra_plus_tab.get());
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll);
        auto* v = new QVBoxLayout(content);
        v->setSpacing(10);

        auto* intro = new QLabel(
            tr("Verified performance & stability optimizations (Fritchii v6.1/v6.2 + Longshot "
               "v3.2.0).\n"
               "Enabled by default for Entry / 4 GB VRAM PCs based on A/B benchmarks. "
               "Restart the game after toggling to apply."),
            content);
        intro->setWordWrap(true);
        v->addWidget(intro);

        auto make_group = [&](const QString& title) {
            auto* gb = new QGroupBox(title, content);
            auto* gl = new QVBoxLayout(gb);
            gl->setSpacing(6);
            v->addWidget(gb);
            return std::pair<QGroupBox*, QVBoxLayout*>{gb, gl};
        };

        auto add_switch = [](QWidget* parent_box, QVBoxLayout* box_layout, const QString& title,
                             const QString& desc, bool checked) {
            auto* cb = new QCheckBox(title, parent_box);
            cb->setChecked(checked);
            auto* note = new QLabel(desc, parent_box);
            note->setWordWrap(true);
            note->setContentsMargins(24, 0, 0, 4);
            note->setStyleSheet(QStringLiteral("color: #9a9aa8; font-size: 11px;"));
            box_layout->addWidget(cb);
            box_layout->addWidget(note);
            return cb;
        };

        auto [gb_vram, lay_vram] = make_group(tr("VRAM & Textures (4 GB GPUs)"));
        auto* cb_mip = add_switch(
            gb_vram, lay_vram, tr("4K Texture Mip Cap (4 GB VRAM Saver)"),
            tr("Skips mip level 0 on large mipmapped textures to reduce VRAM bandwidth & pressure.\n"
               "Recommended for: 4 GB GPUs (e.g. GTX 1650, GTX 1050 Ti, RX 570) at 720p/1080p. "
               "Requires game restart."),
            Settings::values.test_4k_mip_cap.GetValue());
        auto* cb_vram_gc = add_switch(
            gb_vram, lay_vram, tr("4 GB VRAM Smart Texture GC (Fritchii v6.2 + NX2navi)"),
            tr("Prevents hot-texture thrashing (12-frame floor), skips un-evictable ASTC textures "
               "without wasting GC budget, and recycles dead render targets faster past 80% VRAM. "
               "Recommended for 4 GB GPUs (e.g. GTX 1650)."),
            Settings::values.test_vram_gc_4gb.GetValue());
        auto* cb_tidy = add_switch(
            gb_vram, lay_vram, tr("Texture Heap Tidy-Up Once a Second (Fritchii v6.2)"),
            tr("Throttles OoT's texture heap compaction from every frame to once per second, "
               "avoiding repeated texture re-uploads and VRAM churn on 4 GB GPUs. "
               "Requires game restart."),
            Settings::values.test_tex_tidy_1s.GetValue());
        auto* cb_shadow = add_switch(
            gb_vram, lay_vram, tr("Shadow Size Steps (Built-in OoT Patch)"),
            tr("Rounds OoT local shadow map sizes up to multiples of 64 px so the image pool "
               "reuses them instead of creating thousands of shadow textures while moving in "
               "Castle Town / Guard House. Requires game restart."),
            Settings::values.test_shadow_size_steps.GetValue());

        auto [gb_gpu, lay_gpu] = make_group(tr("GPU, ASTC & Readback Anti-Stall"));
        auto* cb_fast_astc = add_switch(
            gb_gpu, lay_gpu, tr("Fast ASTC GPU Decoder (Longshot LDS + Loop Hoisting)"),
            tr("Uses Longshot's optimized ASTC compute decoder (LDS scratchpad + loop hoisting, "
               "~5.3x faster on non-ASTC desktop GPUs like GTX 1650 / RTX / AMD / Intel). "
               "Requires game restart."),
            Settings::values.test_fast_astc_decoder.GetValue());
        auto* cb_byte_rb = add_switch(
            gb_gpu, lay_gpu, tr("Exact Byte Readback — Culling Anti-Stall (Longshot #2366)"),
            tr("Checks exact byte-level GPU-modified ranges in BufferCache::GetFlushArea instead "
               "of 4 KB page alignment to avoid false GPU->CPU stalls on double-buffered OoT "
               "culling tables."),
            Settings::values.test_exact_byte_readback.GetValue());
        auto* cb_occl = add_switch(
            gb_gpu, lay_gpu, tr("Occlusion Off (Built-in OoT Patch)"),
            tr("Disables OoT's GPU occlusion queries and conditional renders to skip GPU->CPU "
               "readback stalls. Requires game restart."),
            Settings::values.test_occlusion_off.GetValue());
        auto* cb_crash_skip = add_switch(
            gb_gpu, lay_gpu, tr("Shader Precache Crash-Skip Journal (Longshot #2323)"),
            tr("Records in-flight shader hashes during precache so if a GPU driver hangs or "
               "crashes on a single shader, the next launch automatically skips only that shader."),
            Settings::values.test_precache_crash_skip.GetValue());

        auto [gb_cpu, lay_cpu] = make_group(tr("CPU & Area Loading (4-Core Optimizations)"));
        auto* cb_poll = add_switch(
            gb_cpu, lay_cpu, tr("Resource Poll Backoff (Built-in OoT Patch)"),
            tr("Replaces OoT's busy-spin resource polling loop with a 500 us sleep (svc 0xF3), "
               "freeing CPU core cycles on 4-core CPUs (e.g. i3-10105F). Requires game restart."),
            Settings::values.test_poll_backoff.GetValue());
        auto* cb_changebank = add_switch(
            gb_cpu, lay_cpu, tr("Neighbour Map Preload Hold — 2s (Built-in OoT Patch)"),
            tr("Holds OoT's preloaded neighbouring area for 2 seconds when walking or turning in "
               "overlapping areas like Hyrule Market / Field (NX2Saria), avoiding repeated "
               "cancelled background loads on 4-core CPUs. Requires game restart."),
            Settings::values.test_changebank_hold.GetValue());
        auto* cb_jit = add_switch(
            gb_cpu, lay_cpu, tr("JIT Block List Warm-Up (Fritchii)"),
            tr("Records compiled ARM64 blocks per game session and pre-warms them on idle cores "
               "at next launch to reduce CPU micro-stutters in Hyrule Field & Castle Town Market. "
               "Requires game restart."),
            Settings::values.test_jit_block_warmup.GetValue());
        auto* cb_zstd = add_switch(
            gb_cpu, lay_cpu, tr("Native Host Zstd Unpacking (Fritchii v6.2)"),
            tr("Intercepts OoT's Zstd decompression calls in the JIT and runs native host libzstd "
               "for faster area loads and lower CPU load. Requires game restart."),
            Settings::values.test_host_zstd.GetValue());
        auto* cb_ubo_miss = add_switch(
            gb_cpu, lay_cpu, tr("UBO Reuse Miss-Streak Bypass (Fritchii v6.5 EXP115)"),
            tr("After 8 consecutive misses on a per-draw uniform slot, streams directly for the "
               "next 64 binds without memcmp or extra slot copies. Reduces CPU load on dynamic "
               "matrices. Takes effect immediately."),
            Settings::values.test_ubo_miss_bypass.GetValue());

        v->addStretch(1);
        scroll->setWidget(content);
        outer->addWidget(scroll);

        apply_graphics_extra_plus = [cb_mip, cb_shadow, cb_poll, cb_changebank, cb_jit, cb_occl,
                                     cb_tidy, cb_zstd, cb_ubo_miss, cb_vram_gc, cb_fast_astc,
                                     cb_byte_rb, cb_crash_skip]() {
            Settings::values.test_4k_mip_cap.SetValue(cb_mip->isChecked());
            Settings::values.test_shadow_size_steps.SetValue(cb_shadow->isChecked());
            Settings::values.test_poll_backoff.SetValue(cb_poll->isChecked());
            Settings::values.test_changebank_hold.SetValue(cb_changebank->isChecked());
            Settings::values.test_jit_block_warmup.SetValue(cb_jit->isChecked());
            Settings::values.test_occlusion_off.SetValue(cb_occl->isChecked());
            Settings::values.test_tex_tidy_1s.SetValue(cb_tidy->isChecked());
            Settings::values.test_host_zstd.SetValue(cb_zstd->isChecked());
            Settings::values.test_ubo_miss_bypass.SetValue(cb_ubo_miss->isChecked());
            Settings::values.test_vram_gc_4gb.SetValue(cb_vram_gc->isChecked());
            Settings::values.test_fast_astc_decoder.SetValue(cb_fast_astc->isChecked());
            Settings::values.test_exact_byte_readback.SetValue(cb_byte_rb->isChecked());
            Settings::values.test_precache_crash_skip.SetValue(cb_crash_skip->isChecked());
        };
    }
    ui->tabWidget->addTab(graphics_extra_plus_tab.get(), tr("Extra+"));

    // FritCheeLii Test Lab (experimental A/B switches, OFF by default)
    graphics_test_tab = std::make_unique<QWidget>(this);
    graphics_test_tab->setAccessibleName(tr("Test"));
    {
        auto* outer = new QVBoxLayout(graphics_test_tab.get());
        outer->setContentsMargins(0, 0, 0, 0);
        auto* scroll = new QScrollArea(graphics_test_tab.get());
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll);
        auto* v = new QVBoxLayout(content);
        v->setSpacing(8);

        auto* info = new QLabel(
            tr("FritCheeLii Test Lab — new A/B experimental switches (all start OFF).\n"
               "Previously verified switches were promoted to Graphics > Extra+. "
               "Use the 30s Benchmark Recorder to measure impact before promoting."),
            content);
        info->setWordWrap(true);
        v->addWidget(info);

        auto add_test_switch = [&](const QString& title, const QString& desc, bool checked) {
            auto* cb = new QCheckBox(title, content);
            cb->setChecked(checked);
            auto* note = new QLabel(desc, content);
            note->setWordWrap(true);
            note->setContentsMargins(24, 0, 0, 4);
            note->setStyleSheet(QStringLiteral("color: #9a9aa8; font-size: 11px;"));
            v->addWidget(cb);
            v->addWidget(note);
            return cb;
        };

        auto* cb_ubo_16k = add_test_switch(
            tr("16 KiB Bone Palette UBO Streaming (Fritchii v6.5 EXP116)"),
            tr("Raises Vulkan uniform stream reuse cap from 4 KiB to 16 KiB so Ocarina of Time's "
               "7–13 KiB skinned bone palettes reuse stream allocations instead of hitting cached "
               "buffers. Requires game restart."),
            Settings::values.test_ubo_16k_stream.GetValue());

        auto* cb_mem_prio = add_test_switch(
            tr("VRAM Memory Priority — Render Targets First (Longshot)"),
            tr("Uses VK_EXT_memory_priority to assign high residency priority (1.0) to render "
               "targets and low priority (0.25) to sampled textures so Windows keeps framebuffers "
               "in fast VRAM under memory pressure. Requires game restart."),
            Settings::values.test_vram_memory_priority.GetValue());

        v->addStretch(1);
        scroll->setWidget(content);
        outer->addWidget(scroll);

        apply_graphics_test = [cb_ubo_16k, cb_mem_prio]() {
            Settings::values.test_ubo_16k_stream.SetValue(cb_ubo_16k->isChecked());
            Settings::values.test_vram_memory_priority.SetValue(cb_mem_prio->isChecked());
        };
    }
    ui->tabWidget->addTab(graphics_test_tab.get(), tr("Test"));
    ui->tabWidget->addTab(hotkeys_tab.get(), tr("Hotkeys"));
    ui->tabWidget->addTab(input_tab.get(), tr("Controls"));
    ui->tabWidget->addTab(profile_tab.get(), tr("Profiles"));
    ui->tabWidget->addTab(network_tab.get(), tr("Network"));
    ui->tabWidget->addTab(system_tab.get(), tr("System"));
    ui->tabWidget->addTab(ui_tab.get(), tr("Game List"));
    ui->tabWidget->addTab(web_tab.get(), tr("Web"));

    web_tab->SetWebServiceConfigEnabled(enable_web_config);
    hotkeys_tab->Populate(registry);

    input_tab->Initialize(input_subsystem);

    general_tab->SetResetCallback([&] { this->close(); });

    SetConfiguration();
    PopulateSelectionList();

    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
        if (index != -1) {
            debug_tab_tab->SetCurrentIndex(0);
        }
    });
    connect(ui_tab.get(), &ConfigureUi::LanguageChanged, this, &ConfigureDialog::OnLanguageChanged);
    connect(general_tab.get(), &ConfigureGeneral::ExternalContentDirsChanged, this,
            &ConfigureDialog::ExternalContentDirsChanged);
    connect(ui->selectorList, &QListWidget::itemSelectionChanged, this,
            &ConfigureDialog::UpdateVisibleTabs);

    // One button for players who don't want to go through the tabs: picks the settings that fit
    // this PC's graphics card and memory (with a way back), instead of what is in this window.
    {
        QPushButton* const best = ui->buttonBox->addButton(
            tr("Find Best Settings for This PC..."), QDialogButtonBox::ResetRole);
        best->setToolTip(
            tr("Closes this window and offers the settings FritCheeLii recommends for this "
               "graphics card and memory. Your own settings are kept: Help > Restore My "
               "Previous Settings brings them back."));
        best->setEnabled(!system.IsPoweredOn());
        if (system.IsPoweredOn()) {
            best->setToolTip(tr("Stop the game first."));
        }
        connect(best, &QAbstractButton::clicked, this, [this] {
            best_settings_requested = true;
            reject();
        });
    }

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigureDialog::HandleApplyButtonClicked);
    }

    // The pages scroll, so the window can be smaller than its longest page and its buttons stay
    // reachable on a small screen: it opens at the size it always had, or at what the screen has.
    adjustSize();
    const QSize natural = size();
    FitWindow::ScrollWrap(ui->tabWidget);
    FitWindow::OpenAt(this, natural);
    ui->selectorList->setCurrentRow(0);

    // Selects the leftmost button on the bottom bar (Cancel as of writing)
    ui->buttonBox->setFocus();
}

ConfigureDialog::~ConfigureDialog() = default;

void ConfigureDialog::SetConfiguration() {}

void ConfigureDialog::ApplyConfiguration() {
    general_tab->ApplyConfiguration();
    ui_tab->ApplyConfiguration();
    system_tab->ApplyConfiguration();
    profile_tab->ApplyConfiguration();
    filesystem_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    hotkeys_tab->ApplyConfiguration(registry);
    cpu_tab->ApplyConfiguration();
    graphics_tab->ApplyConfiguration();
    graphics_advanced_tab->ApplyConfiguration();
    graphics_extensions_tab->ApplyConfiguration();
    if (apply_graphics_extra_plus) {
        apply_graphics_extra_plus();
    }
    if (apply_graphics_test) {
        apply_graphics_test();
    }
    audio_tab->ApplyConfiguration();
    debug_tab_tab->ApplyConfiguration();
    web_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();
    system.ApplySettings();
    Settings::LogSettings();
}

void ConfigureDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigureDialog::RetranslateUI() {
    const int old_row = ui->selectorList->currentRow();
    const int old_index = ui->tabWidget->currentIndex();

    ui->retranslateUi(this);

    PopulateSelectionList();
    ui->selectorList->setCurrentRow(old_row);

    UpdateVisibleTabs();
    ui->tabWidget->setCurrentIndex(old_index);
}

void ConfigureDialog::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

Q_DECLARE_METATYPE(QList<QWidget*>);

void ConfigureDialog::PopulateSelectionList() {
    const std::array<std::pair<QString, QList<QWidget*>>, 6> items{
        {{tr("General"),
          {general_tab.get(), hotkeys_tab.get(), ui_tab.get(), web_tab.get(), debug_tab_tab.get()}},
         {tr("System"),
          {system_tab.get(), profile_tab.get(), network_tab.get(), filesystem_tab.get(),
           applets_tab.get()}},
         {tr("CPU"), {cpu_tab.get()}},
         {tr("Graphics"),
          {graphics_tab.get(), graphics_advanced_tab.get(), graphics_extensions_tab.get(),
           graphics_extra_plus_tab.get(), graphics_test_tab.get()}},
         {tr("Audio"), {audio_tab.get()}},
         {tr("Controls"), input_tab->GetSubTabs()}},
    };

    [[maybe_unused]] const QSignalBlocker blocker(ui->selectorList);

    ui->selectorList->clear();
    for (const auto& entry : items) {
        auto* const item = new QListWidgetItem(entry.first);
        item->setData(Qt::UserRole, QVariant::fromValue(entry.second));

        ui->selectorList->addItem(item);
    }
}

void ConfigureDialog::OnLanguageChanged(const QString& locale) {
    emit LanguageChanged(locale);
    //  Reloading the game list is needed to force retranslation.
    UISettings::values.is_game_list_reload_pending = true;
    // first apply the configuration, and then restore the display
    ApplyConfiguration();
    RetranslateUI();
    SetConfiguration();
}

void ConfigureDialog::UpdateVisibleTabs() {
    const auto items = ui->selectorList->selectedItems();
    if (items.isEmpty()) {
        return;
    }

    [[maybe_unused]] const QSignalBlocker blocker(ui->tabWidget);

    ui->tabWidget->clear();

    const auto tabs = qvariant_cast<QList<QWidget*>>(items[0]->data(Qt::UserRole));

    for (auto* const tab : tabs) {
        LOG_DEBUG(Frontend, "{}", tab->accessibleName().toStdString());
        ui->tabWidget->addTab(tab, tab->accessibleName());
    }
}
