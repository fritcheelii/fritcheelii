// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/saves_window.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <JlCompress.h>
#include <fmt/format.h>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/common_types.h"

namespace FritchiiUi {
namespace {
namespace fs = std::filesystem;

/// A profile folder's saves: <nand>/user/save/0000000000000000 (one folder per user profile id,
/// one per game inside).
struct Place {
    QString name;
    fs::path root;
};

fs::path SaveRoot(const fs::path& nand) {
    return nand / "user" / "save" / "0000000000000000";
}

std::vector<Place> Places() {
    using namespace Common::FS;
    std::vector<Place> places{
        {QStringLiteral("FritCheeLii"), SaveRoot(GetEdenPath(EdenPath::NANDDir))}};
    constexpr std::array<std::pair<EmuPath, const char*>, 4> others{{
        {YuzuDir, "Yuzu"},
        {SuyuDir, "Suyu"},
        {SudachiDir, "Sudachi"},
        {CitronDir, "Citron"},
    }};
    for (const auto& [path, name] : others) {
        if (fs::is_directory(GetLegacyPath(path) / NAND_DIR)) {
            places.push_back({QString::fromLatin1(name), SaveRoot(GetLegacyPath(path) / NAND_DIR)});
        }
    }
    return places;
}

QString Text(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

/// The user profile folders (32 hex digits) in a save root.
std::vector<fs::path> Profiles(const fs::path& root) {
    std::vector<fs::path> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec) && name.size() == 32 &&
            std::ranges::all_of(name, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
            result.push_back(entry.path());
        }
    }
    return result;
}

/// Where the saves under `root` are backed up before a change: <nand>/user/save-backups/<time>.
/// Picked before asking, so the question can say where (the owner, 2026-11-11: always back up,
/// and say where).
fs::path BackupPath(const fs::path& root) {
    const auto stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    return root.parent_path().parent_path() / "save-backups" / stamp.toStdString();
}

/// Copies `root` to `target` (always made, even when there were no saves yet).
fs::path Backup(const fs::path& root, const fs::path& target) {
    fs::create_directories(target);
    if (fs::is_directory(root)) {
        fs::copy(root, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
    return target;
}

/// The user profiles an install really has, as save folder names, from its
/// <nand>/system/save/8000000000000010/su/avators/profiles.dat (8 users of 0xC8 bytes after a
/// 0x10 header; a user's id is 16 bytes, written as its high then low 64-bit half in hex).
std::vector<std::string> SystemProfiles(const fs::path& save_root) {
    // save_root: <nand>/user/save/0000000000000000
    const fs::path nand = save_root.parent_path().parent_path().parent_path();
    std::ifstream file(nand / "system" / "save" / "8000000000000010" / "su" / "avators" /
                           "profiles.dat",
                       std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(file)), {});
    std::vector<std::string> ids;
    for (std::size_t offset = 0x10; offset + 0xC8 <= data.size(); offset += 0xC8) {
        u64 low{};
        u64 high{};
        std::memcpy(&low, data.data() + offset, sizeof(low));
        std::memcpy(&high, data.data() + offset + 8, sizeof(high));
        if (low != 0 || high != 0) {
            ids.push_back(fmt::format("{:016X}{:016X}", high, low));
        }
    }
    return ids;
}

/// Each game's saves from `from` into `to`, replacing that game's saves there. Each install
/// makes its own user profile, so the saves go into the profile the target really has (its
/// profiles.dat): v5.8.3 kept the source's folder name when the target had no saves yet, and the
/// game never saw them ("Orphaned Profiles Detected", 2026-11-11). Returns how many games.
int CopySaves(const fs::path& from, const fs::path& to) {
    auto sources = Profiles(from);
    const auto users = SystemProfiles(to);
    // With several source profiles, the one the source install's first user owns goes last, so
    // its saves win where both have the same game.
    if (const auto source_users = SystemProfiles(from); !source_users.empty()) {
        std::ranges::stable_partition(sources, [&](const fs::path& p) {
            return p.filename().string() != source_users.front();
        });
    }
    const auto target_profile = [&](const fs::path& source) {
        if (users.empty()) {
            return to / source.filename(); // no profile file: keep the folder as it was
        }
        // The target user with saves already, else its first user.
        for (const auto& id : users) {
            if (fs::is_directory(to / id)) {
                return to / id;
            }
        }
        return to / users.front();
    };
    int games = 0;
    for (const auto& source : sources) {
        const fs::path profile = target_profile(source);
        fs::create_directories(profile);
        for (const auto& game : fs::directory_iterator(source)) {
            if (!game.is_directory()) {
                continue;
            }
            const auto target = profile / game.path().filename();
            fs::remove_all(target);
            fs::copy(game.path(), target, fs::copy_options::recursive);
            ++games;
        }
    }
    return games;
}

fs::path ActiveProfileDir(const fs::path& save_root) {
    const auto users = SystemProfiles(save_root);
    if (!users.empty()) {
        for (const auto& id : users) {
            if (fs::is_directory(save_root / id)) {
                return save_root / id;
            }
        }
        return save_root / users.front();
    }
    const auto existing = Profiles(save_root);
    if (!existing.empty()) {
        return existing.front();
    }
    return save_root / "00000000000000000000000000000001";
}

bool HasRawSaveEntries(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (name == "slot_00" || name == "slot_01" || name == "slot_02" || name == "system" ||
            name == "option" || name == "caption.sav" || name.ends_with(".sav")) {
            return true;
        }
    }
    return false;
}

/// Tests only: EDEN_NX2_SAVES_TEST_FILE answers the export/import file dialog (the sandbox tests
/// can't drive Windows' own dialog reliably).
QString TestFile() {
    return qEnvironmentVariable("EDEN_NX2_SAVES_TEST_FILE");
}

void Done(QWidget* parent, int games, const fs::path& backup) {
    QString text = games == 1 ? QObject::tr("Done: the saves of 1 game were copied.")
                              : QObject::tr("Done: the saves of %1 games were copied.").arg(games);
    text += QObject::tr("\n\nThe saves that were there before are backed up in:\n%1")
                .arg(Text(backup));
    QMessageBox box(QMessageBox::Information, QObject::tr("Saves"), text, QMessageBox::Ok, parent);
    QAbstractButton* open = box.addButton(QObject::tr("Open backup"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Text(backup)));
    }
}

} // namespace

