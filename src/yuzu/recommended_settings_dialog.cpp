// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <QComboBox>
#include <QDialog>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include "qt_common/config/shared_translation.h"
#include "yuzu/recommended_settings_dialog.h"

namespace {

// One "name: previous value → new value" line per change, worded as the settings dialog words
// them.
QStringList Describe(QWidget* parent,
                     const std::vector<Settings::RecommendedSettingChange>& changes,
                     const QString& on, const QString& off) {
    const auto titles = ConfigurationShared::InitializeTranslations(parent);
    const auto enums = ConfigurationShared::ComboboxEnumeration(parent);

    const auto title_of = [&](const Settings::BasicSetting& setting) {
        const auto it = titles->find(setting.Id());
        return it == titles->end() ? QString{} : it->second.first;
    };
    const auto text_of = [&](const Settings::BasicSetting& setting, const std::string& value) {
        if (setting.TypeId() == "bool") {
            return value == "true" ? on : off;
        }
        if (const auto it = enums->find(setting.EnumIndex()); it != enums->end()) {
            for (const auto& [id, text] : it->second) {
                if (std::to_string(id) == value) {
                    return text;
                }
            }
        }
        QString text = QString::fromStdString(value);
        if (setting.Specialization() & Settings::Specialization::Percentage) {
            text.append(QLatin1Char{'%'});
        }
        return text;
    };

    QStringList lines;
    for (const auto& change : changes) {
        QString title = title_of(*change.setting);
        if (title.isEmpty()) {
            // A checkbox paired with a value is named by the value's setting.
            for (const auto& [key, other] : Settings::values.linkage.by_key) {
                if (other->PairedSetting() == change.setting) {
                    title = title_of(*other);
                }
            }
        }
        if (title.isEmpty()) {
            title = QString::fromStdString(change.setting->GetLabel());
        }
        if (title.endsWith(QLatin1Char{':'})) {
            title.chop(1);
        }
        lines.append(QStringLiteral("%1: %2 \u2192 %3")
                         .arg(title, text_of(*change.setting, change.previous),
                              text_of(*change.setting, change.current)));
    }
    return lines;
}

void ShowChanges(QWidget* parent, const QString& title, const QString& text,
                 const QStringList& lines, const QString& footer) {
    QMessageBox box(QMessageBox::Information, title, text, QMessageBox::Ok, parent);
    // A long list goes behind the details button so the box stays on the screen.
    if (lines.size() <= 12) {
        // Rich text, so that a line is not wrapped in the middle of a value.
        QString list;
        for (const QString& line : lines) {
            list += QStringLiteral("<nobr>%1</nobr><br>").arg(line.toHtmlEscaped());
        }
        box.setInformativeText(list + QStringLiteral("<br>") + footer.toHtmlEscaped());
    } else {
        box.setInformativeText(footer);
        box.setDetailedText(lines.join(QLatin1Char{'\n'}));
    }
    box.exec();
}

} // namespace

RecommendedSettingsDialog::PresetChoice RecommendedSettingsDialog::ChoosePreset(
    QWidget* parent, const QString& host_text) {
    QDialog dlg(parent);
    dlg.setWindowTitle(tr("Best settings for this PC"));
    dlg.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dlg.setMinimumWidth(480);

    auto* layout = new QVBoxLayout(&dlg);
    layout->setSpacing(10);

    auto* title_lbl =
        new QLabel(tr("<b>Use the settings FritCheeLii recommends for this PC?</b>"), &dlg);
    layout->addWidget(title_lbl);

    auto* info_lbl = new QLabel(
        tr("%1\n\nThese configure performance options, AMD FSR, frame generation, and Ocarina of "
           "Time's render size. You can return to defaults via Help > Restore Default Settings.")
            .arg(host_text),
        &dlg);
    info_lbl->setWordWrap(true);
    layout->addWidget(info_lbl);

    layout->addWidget(new QLabel(tr("Preset to apply:"), &dlg));
    auto* combo = new QComboBox(&dlg);
    combo->addItems({
        tr("Auto — Recommended for This PC"),
        tr("Entry / 4–6 GB VRAM (Max FPS — Handheld, Balanced FSR, BC3)"),
        tr("Mid-Range / 8 GB VRAM (Balanced — Quality FSR, BC3)"),
        tr("High-End / 12–16 GB+ VRAM (Best Visuals — Native/Quality, Uncompressed)"),
    });
    layout->addWidget(combo);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* apply_btn = new QPushButton(tr("Apply settings"), &dlg);
    auto* cancel_btn = new QPushButton(tr("Not now"), &dlg);
    apply_btn->setDefault(true);
    QFont bold = apply_btn->font();
    bold.setBold(true);
    apply_btn->setFont(bold);
    apply_btn->setMinimumWidth(QFontMetrics{bold}.horizontalAdvance(apply_btn->text()) + 36);
    btns->addWidget(apply_btn);
    btns->addWidget(cancel_btn);
    layout->addLayout(btns);

    QObject::connect(apply_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancel_btn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) {
        return PresetChoice::Cancel;
    }
    return static_cast<PresetChoice>(combo->currentIndex());
}

bool RecommendedSettingsDialog::Ask(QWidget* parent, const QString& host_text) {
    return ChoosePreset(parent, host_text) != PresetChoice::Cancel;
}

void RecommendedSettingsDialog::ShowApplied(
    QWidget* parent, const std::vector<Settings::RecommendedSettingChange>& changes) {
    if (changes.empty()) {
        QMessageBox::information(
            parent, tr("Recommended settings"),
            tr("Your settings already match the recommended settings. Nothing changed."));
        return;
    }
    ShowChanges(parent, tr("Recommended settings"), tr("FritCheeLii now uses the recommended settings."),
                Describe(parent, changes, tr("On"), tr("Off")),
                tr("To return to the optimized defaults, use Help > Restore Default Settings."));
}

void RecommendedSettingsDialog::ShowRestored(
    QWidget* parent, const std::vector<Settings::RecommendedSettingChange>& changes) {
    if (changes.empty()) {
        QMessageBox::information(
            parent, tr("Default settings"),
            tr("Your settings already match FritCheeLii's default settings."));
        return;
    }
    ShowChanges(parent, tr("Default settings"),
                tr("FritCheeLii has restored the optimized default settings."),
                Describe(parent, changes, tr("On"), tr("Off")),
                tr("To auto-detect settings for your hardware again, use Help > Find Best "
                   "Settings for This PC..."));
}
