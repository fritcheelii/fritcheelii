// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
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

    // FritCheeLii Test Lab (clean, isolated A/B switches, off by default)
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

        auto* intro = new QLabel(
            tr("Experimental performance switches for A/B testing (Fritchii v6.1/v6.2 ports).\n"
               "All switches start OFF so they never interfere with standard settings. "
               "Restart the game after toggling to measure VRAM & FPS impact."),
            content);
        intro->setWordWrap(true);
        v->addWidget(intro);

        auto add_switch = [&](const QString& title, const QString& desc, bool checked) {
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

        auto* cb_mip = add_switch(
            tr("4K Texture Mip Cap (4 GB VRAM Saver)"),
            tr("Skips mip level 0 on large mipmapped textures to reduce VRAM bandwidth & pressure.\n"
               "Recommended for: 4 GB GPUs (e.g. GTX 1650, GTX 1050 Ti, RX 570) at 720p/1080p. "
               "Requires game restart."),
            Settings::values.test_4k_mip_cap.GetValue());

        auto* cb_shadow = add_switch(
            tr("Shadow Size Steps (Built-in OoT Patch)"),
            tr("Rounds OoT local shadow map sizes up to multiples of 64 px so the image pool "
               "reuses them instead of creating thousands of shadow textures while moving in "
               "Castle Town / Guard House. Requires game restart."),
            Settings::values.test_shadow_size_steps.GetValue());

        auto* cb_poll = add_switch(
            tr("Resource Poll Backoff (Built-in OoT Patch)"),
            tr("Replaces OoT's busy-spin resource polling loop with a 500 us sleep (svc 0xF3), "
               "freeing CPU core cycles on 4-core CPUs (e.g. i3-10105F). Requires game restart."),
            Settings::values.test_poll_backoff.GetValue());

        auto* cb_changebank = add_switch(
            tr("Neighbour Map Preload Hold — 2s (Built-in OoT Patch)"),
            tr("Holds OoT's preloaded neighbouring area for 2 seconds when walking or turning in "
               "overlapping areas like Hyrule Market / Field (NX2Saria), avoiding repeated "
               "cancelled background loads on 4-core CPUs. Requires game restart."),
            Settings::values.test_changebank_hold.GetValue());

        auto* cb_jit = add_switch(
            tr("JIT Block List Warm-Up (Fritchii)"),
            tr("Records compiled ARM64 blocks per game session and pre-warms them on idle cores "
               "at next launch to reduce CPU micro-stutters in Hyrule Field & Castle Town Market. "
               "Requires game restart."),
            Settings::values.test_jit_block_warmup.GetValue());

        auto* cb_occl = add_switch(
            tr("Occlusion Off (Built-in OoT Patch)"),
            tr("Disables OoT's GPU occlusion queries and conditional renders to skip GPU->CPU "
               "readback stalls. Great A/B test in Castle Town Market. Requires game restart."),
            Settings::values.test_occlusion_off.GetValue());

        auto* cb_tidy = add_switch(
            tr("Texture Heap Tidy-Up Once a Second (Fritchii v6.2)"),
            tr("Throttles OoT's texture heap compaction from every frame to once per second, "
               "avoiding repeated texture re-uploads and VRAM churn on 4 GB GPUs. "
               "Requires game restart."),
            Settings::values.test_tex_tidy_1s.GetValue());

        auto* cb_zstd = add_switch(
            tr("Native Host Zstd Unpacking (Fritchii v6.2)"),
            tr("Intercepts OoT's Zstd decompression calls in the JIT and runs native host libzstd "
               "for faster area loads and lower CPU load. Requires game restart."),
            Settings::values.test_host_zstd.GetValue());

        auto* cb_vram_gc = add_switch(
            tr("4 GB VRAM Smart Texture GC (Fritchii v6.2 + NX2navi)"),
            tr("Prevents hot-texture thrashing (12-frame floor), skips un-evictable ASTC textures "
               "without wasting GC budget, and recycles dead render targets faster past 80% VRAM. "
               "Recommended for 4 GB GPUs (e.g. GTX 1650)."),
            Settings::values.test_vram_gc_4gb.GetValue());

        v->addStretch(1);
        scroll->setWidget(content);
        outer->addWidget(scroll);

        apply_graphics_test = [cb_mip, cb_shadow, cb_poll, cb_changebank, cb_jit, cb_occl, cb_tidy,
                               cb_zstd, cb_vram_gc]() {
            Settings::values.test_4k_mip_cap.SetValue(cb_mip->isChecked());
            Settings::values.test_shadow_size_steps.SetValue(cb_shadow->isChecked());
            Settings::values.test_poll_backoff.SetValue(cb_poll->isChecked());
            Settings::values.test_changebank_hold.SetValue(cb_changebank->isChecked());
            Settings::values.test_jit_block_warmup.SetValue(cb_jit->isChecked());
            Settings::values.test_occlusion_off.SetValue(cb_occl->isChecked());
            Settings::values.test_tex_tidy_1s.SetValue(cb_tidy->isChecked());
            Settings::values.test_host_zstd.SetValue(cb_zstd->isChecked());
            Settings::values.test_vram_gc_4gb.SetValue(cb_vram_gc->isChecked());
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
           graphics_test_tab.get()}},
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