void ShowSavesWindow(QWidget* parent, const std::function<bool()>& emulation_running) {
    const auto places = Places();
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Saves"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    QFont big = dialog.font();
    // A font sized in pixels (some Linux desktops) has no point size: scale its pixels.
    if (big.pointSizeF() > 0) {
        big.setPointSizeF(big.pointSizeF() * 1.15);
    } else if (big.pixelSize() > 0) {
        big.setPixelSize(static_cast<int>(big.pixelSize() * 1.15 + 0.5));
    }
    dialog.setFont(big);
    auto* layout = new QGridLayout(&dialog);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(14);

    const auto blocked = [&] {
        if (emulation_running && emulation_running()) {
            QMessageBox::information(&dialog, QObject::tr("Saves"),
                                     QObject::tr("Close the game first."));
            return true;
        }
        return false;
    };
    const auto failed = [&](const std::exception& error) {
        QMessageBox::warning(&dialog, QObject::tr("Saves"),
                             QObject::tr("That didn't work:\n%1")
                                 .arg(QString::fromUtf8(error.what())));
    };
    const fs::path fritchii = places.front().root;
    // Every change backs up first and says where, before and after (the owner, 2026-11-11).
    const auto confirm = [&](const QString& question, const fs::path& backup_path) {
        return QMessageBox::question(
                   &dialog, QObject::tr("Saves"),
                   question + QObject::tr("\n\nThe saves there now are backed up first, in:\n%1")
                                  .arg(Text(backup_path))) == QMessageBox::Yes;
    };

    // Universal Export: choose .zip backup, detected emulator, or any folder (NX2Navi / Hookshot / Longshot)
    auto* export_button = new QPushButton(QObject::tr("Export..."), &dialog);
    layout->addWidget(
        new QLabel(QObject::tr("Export saves (Emulator, NX2Navi, Hookshot/Longshot or .zip)"),
                   &dialog),
        0, 0);
    layout->addWidget(export_button, 0, 1);
    QObject::connect(export_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        if (Profiles(fritchii).empty()) {
            QMessageBox::information(&dialog, QObject::tr("Saves"),
                                     QObject::tr("FritCheeLii has no saves yet."));
            return;
        }
        QDialog pick(&dialog);
        pick.setWindowTitle(QObject::tr("Export Saves"));
        auto* v = new QVBoxLayout(&pick);
        v->addWidget(new QLabel(QObject::tr("Choose where to export your FritCheeLii saves:"), &pick));
        auto* combo = new QComboBox(&pick);
        combo->addItem(QObject::tr("Choose folder (NX2Navi / Hookshot / Longshot / Any Emulator)..."),
                       QStringLiteral("folder"));
        combo->addItem(QObject::tr("Save as .zip backup file..."), QStringLiteral("zip"));
        for (size_t i = 1; i < places.size(); ++i) {
            combo->addItem(QObject::tr("Detected: %1").arg(places[i].name),
                           static_cast<int>(i));
        }
        v->addWidget(combo);
        auto* btns = new QHBoxLayout;
        auto* ok_btn = new QPushButton(QObject::tr("Continue"), &pick);
        auto* cancel_btn = new QPushButton(QObject::tr("Cancel"), &pick);
        btns->addStretch(1);
        btns->addWidget(ok_btn);
        btns->addWidget(cancel_btn);
        v->addLayout(btns);
        QObject::connect(ok_btn, &QPushButton::clicked, &pick, &QDialog::accept);
        QObject::connect(cancel_btn, &QPushButton::clicked, &pick, &QDialog::reject);
        if (pick.exec() != QDialog::Accepted) {
            return;
        }
        const QVariant choice = combo->currentData();
        try {
            if (choice.typeId() == QMetaType::QString && choice.toString() == QStringLiteral("zip")) {
                const QString name =
                    QObject::tr("FritCheeLii saves %1.zip")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")));
                const QString file = !TestFile().isEmpty()
                                         ? TestFile()
                                         : QFileDialog::getSaveFileName(
                                               &dialog, QObject::tr("Export saves"),
                                               QStandardPaths::writableLocation(
                                                   QStandardPaths::DocumentsLocation) +
                                                   QLatin1Char('/') + name,
                                               QObject::tr("Zip files (*.zip)"));
                if (file.isEmpty()) {
                    return;
                }
                if (!JlCompress::compressDir(file, Text(fritchii), true)) {
                    QMessageBox::warning(&dialog, QObject::tr("Saves"),
                                         QObject::tr("Could not write %1").arg(file));
                    return;
                }
                QMessageBox::information(&dialog, QObject::tr("Saves"),
                                         QObject::tr("Saves exported to:\n%1").arg(file));
            } else if (choice.typeId() == QMetaType::QString &&
                       choice.toString() == QStringLiteral("folder")) {
                const QString dir = QFileDialog::getExistingDirectory(
                    &dialog,
                    QObject::tr("Select destination folder (Emulator, NX2Navi, Hookshot/Longshot)"));
                if (dir.isEmpty()) {
                    return;
                }
                fs::path dst = fs::u8path(dir.toStdString());
                // Check if destination is an emulator root/nand
                fs::path emu_dst;
                for (const fs::path& cand :
                     {dst / "user" / "nand" / "user" / "save" / "0000000000000000",
                      dst / "nand" / "user" / "save" / "0000000000000000",
                      dst / "user" / "save" / "0000000000000000"}) {
                    if (fs::is_directory(cand.parent_path())) {
                        emu_dst = cand;
                        break;
                    }
                }
                if (!emu_dst.empty()) {
                    const auto backup_path = BackupPath(emu_dst);
                    if (!confirm(QObject::tr("Export FritCheeLii saves to:\n%1?").arg(Text(emu_dst)),
                                 backup_path)) {
                        return;
                    }
                    const auto backup = Backup(emu_dst, backup_path);
                    const int games = CopySaves(fritchii, emu_dst);
                    Done(&dialog, games, backup);
                    return;
                }
                const fs::path src = ActiveProfileDir(fritchii) / "0400486024DEC000";
                if (!fs::is_directory(src)) {
                    QMessageBox::information(&dialog, QObject::tr("Saves"),
                                             QObject::tr("FritCheeLii has no Ocarina of Time save yet."));
                    return;
                }
                if (fs::is_directory(dst / "profile") && !fs::exists(dst / "slot_00")) {
                    dst = dst / "profile" / "save";
                }
                fs::create_directories(dst);
                fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                QMessageBox::information(
                    &dialog, QObject::tr("Saves"),
                    QObject::tr("Ocarina of Time save exported to:\n%1").arg(Text(dst)));
            } else {
                const Place& target = places[static_cast<size_t>(choice.toInt())];
                const auto backup_path = BackupPath(target.root);
                if (!confirm(QObject::tr("Export FritCheeLii saves to %1? Close %1 first.")
                                 .arg(target.name),
                             backup_path)) {
                    return;
                }
                const auto backup = Backup(target.root, backup_path);
                const int games = CopySaves(fritchii, target.root);
                Done(&dialog, games, backup);
            }
        } catch (const std::exception& error) {
            failed(error);
        }
    });

    // Universal Import: choose detected emulator, any folder (NX2Navi/Hookshot/Longshot), or .zip
    auto* import_button = new QPushButton(QObject::tr("Import..."), &dialog);
    layout->addWidget(
        new QLabel(QObject::tr("Import saves (Emulator, NX2Navi, Hookshot/Longshot or .zip)"),
                   &dialog),
        1, 0);
    layout->addWidget(import_button, 1, 1);
    QObject::connect(import_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        QDialog pick(&dialog);
        pick.setWindowTitle(QObject::tr("Import Saves"));
        auto* v = new QVBoxLayout(&pick);
        v->addWidget(new QLabel(QObject::tr("Choose where to import saves from:"), &pick));
        auto* combo = new QComboBox(&pick);
        combo->addItem(QObject::tr("Choose folder (NX2Navi / Hookshot / Longshot / Any Emulator)..."),
                       QStringLiteral("folder"));
        combo->addItem(QObject::tr("Choose .zip backup file..."), QStringLiteral("zip"));
        for (size_t i = 1; i < places.size(); ++i) {
            combo->addItem(QObject::tr("Detected: %1").arg(places[i].name),
                           static_cast<int>(i));
        }
        v->addWidget(combo);
        auto* btns = new QHBoxLayout;
        auto* ok_btn = new QPushButton(QObject::tr("Continue"), &pick);
        auto* cancel_btn = new QPushButton(QObject::tr("Cancel"), &pick);
        btns->addStretch(1);
        btns->addWidget(ok_btn);
        btns->addWidget(cancel_btn);
        v->addLayout(btns);
        QObject::connect(ok_btn, &QPushButton::clicked, &pick, &QDialog::accept);
        QObject::connect(cancel_btn, &QPushButton::clicked, &pick, &QDialog::reject);
        if (pick.exec() != QDialog::Accepted) {
            return;
        }
        const QVariant choice = combo->currentData();
        const fs::path temp = fritchii.parent_path().parent_path() / "save-import";
        try {
            if (choice.typeId() == QMetaType::QString && choice.toString() == QStringLiteral("zip")) {
                const QString file = !TestFile().isEmpty()
                                         ? TestFile()
                                         : QFileDialog::getOpenFileName(
                                               &dialog, QObject::tr("Import saves"),
                                               QStandardPaths::writableLocation(
                                                   QStandardPaths::DocumentsLocation),
                                               QObject::tr("Zip files (*.zip)"));
                if (file.isEmpty()) {
                    return;
                }
                fs::remove_all(temp);
                fs::create_directories(temp);
                if (JlCompress::extractDir(file, Text(temp)).isEmpty()) {
                    throw std::runtime_error("the zip file is empty or can't be read");
                }
                fs::path root = temp;
                if (Profiles(root).empty() && !Profiles(temp / "0000000000000000").empty()) {
                    root = temp / "0000000000000000";
                }
                if (Profiles(root).empty()) {
                    throw std::runtime_error("no saves in that zip file");
                }
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import these saves into FritCheeLii?"), backup_path)) {
                    fs::remove_all(temp);
                    return;
                }
                const auto backup = Backup(fritchii, backup_path);
                const int games = CopySaves(root, fritchii);
                fs::remove_all(temp);
                Done(&dialog, games, backup);
            } else if (choice.typeId() == QMetaType::QString &&
                       choice.toString() == QStringLiteral("folder")) {
                const QString dir = QFileDialog::getExistingDirectory(
                    &dialog,
                    QObject::tr("Select save folder (NX2Navi, Hookshot, Longshot or Emulator)"));
                if (dir.isEmpty()) {
                    return;
                }
                const fs::path picked = fs::u8path(dir.toStdString());
                fs::path emu_root;
                for (const fs::path& cand :
                     {picked, picked / "user" / "nand" / "user" / "save" / "0000000000000000",
                      picked / "nand" / "user" / "save" / "0000000000000000",
                      picked / "user" / "save" / "0000000000000000",
                      picked / "0000000000000000"}) {
                    if (!Profiles(cand).empty()) {
                        emu_root = cand;
                        break;
                    }
                }
                if (!emu_root.empty()) {
                    const auto backup_path = BackupPath(fritchii);
                    if (!confirm(QObject::tr("Import saves from this folder into FritCheeLii?"),
                                 backup_path)) {
                        return;
                    }
                    const auto backup = Backup(fritchii, backup_path);
                    const int games = CopySaves(emu_root, fritchii);
                    Done(&dialog, games, backup);
                    return;
                }
                fs::path raw_src;
                for (const fs::path& cand : {picked, picked / "profile" / "save", picked / "save",
                                             picked / "savedata", picked / "saves"}) {
                    if (HasRawSaveEntries(cand)) {
                        raw_src = cand;
                        break;
                    }
                }
                if (raw_src.empty()) {
                    throw std::runtime_error(
                        "No compatible saves found in that folder.\n"
                        "Select an emulator folder, NX2Navi folder (or profile\\save), or "
                        "Hookshot/Longshot save folder.");
                }
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import Ocarina of Time save from:\n%1\ninto FritCheeLii?")
                                 .arg(Text(raw_src)),
                             backup_path)) {
                    return;
                }
                const auto backup = Backup(fritchii, backup_path);
                const fs::path target = ActiveProfileDir(fritchii) / "0400486024DEC000";
                fs::create_directories(target);
                fs::copy(raw_src, target,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                Done(&dialog, 1, backup);
            } else {
                const Place& source = places[static_cast<size_t>(choice.toInt())];
                if (Profiles(source.root).empty()) {
                    QMessageBox::information(&dialog, QObject::tr("Saves"),
                                             QObject::tr("%1 has no saves.").arg(source.name));
                    return;
                }
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import saves from %1 into FritCheeLii?").arg(source.name),
                             backup_path)) {
                    return;
                }
                const auto backup = Backup(fritchii, backup_path);
                const int games = CopySaves(source.root, fritchii);
                Done(&dialog, games, backup);
            }
        } catch (const std::exception& error) {
            std::error_code ec;
            fs::remove_all(temp, ec);
            failed(error);
        }
    });

    const auto orphans = [&] {
        const auto users = SystemProfiles(fritchii);
        std::vector<fs::path> result;
        if (users.empty()) {
            return result;
        }
        for (const auto& profile : Profiles(fritchii)) {
            if (std::ranges::find(users, profile.filename().string()) == users.end()) {
                result.push_back(profile);
            }
        }
        return result;
    };
    int row = 2;

    if (!orphans().empty()) {
        auto* fix_button = new QPushButton(QObject::tr("Fix"), &dialog);
        layout->addWidget(new QLabel(QObject::tr("Saves that don't show up in the game"), &dialog),
                          row, 0);
        layout->addWidget(fix_button, row, 1);
        ++row;
        QObject::connect(fix_button, &QPushButton::clicked, &dialog, [&, fix_button] {
            if (blocked()) {
                return;
            }
            const auto backup_path = BackupPath(fritchii);
            if (!confirm(QObject::tr("Move the saves that don't show up into your FritCheeLii "
                                     "profile?"),
                         backup_path)) {
                return;
            }
            try {
                const auto backup = Backup(fritchii, backup_path);
                const auto users = SystemProfiles(fritchii);
                fs::path user = fritchii / users.front();
                for (const auto& id : users) {
                    if (fs::is_directory(fritchii / id)) {
                        user = fritchii / id;
                        break;
                    }
                }
                fs::create_directories(user);
                int games = 0;
                for (const auto& orphan : orphans()) {
                    for (const auto& game : fs::directory_iterator(orphan)) {
                        if (!game.is_directory()) {
                            continue;
                        }
                        const auto target = user / game.path().filename();
                        fs::remove_all(target);
                        fs::copy(game.path(), target, fs::copy_options::recursive);
                        ++games;
                    }
                    fs::remove_all(orphan); // copied above, and in the backup
                }
                fix_button->setEnabled(false);
                Done(&dialog, games, backup);
            } catch (const std::exception& error) {
                failed(error);
            }
        });
    }

    auto* close = new QPushButton(QObject::tr("Close"), &dialog);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(close, row, 1);
    dialog.exec();
}

} // namespace FritchiiUi
