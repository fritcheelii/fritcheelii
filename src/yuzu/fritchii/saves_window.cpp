// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/saves_window.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <JlCompress.h>
#include <fmt/format.h>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include "common/common_types.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"

namespace FritchiiUi {
namespace {
namespace fs = std::filesystem;

constexpr const char* OOT_TITLE_HEX = "0400486024DEC000";
constexpr const char* TRIFORCE_UID = "5a3c9e1f0b7d24680000000000000001";

fs::path SaveRoot(const fs::path& nand) {
    return nand / "user" / "save" / "0000000000000000";
}

QString Text(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

std::string LowerNormPath(const fs::path& p) {
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(p, ec);
    if (ec) {
        norm = p.lexically_normal();
    }
    std::string s = Common::FS::PathToUTF8String(norm);
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool DirHasFiles(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    auto it = fs::directory_iterator(dir, ec);
    return !ec && it != fs::directory_iterator{};
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
/// <nand>/system/save/8000000000000010/su/avators/profiles.dat.
std::vector<std::string> SystemProfiles(const fs::path& save_root) {
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

int CopySaves(const fs::path& from, const fs::path& to) {
    auto sources = Profiles(from);
    const auto users = SystemProfiles(to);
    if (const auto source_users = SystemProfiles(from); !source_users.empty()) {
        std::ranges::stable_partition(sources, [&](const fs::path& p) {
            return p.filename().string() != source_users.front();
        });
    }
    const auto target_profile = [&](const fs::path& source) {
        if (users.empty()) {
            return to / source.filename();
        }
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

fs::path OotSlotsRoot(const fs::path& fritchii_save_root) {
    return fritchii_save_root.parent_path().parent_path() / "save-slots" / OOT_TITLE_HEX;
}

fs::path OotActiveNandDir(const fs::path& fritchii_save_root) {
    return ActiveProfileDir(fritchii_save_root) / OOT_TITLE_HEX;
}

int ReadActiveSlot(const fs::path& slots_root) {
    std::ifstream in(slots_root / "active_slot.txt");
    int slot = 1;
    if (in >> slot) {
        return std::clamp(slot, 1, 8);
    }
    return 1;
}

void WriteActiveSlot(const fs::path& slots_root, int slot) {
    std::error_code ec;
    fs::create_directories(slots_root, ec);
    std::ofstream out(slots_root / "active_slot.txt", std::ios::trunc);
    if (out) {
        out << std::clamp(slot, 1, 8) << '\n';
    }
}

struct SlotMeta {
    QString name;
    QString updated;
};

SlotMeta ReadSlotMeta(const fs::path& slot_dir, int index) {
    SlotMeta meta;
    meta.name = QObject::tr("Slot %1").arg(index);
    std::ifstream in(slot_dir / "meta.txt");
    std::string line1;
    std::string line2;
    if (std::getline(in, line1) && !line1.empty()) {
        const QString parsed =
            QString::fromUtf8(line1.c_str(), static_cast<int>(line1.size())).trimmed();
        if (!parsed.isEmpty()) {
            meta.name = parsed;
        }
    }
    if (std::getline(in, line2)) {
        meta.updated = QString::fromUtf8(line2.c_str(), static_cast<int>(line2.size())).trimmed();
    }
    return meta;
}

void WriteSlotMeta(const fs::path& slot_dir, const QString& name, const QString& updated) {
    std::error_code ec;
    fs::create_directories(slot_dir, ec);
    std::ofstream out(slot_dir / "meta.txt", std::ios::trunc);
    if (out) {
        out << name.toUtf8().toStdString() << '\n' << updated.toUtf8().toStdString() << '\n';
    }
}

void SyncDirCopy(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::remove_all(dst, ec);
    if (DirHasFiles(src)) {
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
}

void EnsureSlot1Seeded(const fs::path& fritchii, const fs::path& slots_root) {
    const fs::path slot1_dir = slots_root / "slot_1";
    const fs::path nand_oot = OotActiveNandDir(fritchii);
    if (!DirHasFiles(slot1_dir / "data") && DirHasFiles(nand_oot)) {
        SyncDirCopy(nand_oot, slot1_dir / "data");
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        WriteSlotMeta(slot1_dir, QObject::tr("Main Campaign"), stamp);
        if (!fs::exists(slots_root / "active_slot.txt")) {
            WriteActiveSlot(slots_root, 1);
        }
    }
}

// ---------------- Triforce-style Caption Thumbnail & OoT Save Inspection ----------------

bool IsOotSaveSet(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    return fs::is_regular_file(dir / "option.sav", ec) ||
           fs::is_regular_file(dir / "account.sav", ec) ||
           fs::is_regular_file(dir / "slot_00" / "progress.sav", ec);
}

QPixmap ExtractCaptionPixmap(const fs::path& caption_sav) {
    std::ifstream file(caption_sav, std::ios::binary);
    if (!file) {
        return {};
    }
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), {});
    if (data.size() < 5) {
        return {};
    }
    constexpr std::array<unsigned char, 3> kStart{0xFF, 0xD8, 0xFF};
    auto it = std::search(data.begin(), data.end(), kStart.begin(), kStart.end());
    if (it == data.end()) {
        return {};
    }
    const std::size_t from = static_cast<std::size_t>(std::distance(data.begin(), it));
    std::size_t to = 0;
    bool found_end = false;
    for (std::size_t i = data.size() - 1; i > from + 2; --i) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) {
            to = i - 1;
            found_end = true;
            break;
        }
    }
    if (!found_end || to <= from) {
        return {};
    }
    QPixmap pix;
    pix.loadFromData(data.data() + from, static_cast<uint>(to - from + 2), "JPG");
    return pix;
}

QPixmap RoundedThumbnail(const QPixmap& src, int w, int h, int radius = 8) {
    if (src.isNull()) {
        return {};
    }
    QPixmap scaled = src.scaled(w, h, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = std::max(0, (scaled.width() - w) / 2);
    const int y = std::max(0, (scaled.height() - h) / 2);
    QPixmap cropped = scaled.copy(x, y, w, h);

    QPixmap out(w, h);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, w, h, radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, cropped);
    return out;
}

struct OotSaveSummary {
    bool has_data = false;
    int used_files = 0;
    QDateTime newest_time;
    fs::path newest_caption_path;
    QPixmap preview;
};

OotSaveSummary InspectOotSaveDir(const fs::path& dir, bool load_preview = true) {
    OotSaveSummary summary;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return summary;
    }

    QDateTime best_caption_time;
    QDateTime fallback_caption_time;
    fs::path fallback_caption_path;

    for (int i = 0; i < 6; ++i) {
        const fs::path slot_dir = dir / fmt::format("slot_{:02d}", i);
        const fs::path progress = slot_dir / "progress.sav";
        const fs::path caption = slot_dir / "caption.sav";
        const bool has_progress = fs::is_regular_file(progress, ec);
        const bool has_caption = fs::is_regular_file(caption, ec);

        if (has_progress) {
            ++summary.used_files;
            const QDateTime pt = QFileInfo(Text(progress)).lastModified();
            if (pt.isValid() && (!summary.newest_time.isValid() || pt > summary.newest_time)) {
                summary.newest_time = pt;
            }
        }
        if (has_caption) {
            const QDateTime ct = QFileInfo(Text(caption)).lastModified();
            if (ct.isValid() && (!summary.newest_time.isValid() || ct > summary.newest_time)) {
                summary.newest_time = ct;
            }
            if (has_progress) {
                if (!best_caption_time.isValid() || (ct.isValid() && ct > best_caption_time)) {
                    best_caption_time = ct;
                    summary.newest_caption_path = caption;
                }
            } else if (!fallback_caption_time.isValid() ||
                       (ct.isValid() && ct > fallback_caption_time)) {
                fallback_caption_time = ct;
                fallback_caption_path = caption;
            }
        }
    }

    if (summary.newest_caption_path.empty() && !fallback_caption_path.empty()) {
        summary.newest_caption_path = fallback_caption_path;
    }

    if (!summary.newest_time.isValid()) {
        for (const char* f : {"option.sav", "account.sav"}) {
            const fs::path p = dir / f;
            if (fs::is_regular_file(p, ec)) {
                const QDateTime t = QFileInfo(Text(p)).lastModified();
                if (t.isValid() && (!summary.newest_time.isValid() || t > summary.newest_time)) {
                    summary.newest_time = t;
                }
            }
        }
    }

    summary.has_data = summary.used_files > 0 || IsOotSaveSet(dir) || DirHasFiles(dir);
    if (load_preview && !summary.newest_caption_path.empty()) {
        summary.preview = ExtractCaptionPixmap(summary.newest_caption_path);
    }
    return summary;
}

QString FormatSaveDate(const QDateTime& dt) {
    return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QObject::tr("Empty");
}

// ---------------- Cross-Emulator / Triforce / NX2navi Auto-Detection ----------------

long long ParseReadySuffix(const std::string& name) {
    if (!name.starts_with("ready-") || name.size() <= 6) {
        return -1;
    }
    long long val = 0;
    for (std::size_t i = 6; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isdigit(c)) {
            return -1;
        }
        val = val * 10 + (c - '0');
        if (val > 1'000'000'000LL) {
            break;
        }
    }
    return val;
}

fs::path HighestReadyCommit(const fs::path& tx_dir) {
    std::error_code ec;
    if (!fs::is_directory(tx_dir, ec)) {
        return {};
    }
    long long best_n = -1;
    fs::path best_dir;
    for (const auto& entry : fs::directory_iterator(tx_dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const long long n = ParseReadySuffix(entry.path().filename().string());
        if (n > best_n && IsOotSaveSet(entry.path())) {
            best_n = n;
            best_dir = entry.path();
        }
    }
    return best_dir;
}

fs::path TriforceTxDir(const fs::path& save_dir) {
    const fs::path parent = save_dir.parent_path();
    if (parent.filename().string() != "save") {
        return {};
    }
    const fs::path user = parent.parent_path();
    if (user.empty()) {
        return {};
    }
    return user / "save-tx" / save_dir.filename();
}

fs::path TriforceNewestDir(const fs::path& save_dir) {
    if (const fs::path tx = TriforceTxDir(save_dir); !tx.empty()) {
        if (const fs::path ready = HighestReadyCommit(tx); !ready.empty()) {
            return ready;
        }
    }
    return save_dir;
}

fs::path NaviSaveDir(const fs::path& base) {
    std::error_code ec;
    for (const fs::path& profile : {base / "profile", base}) {
        const fs::path saves = profile / "save";
        if (!fs::is_directory(saves, ec)) {
            continue;
        }
        if (const fs::path ready = HighestReadyCommit(profile / "save-tx"); !ready.empty()) {
            return ready;
        }
        fs::path best_acc;
        QDateTime best_acc_time;
        for (const auto& entry : fs::directory_iterator(saves, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name.size() == 32 &&
                std::ranges::all_of(name,
                                    [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }) &&
                IsOotSaveSet(entry.path())) {
                const QDateTime t = InspectOotSaveDir(entry.path(), false).newest_time;
                if (best_acc.empty() || (t.isValid() && (!best_acc_time.isValid() || t > best_acc_time))) {
                    best_acc = entry.path();
                    best_acc_time = t;
                }
            }
        }
        if (!best_acc.empty()) {
            return best_acc;
        }
        if (IsOotSaveSet(saves)) {
            return saves;
        }
    }
    return {};
}

fs::path FindOotInYuzuSaveRoot(const fs::path& save_root) {
    std::error_code ec;
    if (!fs::is_directory(save_root, ec)) {
        return {};
    }
    for (const auto& profile : Profiles(save_root)) {
        const fs::path cand = profile / OOT_TITLE_HEX;
        if (IsOotSaveSet(cand)) {
            return cand;
        }
    }
    return {};
}

fs::path FindOotSaveIn(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return {};
    }
    if (IsOotSaveSet(dir)) {
        return dir;
    }
    if (const fs::path navi = NaviSaveDir(dir); !navi.empty()) {
        return navi;
    }
    const fs::path triforce_direct = dir / "user" / "save" / TRIFORCE_UID;
    if (const fs::path tri = TriforceNewestDir(triforce_direct); IsOotSaveSet(tri)) {
        return tri;
    }
    for (const fs::path& root : {dir / "user" / "nand" / "user" / "save" / "0000000000000000",
                                 dir / "nand" / "user" / "save" / "0000000000000000",
                                 dir / "user" / "save" / "0000000000000000",
                                 dir / "0000000000000000", dir}) {
        if (const fs::path found = FindOotInYuzuSaveRoot(root); !found.empty()) {
            return found;
        }
    }

    std::queue<std::pair<fs::path, int>> q;
    q.push({dir, 0});
    int visited = 0;
    while (!q.empty() && visited++ < 2000) {
        auto [cur, depth] = std::move(q.front());
        q.pop();
        for (const auto& entry : fs::directory_iterator(cur, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            if (IsOotSaveSet(entry.path())) {
                return entry.path();
            }
            if (depth < 5) {
                q.push({entry.path(), depth + 1});
            }
        }
    }
    return {};
}

struct DetectedSaveSource {
    QString name;
    fs::path oot_dir;
    fs::path emu_save_root;
    bool is_triforce = false;
    OotSaveSummary summary;
};

std::vector<DetectedSaveSource> DetectAllOotSaves(const fs::path& fritchii_save_root) {
    std::vector<DetectedSaveSource> out;
    std::set<std::string> seen;

    const std::string self_root = LowerNormPath(fritchii_save_root);
    const std::string self_oot = LowerNormPath(OotActiveNandDir(fritchii_save_root));
    seen.insert(self_root);
    seen.insert(self_oot);

    const auto add_source = [&](const QString& name, const fs::path& oot_dir,
                                const fs::path& emu_root = {}, bool is_triforce = false) {
        std::error_code ec;
        const fs::path key_path = !oot_dir.empty() ? oot_dir : emu_root;
        if (key_path.empty()) {
            return;
        }
        const std::string key = LowerNormPath(key_path);
        if (!seen.insert(key).second) {
            return;
        }
        if (!emu_root.empty() && !seen.insert(LowerNormPath(emu_root)).second) {
            return;
        }
        OotSaveSummary sum;
        if (!oot_dir.empty() && fs::is_directory(oot_dir, ec)) {
            sum = InspectOotSaveDir(oot_dir, true);
        }
        if (!sum.has_data && emu_root.empty()) {
            return;
        }
        if (!sum.has_data && Profiles(emu_root).empty()) {
            return;
        }
        out.push_back({name, oot_dir, emu_root, is_triforce, std::move(sum)});
    };

    struct EmuEntry {
        const char* dir_name;
        const char* label;
    };
    constexpr std::array<EmuEntry, 11> kAppDataEmus{{
        {"eden", "Eden"},
        {"lichii", "Lichii"},
        {"fritchii", "Fritchii"},
        {"cheezyemu", "CheezyEmu"},
        {"EvilOOT", "EvilOOT"},
        {"yuzu", "Yuzu"},
        {"suyu", "Suyu"},
        {"sudachi", "Sudachi"},
        {"citron", "Citron"},
        {"torzu", "Torzu"},
        {"eglantine", "Eglantine"},
    }};

    const QString appdata_q = qEnvironmentVariable("APPDATA");
    if (!appdata_q.isEmpty()) {
        const fs::path appdata = fs::u8path(appdata_q.toStdString());
        std::error_code ec;
        for (const auto& emu : kAppDataEmus) {
            const fs::path nand = appdata / emu.dir_name / "nand";
            if (!fs::is_directory(nand, ec)) {
                continue;
            }
            const fs::path root = SaveRoot(nand);
            if (!fs::is_directory(root, ec)) {
                continue;
            }
            const fs::path oot = FindOotInYuzuSaveRoot(root);
            add_source(QString::fromLatin1(emu.label), oot, root, false);
        }

        const fs::path ryu_save = appdata / "Ryujinx" / "bis" / "user" / "save";
        if (fs::is_directory(ryu_save, ec)) {
            for (const auto& entry : fs::directory_iterator(ryu_save, ec)) {
                if (!entry.is_directory(ec)) {
                    continue;
                }
                const fs::path cand = entry.path() / "0";
                if (IsOotSaveSet(cand)) {
                    add_source(QStringLiteral("Ryujinx"), cand);
                }
            }
        }

        for (const auto& [folder, label] :
             {std::pair{"triforce", "Triforce"}, std::pair{"hookshot", "Hookshot"}}) {
            const fs::path base = appdata / folder / "user" / "save" / TRIFORCE_UID;
            const fs::path newest = TriforceNewestDir(base);
            if (IsOotSaveSet(newest)) {
                add_source(QString::fromLatin1(label), newest, {}, true);
            }
        }
    }

    // Scan sibling folders of the running emulator directory for portable installs.
    const fs::path app_dir = fs::u8path(QCoreApplication::applicationDirPath().toStdString());
    const fs::path parent_dir = app_dir.parent_path();
    std::error_code ec;
    if (!parent_dir.empty() && fs::is_directory(parent_dir, ec)) {
        int scanned = 0;
        for (const auto& entry : fs::directory_iterator(parent_dir, ec)) {
            if (scanned++ >= 60) {
                break;
            }
            if (!entry.is_directory(ec)) {
                continue;
            }
            const fs::path dir = entry.path();
            if (LowerNormPath(dir) == LowerNormPath(app_dir)) {
                continue;
            }
            const QString folder_label = Text(dir.filename());
            const fs::path portable_yuzu =
                dir / "user" / "nand" / "user" / "save" / "0000000000000000";
            if (fs::is_directory(portable_yuzu, ec)) {
                const fs::path oot = FindOotInYuzuSaveRoot(portable_yuzu);
                add_source(QObject::tr("Portable (%1)").arg(folder_label), oot, portable_yuzu,
                           false);
            }
            const fs::path portable_tri = dir / "user" / "save" / TRIFORCE_UID;
            if (const fs::path newest = TriforceNewestDir(portable_tri); IsOotSaveSet(newest)) {
                add_source(QObject::tr("Triforce (%1)").arg(folder_label), newest, {}, true);
            }
            if (const fs::path navi = NaviSaveDir(dir); !navi.empty()) {
                add_source(QObject::tr("NX2Saria / NX2navi (%1)").arg(folder_label), navi);
            }
        }
    }

    std::ranges::stable_sort(out, [](const DetectedSaveSource& a, const DetectedSaveSource& b) {
        if (a.summary.has_data != b.summary.has_data) {
            return a.summary.has_data > b.summary.has_data;
        }
        if (a.summary.newest_time != b.summary.newest_time) {
            return a.summary.newest_time > b.summary.newest_time;
        }
        return a.name.toLower() < b.name.toLower();
    });
    return out;
}

/// Tests only: EDEN_NX2_SAVES_TEST_FILE answers the export/import file dialog.
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
    const fs::path fritchii = SaveRoot(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));

    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Saves"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setMinimumWidth(840);
    QFont big = dialog.font();
    if (big.pointSizeF() > 0) {
        big.setPointSizeF(big.pointSizeF() * 1.12);
    } else if (big.pixelSize() > 0) {
        big.setPixelSize(static_cast<int>(big.pixelSize() * 1.12 + 0.5));
    }
    dialog.setFont(big);
    auto* layout = new QGridLayout(&dialog);
    layout->setHorizontalSpacing(14);
    layout->setVerticalSpacing(12);

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
    const auto confirm = [&](const QString& question, const fs::path& backup_path) {
        return QMessageBox::question(
                   &dialog, QObject::tr("Saves"),
                   question + QObject::tr("\n\nThe saves there now are backed up first, in:\n%1")
                                  .arg(Text(backup_path))) == QMessageBox::Yes;
    };

    const fs::path slots_root = OotSlotsRoot(fritchii);
    {
        const fs::path slot1_dir = slots_root / "slot_1";
        const fs::path nand_oot = OotActiveNandDir(fritchii);
        if (!DirHasFiles(slot1_dir / "data") && DirHasFiles(nand_oot)) {
            SyncDirCopy(nand_oot, slot1_dir / "data");
            const QString stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            WriteSlotMeta(slot1_dir, QObject::tr("Main Campaign"), stamp);
            if (!fs::exists(slots_root / "active_slot.txt")) {
                WriteActiveSlot(slots_root, 1);
            }
        }
    }

    int row = 0;

    // Zelda OoT Multi-Save (8 Slots) + Triforce-style Location Preview
    auto* oot_header = new QLabel(QObject::tr("<b>Zelda OoT — Multi-Save (8 Slots)</b>"), &dialog);
    layout->addWidget(oot_header, row++, 0, 1, 2);

    auto* oot_note = new QLabel(
        QObject::tr("Switch between up to 8 separate campaigns. Select a slot to preview the "
                    "exact in-game location where you last saved."),
        &dialog);
    oot_note->setWordWrap(true);
    oot_note->setStyleSheet(QStringLiteral("color: #9a9aa8; font-size: 11px;"));
    layout->addWidget(oot_note, row++, 0, 1, 2);

    auto* split = new QHBoxLayout;
    split->setSpacing(14);

    auto* slot_list = new QListWidget(&dialog);
    slot_list->setFixedHeight(272);
    split->addWidget(slot_list, 2);

    auto* preview_card = new QFrame(&dialog);
    preview_card->setStyleSheet(QStringLiteral(
        "QFrame { background-color: #16161c; border: 1px solid #2c2c38; border-radius: 10px; }"));
    auto* preview_layout = new QVBoxLayout(preview_card);
    preview_layout->setContentsMargins(12, 10, 12, 10);
    preview_layout->setSpacing(6);

    auto* preview_title = new QLabel(preview_card);
    preview_title->setWordWrap(true);
    preview_title->setStyleSheet(
        QStringLiteral("border: none; font-weight: bold; font-size: 13px; color: #ececf4;"));
    preview_layout->addWidget(preview_title);

    auto* preview_meta = new QLabel(preview_card);
    preview_meta->setWordWrap(true);
    preview_meta->setStyleSheet(
        QStringLiteral("border: none; color: #9a9aa8; font-size: 11px;"));
    preview_layout->addWidget(preview_meta);

    struct MiniCard {
        QFrame* frame = nullptr;
        QLabel* img = nullptr;
        QLabel* cap = nullptr;
    };
    std::array<MiniCard, 6> mini_cards{};
    auto* thumb_grid = new QGridLayout;
    thumb_grid->setHorizontalSpacing(8);
    thumb_grid->setVerticalSpacing(8);
    for (int idx = 0; idx < 6; ++idx) {
        auto* frame = new QFrame(preview_card);
        frame->setStyleSheet(QStringLiteral(
            "QFrame { background-color: #0e0e13; border: 1px solid #252530; border-radius: 6px; }"));
        auto* fv = new QVBoxLayout(frame);
        fv->setContentsMargins(4, 4, 4, 4);
        fv->setSpacing(3);
        auto* img = new QLabel(frame);
        img->setFixedSize(136, 76);
        img->setAlignment(Qt::AlignCenter);
        img->setStyleSheet(QStringLiteral(
            "border: none; background-color: #0a0a0e; color: #686876; font-size: 10px;"));
        auto* cap = new QLabel(frame);
        cap->setAlignment(Qt::AlignCenter);
        cap->setStyleSheet(
            QStringLiteral("border: none; color: #b4b4c2; font-size: 10px;"));
        fv->addWidget(img, 0, Qt::AlignHCenter);
        fv->addWidget(cap);
        thumb_grid->addWidget(frame, idx / 3, idx % 3);
        mini_cards[static_cast<size_t>(idx)] = {frame, img, cap};
    }
    preview_layout->addLayout(thumb_grid);
    preview_layout->addStretch(1);

    split->addWidget(preview_card, 3);
    layout->addLayout(split, row++, 0, 1, 2);

    const auto slot_inspect_dir = [&](int slot_idx, int active) {
        const fs::path sdir = slots_root / fmt::format("slot_{}", slot_idx);
        const fs::path data_dir = sdir / "data";
        if (slot_idx == active) {
            const fs::path nand_oot = OotActiveNandDir(fritchii);
            if (DirHasFiles(nand_oot)) {
                return nand_oot;
            }
        }
        return data_dir;
    };

    const auto update_preview = [&] {
        const int sel = slot_list->currentRow() + 1;
        const auto reset_grid = [&](const QString& msg) {
            for (int idx = 0; idx < 6; ++idx) {
                auto& mc = mini_cards[static_cast<size_t>(idx)];
                mc.frame->setStyleSheet(QStringLiteral(
                    "QFrame { background-color: #0e0e13; border: 1px solid #252530; border-radius: 6px; }"));
                mc.img->setPixmap(QPixmap{});
                mc.img->setText(msg);
                mc.cap->setText(idx == 0 ? QObject::tr("File 1 (Manual)")
                                         : QObject::tr("File %1 (Auto)").arg(idx + 1));
            }
        };
        if (sel < 1 || sel > 8) {
            preview_title->setText(QString{});
            preview_meta->setText(QString{});
            reset_grid(QObject::tr("Empty"));
            return;
        }
        const int active = ReadActiveSlot(slots_root);
        const fs::path sdir = slots_root / fmt::format("slot_{}", sel);
        const SlotMeta meta = ReadSlotMeta(sdir, sel);
        const fs::path inspect_dir = slot_inspect_dir(sel, active);
        const OotSaveSummary sum = InspectOotSaveDir(inspect_dir, false);

        const QString badge = (sel == active) ? QObject::tr(" [ACTIVE]") : QString{};
        preview_title->setText(QObject::tr("Slot %1 — %2%3").arg(sel).arg(meta.name, badge));

        if (!sum.has_data) {
            preview_meta->setText(QObject::tr("No campaign data in this slot yet."));
            reset_grid(QObject::tr("Empty"));
            return;
        }

        const QString date_str =
            sum.newest_time.isValid() ? FormatSaveDate(sum.newest_time) : meta.updated;
        preview_meta->setText(
            QObject::tr("%1 / 6 in-game files (1 manual + 5 auto)  •  Last saved: %2")
                .arg(sum.used_files)
                .arg(date_str.isEmpty() ? QObject::tr("Saved") : date_str));

        int newest_idx = -1;
        QDateTime newest_dt;
        std::array<bool, 6> used{};
        std::array<QDateTime, 6> times{};
        std::array<QPixmap, 6> pics{};
        std::error_code ec;
        for (int idx = 0; idx < 6; ++idx) {
            const fs::path fdir = inspect_dir / fmt::format("slot_{:02d}", idx);
            const fs::path prog = fdir / "progress.sav";
            const fs::path cap_file = fdir / "caption.sav";
            used[static_cast<size_t>(idx)] = fs::is_regular_file(prog, ec);
            if (fs::is_regular_file(cap_file, ec)) {
                pics[static_cast<size_t>(idx)] = ExtractCaptionPixmap(cap_file);
                times[static_cast<size_t>(idx)] = QFileInfo(Text(cap_file)).lastModified();
            } else if (used[static_cast<size_t>(idx)]) {
                times[static_cast<size_t>(idx)] = QFileInfo(Text(prog)).lastModified();
            }
            if (used[static_cast<size_t>(idx)] && times[static_cast<size_t>(idx)].isValid() &&
                (!newest_dt.isValid() || times[static_cast<size_t>(idx)] > newest_dt)) {
                newest_dt = times[static_cast<size_t>(idx)];
                newest_idx = idx;
            }
        }
        std::array<int, 6> order{0, 1, 2, 3, 4, 5};
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const bool ua = used[static_cast<size_t>(a)];
            const bool ub = used[static_cast<size_t>(b)];
            if (ua != ub) {
                return ua;
            }
            const QDateTime& ta = times[static_cast<size_t>(a)];
            const QDateTime& tb = times[static_cast<size_t>(b)];
            if (ta.isValid() != tb.isValid()) {
                return ta.isValid();
            }
            if (ta.isValid() && tb.isValid() && ta != tb) {
                return ta > tb;
            }
            return a < b;
        });
        for (int card_pos = 0; card_pos < 6; ++card_pos) {
            const int idx = order[static_cast<size_t>(card_pos)];
            auto& mc = mini_cards[static_cast<size_t>(card_pos)];
            const bool is_used = used[static_cast<size_t>(idx)];
            const bool is_manual = (idx == 0);
            const bool is_newest = (idx == newest_idx);
            if (is_manual && is_used) {
                mc.frame->setStyleSheet(QStringLiteral(
                    "QFrame { background-color: #1c1910; border: 1.5px solid #ffd23f; border-radius: 6px; }"));
            } else if (is_newest) {
                mc.frame->setStyleSheet(QStringLiteral(
                    "QFrame { background-color: #161422; border: 1.5px solid #a78bfa; border-radius: 6px; }"));
            } else {
                mc.frame->setStyleSheet(QStringLiteral(
                    "QFrame { background-color: #0e0e13; border: 1px solid #252530; border-radius: 6px; }"));
            }
            const QPixmap& pix = pics[static_cast<size_t>(idx)];
            if (!pix.isNull()) {
                mc.img->setText(QString{});
                mc.img->setPixmap(RoundedThumbnail(pix, 136, 76, 5));
            } else {
                mc.img->setPixmap(QPixmap{});
                mc.img->setText(is_used ? QObject::tr("No preview") : QObject::tr("Empty"));
            }
            const QString tag =
                is_manual ? (is_newest ? QObject::tr("★ Manual (Latest)") : QObject::tr("★ Manual"))
                          : QObject::tr("Auto %1").arg(idx);
            const QDateTime& dt = times[static_cast<size_t>(idx)];
            const QString stamp = dt.isValid()
                                      ? dt.toString(QStringLiteral("MM-dd HH:mm"))
                                      : (is_used ? QObject::tr("Saved") : QObject::tr("Empty"));
            mc.cap->setStyleSheet(
                (is_manual && is_used)
                    ? QStringLiteral("color: #ffd23f; font-size: 10px; font-weight: 700;")
                    : (is_newest ? QStringLiteral("color: #c4b5fd; font-size: 10px; font-weight: 600;")
                                 : QStringLiteral("color: #a8a8b4; font-size: 10px;")));
            mc.cap->setText(QStringLiteral("%1 • %2").arg(tag, stamp));
        }
    };

