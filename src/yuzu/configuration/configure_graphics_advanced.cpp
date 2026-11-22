// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <system_error>
#include <vector>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <qnamespace.h>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/shared_translation.h"
#include "ui_configure_graphics_advanced.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsAdvanced::ConfigureGraphicsAdvanced(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphicsAdvanced>()}, system{system_} {

    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();

    checkbox_enable_compute_pipelines->setVisible(false);
}

ConfigureGraphicsAdvanced::~ConfigureGraphicsAdvanced() = default;

void ConfigureGraphicsAdvanced::SetConfiguration() {}

void ConfigureGraphicsAdvanced::Setup(const ConfigurationShared::Builder& builder) {
    auto& normal_layout = *ui->normal_target->layout();

    // A map will sort the data for us
    std::map<u32, QWidget*> normal_hold{};

    for (auto setting :
         Settings::values.linkage.by_category[Settings::Category::RendererAdvanced]) {
        ConfigurationShared::Widget* widget = builder.BuildWidget(setting, apply_funcs);

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        const auto id = setting->Id();

        normal_hold.emplace(id, widget);

        // Keep track of enable_compute_pipelines so we can display it when needed
        if (setting->Id() == Settings::values.enable_compute_pipelines.Id()) {
            checkbox_enable_compute_pipelines = widget;
        }
    }

    for (const auto& [id, widget] : normal_hold) {
        normal_layout.addWidget(widget);
    }

    auto* reset_shader_btn = new QPushButton(tr("Reset Shader Cache..."), this);
    reset_shader_btn->setToolTip(
        tr("Deletes cached shaders and precompiled shader markers to fix graphical glitches or "
           "corrupted caches. Your saves and settings are never touched."));
    connect(reset_shader_btn, &QPushButton::clicked, this, [this] {
        if (system.IsPoweredOn()) {
            QMessageBox::information(
                this, tr("Reset Shader Cache"),
                tr("Stop the running game first before resetting the shader cache."));
            return;
        }
        const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
        const auto reply = QMessageBox::question(
            this, tr("Reset Shader Cache"),
            tr("Delete all cached shaders in:\n%1\n\n"
               "Your saves and settings will NOT be affected. Shaders will be rebuilt cleanly "
               "the next time you launch a game.")
                .arg(QString::fromStdString(Common::FS::PathToUTF8String(shader_dir))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return;
        }
        std::error_code ec;
        std::uintmax_t removed_files = 0;
        std::uintmax_t removed_bytes = 0;
        if (std::filesystem::exists(shader_dir, ec)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(shader_dir, ec)) {
                if (entry.is_regular_file(ec)) {
                    removed_bytes += entry.file_size(ec);
                    ++removed_files;
                }
            }
            for (const auto& entry : std::filesystem::directory_iterator(shader_dir, ec)) {
                std::filesystem::remove_all(entry.path(), ec);
            }
        }
        const double mb = static_cast<double>(removed_bytes) / (1024.0 * 1024.0);
        QMessageBox::information(
            this, tr("Reset Shader Cache"),
            tr("Shader cache cleared (%1 files, %2 MB freed).")
                .arg(static_cast<qulonglong>(removed_files))
                .arg(mb, 0, 'f', 1));
    });
    normal_layout.addWidget(reset_shader_btn);
}

void ConfigureGraphicsAdvanced::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureGraphicsAdvanced::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphicsAdvanced::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphicsAdvanced::ExposeComputeOption() {
    checkbox_enable_compute_pipelines->setVisible(true);
}
