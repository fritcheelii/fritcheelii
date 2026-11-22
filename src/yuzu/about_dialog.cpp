// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QIcon>
#include <fmt/ranges.h>
#include "common/scm_rev.h"
#include "ui_aboutdialog.h"
#include "yuzu/about_dialog.h"
#include "yuzu/fritchii/fritchii_ui.h"

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent), ui{std::make_unique<Ui::AboutDialog>()} {
    static const std::string build_line =
        fmt::format("{} | {}", std::string{Common::g_build_version},
                    std::string{Common::g_compiler_id});

    ui->setupUi(this);
    setWindowTitle(tr("About"));
    ui->labelYuzu->hide();
    ui->labelLinks->hide();
    const QIcon yuzu_logo = QIcon(QStringLiteral(":/img/cheezyemu.ico"));
    if (!yuzu_logo.isNull()) {
        ui->labelLogo->setPixmap(yuzu_logo.pixmap(180));
    }
    ui->labelBuildInfo->setText(
        tr("<span style='font-size:26px'><b>FritCheeLii %1</b></span><br>"
           "%2 (%3)")
            .arg(QString::fromLatin1(FritchiiUi::Version),
                 QString::fromStdString(build_line),
                 QString::fromUtf8(Common::g_build_date).left(10)));
}

AboutDialog::~AboutDialog() = default;