    const auto refresh_slots = [&] {
        const int active = ReadActiveSlot(slots_root);
        int prev_row = slot_list->currentRow();
        if (prev_row < 0 || prev_row >= 8) {
            prev_row = active - 1;
        }
        slot_list->blockSignals(true);
        slot_list->clear();
        for (int i = 1; i <= 8; ++i) {
            const fs::path sdir = slots_root / fmt::format("slot_{}", i);
            const SlotMeta meta = ReadSlotMeta(sdir, i);
            const OotSaveSummary sum = InspectOotSaveDir(slot_inspect_dir(i, active), false);
            QString status_str = QObject::tr("Empty");
            if (sum.has_data) {
                const QString date_str =
                    sum.newest_time.isValid() ? FormatSaveDate(sum.newest_time) : meta.updated;
                status_str = QObject::tr("%1/6 files • %2")
                                 .arg(sum.used_files)
                                 .arg(date_str.isEmpty() ? QObject::tr("Saved") : date_str);
            }
            const bool is_active = (i == active);
            const QString text =
                is_active
                    ? QObject::tr("● [ACTIVE]  Slot %1 — %2   (%3)")
                          .arg(i)
                          .arg(meta.name, status_str)
                    : QObject::tr("   Slot %1 — %2   (%3)").arg(i).arg(meta.name, status_str);
            auto* item = new QListWidgetItem(text, slot_list);
            if (is_active) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
                item->setForeground(QColor(0xa7, 0x8b, 0xfa));
            } else if (!sum.has_data) {
                item->setForeground(QColor(0x77, 0x77, 0x84));
            }
        }
        slot_list->setCurrentRow(prev_row);
        slot_list->blockSignals(false);
        update_preview();
    };
    QObject::connect(slot_list, &QListWidget::currentRowChanged, &dialog,
                     [&](int) { update_preview(); });
    refresh_slots();

    auto* slot_btns = new QHBoxLayout;
    auto* btn_activate = new QPushButton(QObject::tr("Activate / Switch"), &dialog);
    auto* btn_save_here = new QPushButton(QObject::tr("Save Current Here"), &dialog);
    auto* btn_rename = new QPushButton(QObject::tr("Rename"), &dialog);
    auto* btn_export_slot = new QPushButton(QObject::tr("Export Slot (.zip)"), &dialog);
    auto* btn_clear_slot = new QPushButton(QObject::tr("Clear Slot"), &dialog);
    slot_btns->addWidget(btn_activate);
    slot_btns->addWidget(btn_save_here);
    slot_btns->addWidget(btn_rename);
    slot_btns->addWidget(btn_export_slot);
    slot_btns->addWidget(btn_clear_slot);
    layout->addLayout(slot_btns, row++, 0, 1, 2);

    auto* ask_on_play_cb = new QCheckBox(
        QObject::tr("Ask which save slot before playing (8-slot picker on Play)"), &dialog);
    {
        std::error_code ec;
        ask_on_play_cb->setChecked(!fs::is_regular_file(slots_root / "skip_play_picker.txt", ec));
    }
    QObject::connect(ask_on_play_cb, &QCheckBox::toggled, &dialog, [slots_root](bool checked) {
        std::error_code ec;
        const fs::path flag = slots_root / "skip_play_picker.txt";
        if (checked) {
            fs::remove(flag, ec);
        } else {
            fs::create_directories(slots_root, ec);
            std::ofstream out(flag, std::ios::trunc);
            if (out) {
                out << "1\n";
            }
        }
    });
    layout->addWidget(ask_on_play_cb, row++, 0, 1, 2);

    QObject::connect(btn_activate, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const int target = slot_list->currentRow() + 1;
        if (target < 1 || target > 8) {
            return;
        }
        const int active = ReadActiveSlot(slots_root);
        if (target == active) {
            QMessageBox::information(&dialog, QObject::tr("Multi-Save"),
                                     QObject::tr("Slot %1 is already active.").arg(target));
            return;
        }
        const fs::path target_dir = slots_root / fmt::format("slot_{}", target);
        const fs::path target_data = target_dir / "data";
        const fs::path active_dir = slots_root / fmt::format("slot_{}", active);
        const fs::path nand_oot = OotActiveNandDir(fritchii);

        if (!DirHasFiles(target_data)) {
            if (QMessageBox::question(
                    &dialog, QObject::tr("Multi-Save"),
                    QObject::tr("Slot %1 is empty. Switching to it will start a fresh Zelda OoT "
                                "campaign.\n\nYour current progress will be saved safely in Slot %2. "
                                "Continue?")
                        .arg(target)
                        .arg(active)) != QMessageBox::Yes) {
                return;
            }
        }
        try {
            const QString stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            if (DirHasFiles(nand_oot)) {
                SyncDirCopy(nand_oot, active_dir / "data");
                const SlotMeta active_meta = ReadSlotMeta(active_dir, active);
                WriteSlotMeta(active_dir, active_meta.name, stamp);
            }
            Backup(fritchii, BackupPath(fritchii));
            std::error_code ec;
            fs::remove_all(nand_oot, ec);
            if (DirHasFiles(target_data)) {
                SyncDirCopy(target_data, nand_oot);
            }
            WriteActiveSlot(slots_root, target);
            refresh_slots();
            QMessageBox::information(
                &dialog, QObject::tr("Multi-Save"),
                QObject::tr("Switched to Slot %1. Your previous progress is saved in Slot %2.")
                    .arg(target)
                    .arg(active));
        } catch (const std::exception& error) {
            failed(error);
        }
    });

    QObject::connect(btn_save_here, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const int sel = slot_list->currentRow() + 1;
        if (sel < 1 || sel > 8) {
            return;
        }
        const fs::path nand_oot = OotActiveNandDir(fritchii);
        if (!DirHasFiles(nand_oot)) {
            QMessageBox::information(&dialog, QObject::tr("Multi-Save"),
                                     QObject::tr("No active Zelda OoT save found to snapshot."));
            return;
        }
        const fs::path sdir = slots_root / fmt::format("slot_{}", sel);
        if (DirHasFiles(sdir / "data")) {
            if (QMessageBox::question(
                    &dialog, QObject::tr("Multi-Save"),
                    QObject::tr("Overwrite Slot %1 with your current Zelda OoT save?").arg(sel)) !=
                QMessageBox::Yes) {
                return;
            }
        }
        try {
            SyncDirCopy(nand_oot, sdir / "data");
            const SlotMeta meta = ReadSlotMeta(sdir, sel);
            const QString stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            WriteSlotMeta(sdir, meta.name, stamp);
            refresh_slots();
            QMessageBox::information(&dialog, QObject::tr("Multi-Save"),
                                     QObject::tr("Current Zelda OoT save stored in Slot %1.").arg(sel));
        } catch (const std::exception& error) {
            failed(error);
        }
    });

    QObject::connect(btn_rename, &QPushButton::clicked, &dialog, [&] {
        const int sel = slot_list->currentRow() + 1;
        if (sel < 1 || sel > 8) {
            return;
        }
        const fs::path sdir = slots_root / fmt::format("slot_{}", sel);
        const SlotMeta meta = ReadSlotMeta(sdir, sel);
        bool ok = false;
        const QString new_name =
            QInputDialog::getText(&dialog, QObject::tr("Rename Slot"),
                                  QObject::tr("Name for Slot %1:").arg(sel), QLineEdit::Normal,
                                  meta.name, &ok)
                .trimmed();
        if (ok && !new_name.isEmpty()) {
            WriteSlotMeta(sdir, new_name, meta.updated);
            refresh_slots();
        }
    });

    QObject::connect(btn_export_slot, &QPushButton::clicked, &dialog, [&] {
        const int sel = slot_list->currentRow() + 1;
        if (sel < 1 || sel > 8) {
            return;
        }
        const int active = ReadActiveSlot(slots_root);
        const fs::path data_dir = slot_inspect_dir(sel, active);
        if (!DirHasFiles(data_dir)) {
            QMessageBox::information(&dialog, QObject::tr("Multi-Save"),
                                     QObject::tr("Slot %1 is empty.").arg(sel));
            return;
        }
        const QString default_name = QObject::tr("Zelda_OoT_Slot_%1.zip").arg(sel);
        const QString file = QFileDialog::getSaveFileName(
            &dialog, QObject::tr("Export Slot %1").arg(sel),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/') +
                default_name,
            QObject::tr("Zip files (*.zip)"));
        if (file.isEmpty()) {
            return;
        }
        if (!JlCompress::compressDir(file, Text(data_dir), true)) {
            QMessageBox::warning(&dialog, QObject::tr("Multi-Save"),
                                 QObject::tr("Could not write %1").arg(file));
            return;
        }
        QMessageBox::information(&dialog, QObject::tr("Multi-Save"),
                                 QObject::tr("Slot %1 exported to:\n%2").arg(sel).arg(file));
    });

    QObject::connect(btn_clear_slot, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const int sel = slot_list->currentRow() + 1;
        if (sel < 1 || sel > 8) {
            return;
        }
        if (sel == ReadActiveSlot(slots_root)) {
            QMessageBox::warning(
                &dialog, QObject::tr("Multi-Save"),
                QObject::tr("Slot %1 is currently active. Switch to another slot first.").arg(sel));
            return;
        }
        const fs::path sdir = slots_root / fmt::format("slot_{}", sel);
        if (!DirHasFiles(sdir / "data")) {
            return;
        }
        if (QMessageBox::question(&dialog, QObject::tr("Multi-Save"),
                                  QObject::tr("Clear saved data in Slot %1?").arg(sel)) !=
            QMessageBox::Yes) {
            return;
        }
        std::error_code ec;
        fs::remove_all(sdir / "data", ec);
        WriteSlotMeta(sdir, ReadSlotMeta(sdir, sel).name, QString{});
        refresh_slots();
    });

    auto* sep = new QFrame(&dialog);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep, row++, 0, 1, 2);

    // Universal Export (Active Campaign or Any Slot -> Emulator / Triforce / NX2navi / Ryujinx / .zip)
    auto* export_button = new QPushButton(QObject::tr("Export..."), &dialog);
    layout->addWidget(
        new QLabel(
            QObject::tr("Export saves (Triforce, NX2navi, Ryujinx, Other Emulators or .zip)"),
            &dialog),
        row, 0);
    layout->addWidget(export_button, row, 1);
    ++row;

    QObject::connect(export_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const int active = ReadActiveSlot(slots_root);
        const auto detected = DetectAllOotSaves(fritchii);

        QDialog pick(&dialog);
        pick.setWindowTitle(QObject::tr("Export Saves"));
        pick.setMinimumWidth(480);
        auto* v = new QVBoxLayout(&pick);

        v->addWidget(new QLabel(QObject::tr("Which FritCheeLii save to export:"), &pick));
        auto* src_combo = new QComboBox(&pick);
        const fs::path active_oot = OotActiveNandDir(fritchii);
        if (DirHasFiles(active_oot)) {
            src_combo->addItem(
                QObject::tr("Active Campaign (Current Slot %1)").arg(active), 0);
        }
        for (int i = 1; i <= 8; ++i) {
            const fs::path sdata = slots_root / fmt::format("slot_{}", i) / "data";
            if (DirHasFiles(sdata)) {
                const SlotMeta sm = ReadSlotMeta(slots_root / fmt::format("slot_{}", i), i);
                src_combo->addItem(QObject::tr("Slot %1 — %2").arg(i).arg(sm.name), i);
            }
        }
        if (src_combo->count() == 0 && Profiles(fritchii).empty()) {
            QMessageBox::information(&dialog, QObject::tr("Saves"),
                                     QObject::tr("FritCheeLii has no saves yet."));
            return;
        }
        v->addWidget(src_combo);

        v->addWidget(new QLabel(QObject::tr("Choose destination:"), &pick));
        auto* dst_combo = new QComboBox(&pick);
        dst_combo->addItem(QObject::tr("Save as .zip backup file..."), QStringLiteral("zip"));
        dst_combo->addItem(
            QObject::tr("Choose folder (Triforce / NX2navi / Ryujinx / Any Emulator)..."),
            QStringLiteral("folder"));
        for (size_t i = 0; i < detected.size(); ++i) {
            const auto& d = detected[i];
            const QString info =
                d.summary.has_data
                    ? QObject::tr(" (%1 files, %2)")
                          .arg(d.summary.used_files)
                          .arg(FormatSaveDate(d.summary.newest_time))
                    : QString{};
            dst_combo->addItem(QObject::tr("Detected: %1%2").arg(d.name, info),
                               static_cast<int>(i));
        }
        v->addWidget(dst_combo);

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

        const int src_slot = src_combo->count() > 0 ? src_combo->currentData().toInt() : 0;
        const fs::path src_oot =
            (src_slot == 0) ? active_oot
                            : (slots_root / fmt::format("slot_{}", src_slot) / "data");
        const QVariant choice = dst_combo->currentData();

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
                const fs::path zip_src =
                    (src_slot == 0 && !DirHasFiles(src_oot)) ? fritchii : src_oot;
                if (!JlCompress::compressDir(file, Text(zip_src), true)) {
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
                    QObject::tr(
                        "Select destination folder (Triforce, NX2navi, Ryujinx or Emulator)"));
                if (dir.isEmpty()) {
                    return;
                }
                fs::path dst = fs::u8path(dir.toStdString());
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
                if (!emu_dst.empty() && src_slot == 0) {
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
                if (!DirHasFiles(src_oot)) {
                    QMessageBox::information(&dialog, QObject::tr("Saves"),
                                             QObject::tr("Selected save has no Ocarina of Time files."));
                    return;
                }
                if (fs::is_directory(dst / "user") && !fs::exists(dst / "slot_00") &&
                    fs::exists(dst / "user" / "save")) {
                    dst = dst / "user" / "save" / TRIFORCE_UID;
                } else if (fs::is_directory(dst / "profile") && !fs::exists(dst / "slot_00")) {
                    const fs::path navi = NaviSaveDir(dst);
                    dst = navi.empty() ? (dst / "profile" / "save") : navi;
                } else if (const fs::path existing = FindOotSaveIn(dst); !existing.empty()) {
                    dst = existing;
                }
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Export Ocarina of Time save to:\n%1?").arg(Text(dst)),
                             backup_path)) {
                    return;
                }
                const auto backup = Backup(dst, backup_path);
                SyncDirCopy(src_oot, dst);
                if (const fs::path tx = TriforceTxDir(dst); !tx.empty() && fs::exists(tx)) {
                    std::error_code ec;
                    fs::remove_all(tx, ec);
                }
                Done(&dialog, 1, backup);
            } else {
                const DetectedSaveSource& target = detected[static_cast<size_t>(choice.toInt())];
                if (!target.emu_save_root.empty() && src_slot == 0) {
                    const auto backup_path = BackupPath(target.emu_save_root);
                    if (!confirm(QObject::tr("Export FritCheeLii saves to %1? Close %1 first.")
                                     .arg(target.name),
                                 backup_path)) {
                        return;
                    }
                    const auto backup = Backup(target.emu_save_root, backup_path);
                    const int games = CopySaves(fritchii, target.emu_save_root);
                    Done(&dialog, games, backup);
                } else {
                    if (!DirHasFiles(src_oot)) {
                        QMessageBox::information(
                            &dialog, QObject::tr("Saves"),
                            QObject::tr("Selected save has no Ocarina of Time files."));
                        return;
                    }
                    const fs::path dst =
                        !target.oot_dir.empty()
                            ? target.oot_dir
                            : (ActiveProfileDir(target.emu_save_root) / OOT_TITLE_HEX);
                    const auto backup_path = BackupPath(fritchii);
                    if (!confirm(QObject::tr("Export Ocarina of Time save to %1? Close %1 first.")
                                     .arg(target.name),
                                 backup_path)) {
                        return;
                    }
                    const auto backup = Backup(dst, backup_path);
                    SyncDirCopy(src_oot, dst);
                    if (target.is_triforce) {
                        if (const fs::path tx = TriforceTxDir(dst);
                            !tx.empty() && fs::exists(tx)) {
                            std::error_code ec;
                            fs::remove_all(tx, ec);
                        }
                    }
                    Done(&dialog, 1, backup);
                }
            }
        } catch (const std::exception& error) {
            failed(error);
        }
    });

    // Universal Import (with Location Preview & Target Slot Selection)
    auto* import_button = new QPushButton(QObject::tr("Import..."), &dialog);
    layout->addWidget(
        new QLabel(
            QObject::tr("Import saves (Triforce, NX2navi, Ryujinx, Other Emulators or .zip)"),
            &dialog),
        row, 0);
    layout->addWidget(import_button, row, 1);
    ++row;

    QObject::connect(import_button, &QPushButton::clicked, &dialog, [&] {
        if (blocked()) {
            return;
        }
        const int active = ReadActiveSlot(slots_root);
        const auto detected = DetectAllOotSaves(fritchii);

        QDialog pick(&dialog);
        pick.setWindowTitle(QObject::tr("Import Saves"));
        pick.setMinimumWidth(500);
        auto* v = new QVBoxLayout(&pick);

        v->addWidget(new QLabel(QObject::tr("Choose where to import saves from:"), &pick));
        auto* combo = new QComboBox(&pick);
        for (size_t i = 0; i < detected.size(); ++i) {
            const auto& d = detected[i];
            const QString info =
                d.summary.has_data
                    ? QObject::tr(" (%1 files, %2)")
                          .arg(d.summary.used_files)
                          .arg(FormatSaveDate(d.summary.newest_time))
                    : QString{};
            combo->addItem(QObject::tr("Detected: %1%2").arg(d.name, info),
                           static_cast<int>(i));
        }
        combo->addItem(
            QObject::tr("Choose folder (Triforce / NX2navi / Ryujinx / Any Emulator)..."),
            QStringLiteral("folder"));
        combo->addItem(QObject::tr("Choose .zip backup file..."), QStringLiteral("zip"));
        v->addWidget(combo);

        // Live preview inside Import dialog for detected sources
        auto* imp_preview_box = new QFrame(&pick);
        imp_preview_box->setStyleSheet(QStringLiteral(
            "QFrame { background-color: #16161c; border: 1px solid #2c2c38; border-radius: 8px; }"));
        auto* imp_preview_row = new QHBoxLayout(imp_preview_box);
        imp_preview_row->setContentsMargins(10, 10, 10, 10);
        imp_preview_row->setSpacing(10);
        auto* imp_thumb = new QLabel(imp_preview_box);
        imp_thumb->setFixedSize(224, 126);
        imp_thumb->setAlignment(Qt::AlignCenter);
        imp_thumb->setStyleSheet(QStringLiteral(
            "background-color: #0e0e13; border: 1px solid #252530; border-radius: 6px; "
            "color: #777784; font-size: 11px;"));
        auto* imp_info = new QLabel(imp_preview_box);
        imp_info->setWordWrap(true);
        imp_info->setStyleSheet(QStringLiteral("border: none; color: #c8c8d4; font-size: 11px;"));
        imp_preview_row->addWidget(imp_thumb);
        imp_preview_row->addWidget(imp_info, 1);
        v->addWidget(imp_preview_box);

        const auto update_import_preview = [&] {
            const QVariant cur = combo->currentData();
            if (cur.typeId() == QMetaType::Int) {
                const auto& d = detected[static_cast<size_t>(cur.toInt())];
                imp_preview_box->setVisible(true);
                if (!d.summary.preview.isNull()) {
                    imp_thumb->setText(QString{});
                    imp_thumb->setPixmap(RoundedThumbnail(d.summary.preview, 224, 126, 6));
                } else {
                    imp_thumb->setPixmap(QPixmap{});
                    imp_thumb->setText(QObject::tr("No preview"));
                }
                const fs::path shown_path = !d.oot_dir.empty() ? d.oot_dir : d.emu_save_root;
                imp_info->setText(
                    QObject::tr("<b>%1</b><br>%2 / 6 files<br>Saved: %3<br><span style='color:#7c7c8a'>%4</span>")
                        .arg(d.name)
                        .arg(d.summary.used_files)
                        .arg(FormatSaveDate(d.summary.newest_time))
                        .arg(Text(shown_path)));
            } else {
                imp_preview_box->setVisible(false);
            }
        };
        QObject::connect(combo, &QComboBox::currentIndexChanged, &pick,
                         [&](int) { update_import_preview(); });
        update_import_preview();

        v->addWidget(new QLabel(QObject::tr("Import into:"), &pick));
        auto* dst_slot_combo = new QComboBox(&pick);
        dst_slot_combo->addItem(
            QObject::tr("Active Campaign (Current Slot %1)").arg(active), 0);
        for (int i = 1; i <= 8; ++i) {
            const SlotMeta sm = ReadSlotMeta(slots_root / fmt::format("slot_{}", i), i);
            dst_slot_combo->addItem(QObject::tr("Slot %1 — %2").arg(i).arg(sm.name), i);
        }
        v->addWidget(dst_slot_combo);

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
        const int target_slot = dst_slot_combo->currentData().toInt();
        const fs::path temp = fritchii.parent_path().parent_path() / "save-import";

        const auto apply_oot_import = [&](const fs::path& src_oot, const QString& label) {
            const QString stamp =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            if (target_slot == 0) {
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import Ocarina of Time save from %1 into your active "
                                         "FritCheeLii campaign (Slot %2)?")
                                 .arg(label)
                                 .arg(active),
                             backup_path)) {
                    return;
                }
                const auto backup = Backup(fritchii, backup_path);
                const fs::path target_nand = OotActiveNandDir(fritchii);
                SyncDirCopy(src_oot, target_nand);
                const fs::path active_dir = slots_root / fmt::format("slot_{}", active);
                SyncDirCopy(src_oot, active_dir / "data");
                const SlotMeta sm = ReadSlotMeta(active_dir, active);
                WriteSlotMeta(active_dir, sm.name, stamp);
                refresh_slots();
                Done(&dialog, 1, backup);
            } else {
                const fs::path sdir = slots_root / fmt::format("slot_{}", target_slot);
                const fs::path sdata = sdir / "data";
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import Ocarina of Time save from %1 into Slot %2?")
                                 .arg(label)
                                 .arg(target_slot),
                             backup_path)) {
                    return;
                }
                const auto backup =
                    DirHasFiles(sdata) ? Backup(sdata, backup_path) : Backup(fritchii, backup_path);
                SyncDirCopy(src_oot, sdata);
                const SlotMeta sm = ReadSlotMeta(sdir, target_slot);
                WriteSlotMeta(sdir, sm.name, stamp);
                if (target_slot == active) {
                    SyncDirCopy(src_oot, OotActiveNandDir(fritchii));
                }
                refresh_slots();
                Done(&dialog, 1, backup);
            }
        };

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
                if (const fs::path oot_in_zip = FindOotSaveIn(temp); !oot_in_zip.empty()) {
                    apply_oot_import(oot_in_zip, QObject::tr("the .zip file"));
                    fs::remove_all(temp);
                    return;
                }
                fs::path root = temp;
                if (Profiles(root).empty() && !Profiles(temp / "0000000000000000").empty()) {
                    root = temp / "0000000000000000";
                }
                if (Profiles(root).empty()) {
                    throw std::runtime_error("no compatible saves in that zip file");
                }
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import these saves into FritCheeLii?"), backup_path)) {
                    fs::remove_all(temp);
                    return;
                }
                const auto backup = Backup(fritchii, backup_path);
                const int games = CopySaves(root, fritchii);
                fs::remove_all(temp);
                refresh_slots();
                Done(&dialog, games, backup);
            } else if (choice.typeId() == QMetaType::QString &&
                       choice.toString() == QStringLiteral("folder")) {
                const QString dir = QFileDialog::getExistingDirectory(
                    &dialog,
                    QObject::tr(
                        "Select save folder (Triforce, NX2navi, Ryujinx or Emulator)"));
                if (dir.isEmpty()) {
                    return;
                }
                const fs::path picked = fs::u8path(dir.toStdString());
                if (const fs::path oot_found = FindOotSaveIn(picked); !oot_found.empty()) {
                    apply_oot_import(oot_found, Text(oot_found));
                    return;
                }
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
                if (emu_root.empty()) {
                    throw std::runtime_error(
                        "No compatible saves found in that folder.\n"
                        "Select a Triforce, NX2navi, Ryujinx, or Yuzu/Eden/Lichii save folder.");
                }
                const auto backup_path = BackupPath(fritchii);
                if (!confirm(QObject::tr("Import saves from this folder into FritCheeLii?"),
                             backup_path)) {
                    return;
                }
                const auto backup = Backup(fritchii, backup_path);
                const int games = CopySaves(emu_root, fritchii);
                refresh_slots();
                Done(&dialog, games, backup);
            } else {
                const DetectedSaveSource& source = detected[static_cast<size_t>(choice.toInt())];
                if (!source.oot_dir.empty()) {
                    apply_oot_import(source.oot_dir, source.name);
                } else if (!source.emu_save_root.empty() && !Profiles(source.emu_save_root).empty()) {
                    const auto backup_path = BackupPath(fritchii);
                    if (!confirm(QObject::tr("Import saves from %1 into FritCheeLii?")
                                     .arg(source.name),
                                 backup_path)) {
                        return;
                    }
                    const auto backup = Backup(fritchii, backup_path);
                    const int games = CopySaves(source.emu_save_root, fritchii);
                    refresh_slots();
                    Done(&dialog, games, backup);
                } else {
                    QMessageBox::information(&dialog, QObject::tr("Saves"),
                                             QObject::tr("%1 has no saves.").arg(source.name));
                }
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
                    fs::remove_all(orphan);
                }
                fix_button->setEnabled(false);
                refresh_slots();
                Done(&dialog, games, backup);
            } catch (const std::exception& error) {
                failed(error);
            }
        });
    }

    auto* bottom = new QHBoxLayout;
    auto* open_saves_btn = new QPushButton(QObject::tr("Open Saves Folder"), &dialog);
    auto* open_backups_btn = new QPushButton(QObject::tr("Open Backups"), &dialog);
    auto* close = new QPushButton(QObject::tr("Close"), &dialog);
    bottom->addWidget(open_saves_btn);
    bottom->addWidget(open_backups_btn);
    bottom->addStretch(1);
    bottom->addWidget(close);
    layout->addLayout(bottom, row, 0, 1, 2);

    QObject::connect(open_saves_btn, &QPushButton::clicked, &dialog, [&] {
        const fs::path nand_oot = OotActiveNandDir(fritchii);
        const fs::path to_open = DirHasFiles(nand_oot) ? nand_oot : fritchii;
        std::error_code ec;
        fs::create_directories(to_open, ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(Text(to_open)));
    });
    QObject::connect(open_backups_btn, &QPushButton::clicked, &dialog, [&] {
        const fs::path backups_root = fritchii.parent_path().parent_path() / "save-backups";
        std::error_code ec;
        if (!fs::is_directory(backups_root, ec)) {
            QMessageBox::information(
                &dialog, QObject::tr("Saves"),
                QObject::tr("No backups have been created yet. Backups are saved automatically "
                            "before replacing or switching saves."));
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(Text(backups_root)));
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

std::vector<OotSlotBrief> ListOotSaveSlots() {
    const fs::path fritchii = SaveRoot(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));
    const fs::path slots_root = OotSlotsRoot(fritchii);
    const fs::path nand_oot = OotActiveNandDir(fritchii);
    EnsureSlot1Seeded(fritchii, slots_root);
    const int active = ReadActiveSlot(slots_root);

    std::vector<OotSlotBrief> out;
    out.reserve(8);
    for (int i = 1; i <= 8; ++i) {
        const fs::path sdir = slots_root / fmt::format("slot_{}", i);
        const SlotMeta meta = ReadSlotMeta(sdir, i);
        const fs::path inspect_dir = (i == active && DirHasFiles(nand_oot)) ? nand_oot : (sdir / "data");
        const OotSaveSummary sum = InspectOotSaveDir(inspect_dir, false);
        OotSlotBrief brief;
        brief.slot = i;
        brief.name = meta.name;
        brief.has_data = sum.has_data;
        brief.active = (i == active);
        if (sum.has_data) {
            brief.date_str =
                sum.newest_time.isValid() ? FormatSaveDate(sum.newest_time) : meta.updated;
        }
        out.push_back(std::move(brief));
    }
    return out;
}

bool SwitchOotSaveSlot(int target_slot) {
    const int target = std::clamp(target_slot, 1, 8);
    const fs::path fritchii = SaveRoot(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));
    const fs::path slots_root = OotSlotsRoot(fritchii);
    const fs::path nand_oot = OotActiveNandDir(fritchii);
    EnsureSlot1Seeded(fritchii, slots_root);
    const int active = ReadActiveSlot(slots_root);
    if (target == active) {
        return true;
    }
    try {
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        if (DirHasFiles(nand_oot)) {
            const fs::path active_dir = slots_root / fmt::format("slot_{}", active);
            SyncDirCopy(nand_oot, active_dir / "data");
            const SlotMeta active_meta = ReadSlotMeta(active_dir, active);
            WriteSlotMeta(active_dir, active_meta.name, stamp);
        }
        const fs::path target_data = slots_root / fmt::format("slot_{}", target) / "data";
        std::error_code ec;
        fs::remove_all(nand_oot, ec);
        if (DirHasFiles(target_data)) {
            SyncDirCopy(target_data, nand_oot);
        }
        WriteActiveSlot(slots_root, target);
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

class SlotCardFilter final : public QObject {
public:
    SlotCardFilter(int slot_id, std::function<void(int)> on_pick,
                   std::function<void(int)> on_launch, QObject* parent)
        : QObject(parent), slot{slot_id}, pick{std::move(on_pick)}, launch{std::move(on_launch)} {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress) {
            pick(slot);
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            launch(slot);
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    int slot;
    std::function<void(int)> pick;
    std::function<void(int)> launch;
};

} // namespace

bool ChooseOotSaveSlotBeforePlay(QWidget* parent, const std::function<bool()>& emulation_running,
                                 bool force_show, bool launch_mode) {
    const fs::path fritchii = SaveRoot(Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir));
    const fs::path slots_root = OotSlotsRoot(fritchii);
    const fs::path nand_oot = OotActiveNandDir(fritchii);
    EnsureSlot1Seeded(fritchii, slots_root);

    std::error_code ec;
    if (!force_show && fs::is_regular_file(slots_root / "skip_play_picker.txt", ec)) {
        return true;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(launch_mode ? QObject::tr("Choose Save Slot — Zelda OoT")
                                      : QObject::tr("Choose Active Save Slot — Zelda OoT"));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setMinimumWidth(680);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(10);

    auto* header = new QLabel(
        launch_mode
            ? QObject::tr("<b>Select which Save Slot to load for Zelda OoT</b><br>"
                          "<span style='color:#96969e;font-size:12px;'>All 8 slots are ready to "
                          "use — double-click a slot to launch immediately.</span>")
            : QObject::tr("<b>Select which Save Slot should be active for Zelda OoT</b><br>"
                          "<span style='color:#96969e;font-size:12px;'>All 8 slots are ready to "
                          "use — double-click a slot to switch immediately.</span>"),
        &dialog);
    layout->addWidget(header);

    auto* grid = new QGridLayout;
    grid->setSpacing(10);

    struct PickerCard {
        QFrame* frame = nullptr;
        QLabel* img = nullptr;
        QLabel* title = nullptr;
        QLabel* meta = nullptr;
    };
    std::array<PickerCard, 8> cards{};
    int selected_slot = ReadActiveSlot(slots_root);

    const auto update_styles = [&] {
        const int active = ReadActiveSlot(slots_root);
        for (int i = 0; i < 8; ++i) {
            const int slot_num = i + 1;
            const bool is_sel = (slot_num == selected_slot);
            const bool is_act = (slot_num == active);
            cards[static_cast<size_t>(i)].frame->setStyleSheet(
                is_sel ? QStringLiteral(
                             "QFrame { background-color: #191628; border: 2px solid #a78bfa; "
                             "border-radius: 8px; }")
                       : (is_act ? QStringLiteral(
                                       "QFrame { background-color: #14141b; border: 1px solid #ffd23f; "
                                       "border-radius: 8px; }")
                                 : QStringLiteral(
                                       "QFrame { background-color: #111117; border: 1px solid #262632; "
                                       "border-radius: 8px; }")));
        }
    };

    const auto populate_cards = [&] {
        const int active = ReadActiveSlot(slots_root);
        for (int i = 0; i < 8; ++i) {
            const int slot_num = i + 1;
            const fs::path sdir = slots_root / fmt::format("slot_{}", slot_num);
            const SlotMeta meta = ReadSlotMeta(sdir, slot_num);
            const fs::path inspect_dir =
                (slot_num == active && DirHasFiles(nand_oot)) ? nand_oot : (sdir / "data");
            const OotSaveSummary sum = InspectOotSaveDir(inspect_dir, true);
            auto& card = cards[static_cast<size_t>(i)];
            if (sum.has_data && !sum.preview.isNull()) {
                card.img->setText(QString{});
                card.img->setPixmap(RoundedThumbnail(sum.preview, 144, 80, 5));
            } else {
                card.img->setPixmap(QPixmap{});
                card.img->setText(sum.has_data ? QObject::tr("Saved") : QObject::tr("Empty — New Save"));
            }
            const QString badge = (slot_num == active) ? QObject::tr(" [ACTIVE]") : QString{};
            card.title->setText(QObject::tr("Slot %1 — %2%3").arg(slot_num).arg(meta.name, badge));
            if (sum.has_data) {
                const QString d = sum.newest_time.isValid() ? FormatSaveDate(sum.newest_time) : meta.updated;
                card.meta->setText(d.isEmpty() ? QObject::tr("%1 / 6 files").arg(sum.used_files)
                                               : QObject::tr("%1 / 6 files • %2").arg(sum.used_files).arg(d));
            } else {
                card.meta->setText(QObject::tr("Empty slot"));
            }
        }
        update_styles();
    };

    const auto pick_slot = [&](int slot_num) {
        selected_slot = slot_num;
        update_styles();
    };
    const auto launch_slot = [&](int slot_num) {
        selected_slot = slot_num;
        dialog.accept();
    };

    for (int i = 0; i < 8; ++i) {
        auto* frame = new QFrame(&dialog);
        frame->setCursor(Qt::PointingHandCursor);
        auto* vbox = new QVBoxLayout(frame);
        vbox->setContentsMargins(8, 8, 8, 8);
        vbox->setSpacing(4);

        auto* img = new QLabel(frame);
        img->setFixedSize(144, 80);
        img->setAlignment(Qt::AlignCenter);
        img->setStyleSheet(QStringLiteral(
            "background-color: #0b0b10; border-radius: 5px; color: #7a7a88; font-size: 11px;"));
        vbox->addWidget(img, 0, Qt::AlignCenter);

        auto* title = new QLabel(frame);
        title->setStyleSheet(QStringLiteral("color: #f4f4f0; font-size: 11px; font-weight: 600;"));
        title->setAlignment(Qt::AlignCenter);
        vbox->addWidget(title);

        auto* meta = new QLabel(frame);
        meta->setStyleSheet(QStringLiteral("color: #96969e; font-size: 10px;"));
        meta->setAlignment(Qt::AlignCenter);
        vbox->addWidget(meta);

        auto* filter = new SlotCardFilter(i + 1, pick_slot, launch_slot, frame);
        frame->installEventFilter(filter);
        img->installEventFilter(filter);
        title->installEventFilter(filter);
        meta->installEventFilter(filter);

        cards[static_cast<size_t>(i)] = PickerCard{frame, img, title, meta};
        grid->addWidget(frame, i / 4, i % 4);
    }
    layout->addLayout(grid);

    populate_cards();

    auto* skip_cb =
        new QCheckBox(QObject::tr("Don't ask again (launch active slot directly)"), &dialog);
    skip_cb->setVisible(launch_mode);
    layout->addWidget(skip_cb);

    auto* bottom = new QHBoxLayout;
    auto* manage_btn = new QPushButton(QObject::tr("Manage Saves..."), &dialog);
    auto* cancel_btn = new QPushButton(QObject::tr("Cancel"), &dialog);
    auto* play_btn = new QPushButton(
        launch_mode ? QObject::tr("Play Selected Slot") : QObject::tr("Use Selected Slot"),
        &dialog);
    play_btn->setDefault(true);
    play_btn->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #7c3aed; color: #ffffff; font-weight: 700; "
        "padding: 7px 16px; border-radius: 6px; border: none; }"
        "QPushButton:hover { background-color: #8b5cf6; }"));
    bottom->addWidget(manage_btn);
    bottom->addStretch(1);
    bottom->addWidget(cancel_btn);
    bottom->addWidget(play_btn);
    layout->addLayout(bottom);

    QObject::connect(manage_btn, &QPushButton::clicked, &dialog, [&] {
        ShowSavesWindow(&dialog, emulation_running);
        selected_slot = ReadActiveSlot(slots_root);
        populate_cards();
    });
    QObject::connect(cancel_btn, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(play_btn, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    if (skip_cb->isChecked()) {
        std::error_code dir_ec;
        fs::create_directories(slots_root, dir_ec);
        std::ofstream skip_out(slots_root / "skip_play_picker.txt", std::ios::trunc);
        if (skip_out) {
            skip_out << "1\n";
        }
    }
    return SwitchOotSaveSlot(selected_slot);
}

void ExportLogsZip(QWidget* parent) {
    const fs::path log_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    if (!DirHasFiles(log_dir)) {
        QMessageBox::information(parent, QObject::tr("Export Logs"),
                                 QObject::tr("No log files were found yet."));
        return;
    }
    QString base_dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (base_dir.isEmpty()) {
        base_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm"));
    const QString default_path =
        base_dir + QStringLiteral("/FritCheeLii-Logs-%1.zip").arg(stamp);
    const QString dest_zip = QFileDialog::getSaveFileName(
        parent, QObject::tr("Export Logs (.zip)"), default_path,
        QObject::tr("Zip Archive (*.zip)"));
    if (dest_zip.isEmpty()) {
        return;
    }
    if (JlCompress::compressDir(dest_zip, Text(log_dir), true)) {
        QMessageBox::information(
            parent, QObject::tr("Export Logs"),
            QObject::tr("Logs exported successfully to:\n%1").arg(dest_zip));
    } else {
        QMessageBox::warning(
            parent, QObject::tr("Export Logs"),
            QObject::tr("Failed to export logs to:\n%1").arg(dest_zip));
    }
}

} // namespace FritchiiUi
