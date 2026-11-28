// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/fritchii/home.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include <fmt/format.h>
#include <QApplication>
#include <QBoxLayout>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>

#include "common/fs/path_util.h"
#include "qt_common/game_list/game_list_p.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "yuzu/fritchii/fritchii_ui.h"
#include "yuzu/fritchii/saves_window.h"

namespace FritchiiUi {
namespace {

constexpr u64 OcarinaOfTime = 0x0400486024DEC000ULL;
constexpr int CoverSize = 200;
constexpr int CoverWidthWide = 356;
constexpr int TileIcon = 72;
constexpr int MaxTiles = 7;

struct Game {
    QString title;
    QString path;
    u64 program_id = 0;
    QPixmap icon;
};

QPixmap Rounded(const QPixmap& source, int size, int radius) {
    const qreal ratio = qApp->devicePixelRatio();
    QPixmap out(QSize(size, size) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
        painter.setClipPath(clip);
        if (source.isNull()) {
            painter.fillRect(QRect(0, 0, size, size), QColor(0x24, 0x23, 0x2c));
        } else {
            painter.drawPixmap(QRect(0, 0, size, size), source);
        }
    }
    return out;
}

QPixmap RoundedBanner(const QPixmap& source, int width, int height, int radius) {
    const qreal ratio = qApp->devicePixelRatio();
    QPixmap out(QSize(width, height) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, width, height), radius, radius);
        painter.setClipPath(clip);
        if (source.isNull()) {
            painter.fillRect(QRect(0, 0, width, height), QColor(0x24, 0x23, 0x2c));
        } else {
            QPixmap normalized = source;
            normalized.setDevicePixelRatio(1.0);
            const QSize target_px = out.size();
            const QPixmap scaled = normalized.scaled(target_px, Qt::KeepAspectRatioByExpanding,
                                                     Qt::SmoothTransformation);
            const int x = (scaled.width() - target_px.width()) / 2;
            const int y = (scaled.height() - target_px.height()) / 2;
            const QPixmap cropped = scaled.copy(x, y, target_px.width(), target_px.height());
            painter.drawPixmap(QRect(0, 0, width, height), cropped);
        }
    }
    return out;
}

/// True for a missing or fully transparent icon (some dumps have none).
bool IsBlank(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return true;
    }
    const QImage image = pixmap.toImage();
    const int step_x = std::max(1, image.width() / 16);
    const int step_y = std::max(1, image.height() / 16);
    for (int y = 0; y < image.height(); y += step_y) {
        for (int x = 0; x < image.width(); x += step_x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return false;
            }
        }
    }
    return true;
}

/// A cover the player put next to the game file: "<file name>.png" or "icon.png" (the same files
/// Game Settings > Detect Icon looks at first). Read only; the game file is never changed.
QPixmap LocalCover(const QString& game_path) {
    const QFileInfo info(game_path);
    const QDir dir = info.absoluteDir();
    for (const QString& base : {info.completeBaseName(), QStringLiteral("icon")}) {
        for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
            const QFileInfo candidate(dir.filePath(base + QLatin1String(ext)));
            if (!candidate.isFile() || candidate.size() > 16 * 1024 * 1024) {
                continue;
            }
            QPixmap pixmap;
            if (pixmap.load(candidate.filePath())) {
                return pixmap;
            }
        }
    }
    return {};
}

std::vector<Game> CollectGames(QStandardItemModel* model, const QPixmap& fallback) {
    std::vector<Game> games;
    std::set<QString> seen;
    if (!model) {
        return games;
    }
    const auto add = [&](const QStandardItem* item) {
        if (item->data(GameListItem::TypeRole).toInt() !=
            static_cast<int>(GameListItemType::Game)) {
            return;
        }
        const QString path = item->data(GameListItemPath::FullPathRole).toString();
        if (path.isEmpty() || !seen.insert(path).second) {
            return;
        }
        Game game;
        game.path = path;
        game.program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
        game.title = item->data(GameListItemPath::TitleRole).toString();
        if (game.title.isEmpty()) {
            game.title = QFileInfo(path).completeBaseName();
        }
        if (game.program_id == OcarinaOfTime) {
            game.icon = LocalCover(path);
            if (IsBlank(game.icon)) {
                game.icon = QPixmap(QStringLiteral(":/fritchii/icons/oot_cover.jpg"));
            }
        }
        if (IsBlank(game.icon)) {
            game.icon = item->data(Qt::DecorationRole).value<QPixmap>();
        }
        if (IsBlank(game.icon)) {
            game.icon = LocalCover(path);
        }
        if (IsBlank(game.icon)) {
            // No icon in the dump and none next to it: Fritchii's own icon, not an empty square.
            game.icon = fallback;
        }
        games.push_back(std::move(game));
    };
    for (int row = 0; row < model->rowCount(); ++row) {
        const QStandardItem* top = model->item(row);
        if (!top) {
            continue;
        }
        if (top->hasChildren()) {
            for (int child = 0; child < top->rowCount(); ++child) {
                add(top->child(child));
            }
        } else {
            add(top);
        }
    }
    return games;
}

} // namespace

Home::Home(QMainWindow* window_, QBoxLayout* central, QWidget* game_list_, QWidget* placeholder_,
           HomeHooks hooks_)
    : QObject(window_), window{window_}, game_list{game_list_}, placeholder{placeholder_},
      hooks{std::move(hooks_)} {
    // The classic menus live behind the rail's menu button. A zero-height bar keeps every
    // shortcut working and stays out of the way when fullscreen shows the bar again.
    window->menuBar()->setFixedHeight(0);

    rail = MakeRail();
    page = MakePage();
    central->insertWidget(0, rail);
    central->addWidget(page, 1);

    game_list->installEventFilter(this);
    placeholder->installEventFilter(this);

    refresh_timer = new QTimer(this);
    refresh_timer->setSingleShot(true);
    refresh_timer->setInterval(150);
    connect(refresh_timer, &QTimer::timeout, this, &Home::Refresh);
    if (hooks.model) {
        const auto later = [this] { refresh_timer->start(); };
        connect(hooks.model, &QAbstractItemModel::rowsInserted, this, later);
        connect(hooks.model, &QAbstractItemModel::rowsRemoved, this, later);
        connect(hooks.model, &QAbstractItemModel::modelReset, this, later);
        connect(hooks.model, &QAbstractItemModel::dataChanged, this, later);
    }
    QTimer::singleShot(0, this, &Home::Restore);
}

QToolButton* Home::RailButton(const QString& icon, const QString& tip) {
    auto* button = new QToolButton(rail);
    button->setObjectName(QStringLiteral("FritchiiRailButton"));
    button->setIcon(Icon(icon, 22));
    button->setIconSize(QSize(22, 22));
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QWidget* Home::MakeRail() {
    rail = new QWidget;
    rail->setObjectName(QStringLiteral("FritchiiRail"));
    rail->setAttribute(Qt::WA_StyledBackground);
    rail->setFixedWidth(64);
    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(11, 16, 11, 16);
    layout->setSpacing(8);

    library_button = RailButton(QStringLiteral("library"), tr("Library"));
    library_button->setCheckable(true);
    connect(library_button, &QToolButton::clicked, this, &Home::ShowHome);

    QToolButton* settings = RailButton(QStringLiteral("settings"), tr("Settings"));
    connect(settings, &QToolButton::clicked, this, [this] { hooks.open_settings(); });

    QToolButton* help = RailButton(QStringLiteral("help"), tr("Help"));
    connect(help, &QToolButton::clicked, this, [this, help] {
        if (hooks.help_menu) {
            hooks.help_menu->exec(help->mapToGlobal(QPoint(help->width() + 8, 0)));
        }
    });

    auto* classic = new QMenu(rail);
    for (QMenu* menu : hooks.classic_menus) {
        classic->addMenu(menu);
    }
    QToolButton* menu_button = RailButton(QStringLiteral("menu"), tr("Menu"));
    connect(menu_button, &QToolButton::clicked, this, [classic, menu_button] {
        const QSize size = classic->sizeHint();
        classic->exec(menu_button->mapToGlobal(
            QPoint(menu_button->width() + 8, menu_button->height() - size.height())));
    });

    // Discord and Donate are on the home page itself (MakePage), with their names.
    layout->addWidget(library_button);
    layout->addWidget(settings);
    layout->addWidget(help);
    layout->addStretch(1);
    layout->addWidget(menu_button);
    return rail;
}

QWidget* Home::MakePage() {
    page = new QWidget;
    page->setObjectName(QStringLiteral("FritchiiHome"));
    page->setAttribute(Qt::WA_StyledBackground);
    page->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(page, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (hero_path.isEmpty()) {
            return;
        }
        // The game list's full menu (modders use its folders and dumps), then Delete Shader Cache.
        QMenu menu;
        if (hooks.game_menu) {
            hooks.game_menu(menu, hero_id, hero_path.toStdString());
        } else {
            menu.addAction(tr("Game Settings..."), &menu, [this] {
                hooks.game_settings(hero_id, hero_path.toStdString());
            });
        }
        menu.addSeparator();
        menu.addAction(tr("Delete Shader Cache..."), &menu,
                       [this] { hooks.delete_shader_cache(hero_id); });
        menu.exec(page->mapToGlobal(pos));
    });

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(56, 24, 56, 36);
    layout->setSpacing(0);

    // Discord and Donate in plain sight at the top right, with their names (2026-11-10: the
    // rail's bare icons were easy to miss).
    auto* links = new QHBoxLayout;
    links->setSpacing(10);
    links->addStretch(1);
    const auto link = [this](const QString& icon, const QString& text, void (*open)()) {
        auto* button = new QPushButton(Icon(icon, 18), text, page);
        button->setObjectName(QStringLiteral("FritchiiLink"));
        button->setIconSize(QSize(18, 18));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, [open] { open(); });
        return button;
    };
    // v5.8.3: the Saves window (export, import, copy between Fritchii, Eden and Lichii).
    auto* saves = new QPushButton(Icon(QStringLiteral("list"), 18), tr("Saves"), page);
    saves->setObjectName(QStringLiteral("FritchiiLink"));
    saves->setIconSize(QSize(18, 18));
    saves->setCursor(Qt::PointingHandCursor);
    saves->setFocusPolicy(Qt::NoFocus);
    connect(saves, &QPushButton::clicked, this, [this] {
        ShowSavesWindow(window, hooks.emulation_running);
        RefreshSaveSlots();
    });
    links->addWidget(saves);
    auto* shader_cache_btn =
        new QPushButton(Icon(QStringLiteral("sliders"), 18), tr("Shader Cache"), page);
    shader_cache_btn->setObjectName(QStringLiteral("FritchiiLink"));
    shader_cache_btn->setIconSize(QSize(18, 18));
    shader_cache_btn->setCursor(Qt::PointingHandCursor);
    shader_cache_btn->setFocusPolicy(Qt::NoFocus);
    connect(shader_cache_btn, &QPushButton::clicked, this, [this] {
        if (hooks.emulation_running && hooks.emulation_running()) {
            QMessageBox::information(window, tr("Shader Cache"), tr("Stop the game first."));
            return;
        }
        if (hero_path.isEmpty()) {
            QMessageBox::information(window, tr("Shader Cache"),
                                     tr("Add your game folder first to manage or pre-build its "
                                        "Shader Cache."));
            return;
        }
        QDialog dlg(window);
        dlg.setWindowTitle(tr("Shader Cache Manager"));
        dlg.setMinimumWidth(510);
        auto* vbox = new QVBoxLayout(&dlg);
        vbox->setContentsMargins(20, 18, 20, 18);
        vbox->setSpacing(12);

        auto* hdr = new QLabel(
            tr("<b>Pre-build or manage Shader Cache without starting gameplay</b><br>"
               "<span style='color:#96969e;font-size:12px;'>You can leave shaders building here "
               "while idle and play later with zero wait time.</span>"),
            &dlg);
        hdr->setWordWrap(true);
        vbox->addWidget(hdr);

        const u64 tid = hero_id ? hero_id : OcarinaOfTime;
        const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
                                fmt::format("{:016x}", tid);
        u64 recipe_done_entries = 0;
        u64 compute_done_entries = 0;
        {
            std::ifstream rec_in(shader_dir / "precompiled.fork-v54.done");
            u64 sz = 0;
            s64 wt = 0;
            rec_in >> sz >> wt >> recipe_done_entries;
        }
        {
            std::ifstream cmp_in(shader_dir / "warm-compute.done");
            u64 sz = 0;
            s64 wt = 0;
            cmp_in >> sz >> wt >> compute_done_entries;
        }
        const u64 init_gfx = VideoCommon::ShaderWarm::FastEssentialGraphicsCount();
        const u64 init_cmp = VideoCommon::ShaderWarm::FastEssentialComputeCount();
        const u64 init_total = init_gfx + init_cmp;
        const u64 tot_gfx = VideoCommon::ShaderWarm::TotalRealGraphicsCount();
        const u64 tot_cmp = VideoCommon::ShaderWarm::TotalRealComputeCount();
        const u64 tot_total = tot_gfx + tot_cmp;

        const bool fast_ready = (recipe_done_entries > 0);
        const bool full_ready = (recipe_done_entries >= tot_gfx);
        std::error_code cache_ec;
        const bool has_any_cache =
            fast_ready || (compute_done_entries > 0) ||
            std::filesystem::exists(shader_dir / "vulkan.fork-v54.bin", cache_ec) ||
            std::filesystem::exists(shader_dir / "precache-graphics.bin", cache_ec);

        const QLocale loc;
        const QString init_total_s = loc.toString(static_cast<qulonglong>(init_total));
        const QString init_gfx_s = loc.toString(static_cast<qulonglong>(init_gfx));
        const QString init_cmp_s = loc.toString(static_cast<qulonglong>(init_cmp));
        const QString tot_total_s = loc.toString(static_cast<qulonglong>(tot_total));
        const QString tot_gfx_s = loc.toString(static_cast<qulonglong>(tot_gfx));
        const QString tot_cmp_s = loc.toString(static_cast<qulonglong>(tot_cmp));

        auto* status_box = new QLabel(&dlg);
        status_box->setWordWrap(true);
        if (full_ready) {
            status_box->setStyleSheet(QStringLiteral(
                "QLabel { background-color: #102419; color: #6ee7b7; border: 1px solid #059669; "
                "border-radius: 8px; padding: 10px 14px; font-size: 13px; font-weight: 700; }"));
            status_box->setText(
                tr("✔  Status: Total Shader Cache Built (%1 real shaders ready)").arg(tot_total_s));
        } else if (fast_ready) {
            status_box->setStyleSheet(QStringLiteral(
                "QLabel { background-color: #102419; color: #6ee7b7; border: 1px solid #059669; "
                "border-radius: 8px; padding: 10px 14px; font-size: 13px; font-weight: 700; }"));
            status_box->setText(
                tr("✔  Status: Initial Shader Cache Built (%1 real shaders ready to play!)")
                    .arg(init_total_s));
        } else {
            status_box->setStyleSheet(QStringLiteral(
                "QLabel { background-color: #181822; color: #d4d4dc; border: 1px solid #2e2e3a; "
                "border-radius: 8px; padding: 10px 14px; font-size: 13px; font-weight: 600; }"));
            status_box->setText(
                tr("⚪  Status: Not pre-built yet (Initial Shader Cache will run on first Play)"));
        }
        vbox->addWidget(status_box);

        const auto make_action_btn = [&](const QString& label, const QString& desc,
                                         const char* border_color, const char* bg_color) {
            auto* btn = new QPushButton(&dlg);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; color: #f4f4f0; border: 1px solid %2; "
                "border-radius: 8px; padding: 12px 14px; text-align: left; font-size: 13px; "
                "font-weight: 600; }"
                "QPushButton:hover { border-color: #a78bfa; background-color: #1e1a2e; }")
                                   .arg(QString::fromLatin1(bg_color),
                                        QString::fromLatin1(border_color)));
            btn->setText(label + QStringLiteral("\n") + desc);
            vbox->addWidget(btn);
            return btn;
        };

        auto* fast_btn = make_action_btn(
            fast_ready
                ? tr("✔  Initial Shader Cache (%1 shaders)  [BUILT / READY]").arg(init_total_s)
                : tr("▶  Initial Shader Cache (%1 shaders)").arg(init_total_s),
            tr("1st dungeon up to Hyrule Castle (%1 graphics + %2 compute). Fast start && "
               "mandatory for Play.")
                .arg(init_gfx_s, init_cmp_s),
            fast_ready ? "#059669" : "#7c3aed", fast_ready ? "#11221a" : "#181526");
        auto* full_btn = make_action_btn(
            full_ready
                ? tr("✔  Total Shader Cache (%1 shaders)  [BUILT / READY]").arg(tot_total_s)
                : tr("▶  Total Shader Cache (%1 shaders)").arg(tot_total_s),
            tr("All real-gameplay shaders collected so far (%1 graphics + %2 compute, up to "
               "Forest Temple).")
                .arg(tot_gfx_s, tot_cmp_s),
            full_ready ? "#059669" : "#2e2e3a", full_ready ? "#11221a" : "#14141b");
        auto* clear_btn = make_action_btn(
            has_any_cache ? tr("🗑  Clear Shader Cache")
                          : tr("🗑  Clear Shader Cache  (Already Empty)"),
            tr("Deletes compiled shaders (use after changing your graphics card/driver or to fix "
               "visual glitches)."),
            "#4a2228", "#1a1215");

        auto* close_row = new QHBoxLayout;
        close_row->addStretch(1);
        auto* cancel_btn = new QPushButton(tr("Close"), &dlg);
        connect(cancel_btn, &QPushButton::clicked, &dlg, &QDialog::reject);
        close_row->addWidget(cancel_btn);
        vbox->addLayout(close_row);

        int action = 0;
        connect(fast_btn, &QPushButton::clicked, &dlg, [&] {
            action = 1;
            dlg.accept();
        });
        connect(full_btn, &QPushButton::clicked, &dlg, [&] {
            action = 2;
            dlg.accept();
        });
        connect(clear_btn, &QPushButton::clicked, &dlg, [&] {
            action = 3;
            dlg.accept();
        });
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }
        if (action == 1 && hooks.build_shader_cache) {
            hooks.build_shader_cache(hero_path, hero_id, 1);
        } else if (action == 2 && hooks.build_shader_cache) {
            hooks.build_shader_cache(hero_path, hero_id, 2);
        } else if (action == 3 && hooks.delete_shader_cache) {
            hooks.delete_shader_cache(hero_id ? hero_id : OcarinaOfTime);
        }
    });
    links->addWidget(shader_cache_btn);
    auto* credits = new QPushButton(Icon(QStringLiteral("heart"), 18),
                                    tr("Credits"), page);
    credits->setObjectName(QStringLiteral("FritchiiLink"));
    credits->setIconSize(QSize(18, 18));
    credits->setCursor(Qt::PointingHandCursor);
    credits->setFocusPolicy(Qt::NoFocus);
    connect(credits, &QPushButton::clicked, this, [this] {
        QMessageBox::information(
            window, tr("Credits"),
            tr("<b>FritCheeLii %1</b><br><br>"
               "Credits to the open-source projects that made this build possible:<br><br>"
               "• <b>Fritchii v6.5 (Frizti & Bakemono)</b>: Graphite UI, Home Screen, In-Game Controls, Bottleneck PerfOverlay, 3rd GPU Render Thread, Shared JIT Cache & Adaptive 4–6 GB VRAM Thrash Guard.<br>"
               "• <b>CheezyEmu (CheezerWheezer)</b>: Idle Buffer Garbage Collector, Fast Shader Loading & Ocarina of Time PGO Profile.<br>"
               "• <b>Lichii (zortos293)</b>: Switch 2 NX2 Core & Expanded 4-Tonemap Neural AA Bypass.<br>"
               "• <b>Triforce</b>: Save Location Thumbnail Extraction & Cross-Emulator Save Detection.<br>"
               "• <b>Eden / Yuzu & Dark</b>: Upstream Emulator Foundations & Dynamic FPS.")
                .arg(QString::fromLatin1(FritchiiUi::Version)));
    });
    links->addWidget(credits);
    layout->addLayout(links);
    layout->addSpacing(28);

    auto* hero = new QHBoxLayout;
    hero->setSpacing(36);
    cover = new QLabel;
    cover->setFixedSize(CoverSize, CoverSize);
    hero->addWidget(cover, 0, Qt::AlignTop);

    auto* side = new QVBoxLayout;
    side->setSpacing(0);
    side->addStretch(1);
    title = new QLabel;
    title->setObjectName(QStringLiteral("FritchiiHeroTitle"));
    title->setWordWrap(true);
    title->setMaximumWidth(680);
    side->addWidget(title);
    side->addSpacing(24);
    auto* play_row = new QHBoxLayout;
    play_row->setSpacing(12);
    play = new QPushButton;
    play->setObjectName(QStringLiteral("FritchiiPlay"));
    play->setCursor(Qt::PointingHandCursor);
    play->setIconSize(QSize(18, 18));
    connect(play, &QPushButton::clicked, this, [this] {
        if (hero_path.isEmpty()) {
            hooks.add_folder();
        } else {
            VideoCommon::ShaderWarm::SetPlayFullCache(false);
            if (hero_id == OcarinaOfTime &&
                Settings::values.precache_shaders.GetValue() != Settings::PrecacheShaders::Off &&
                !VideoCommon::ShaderWarm::IsOotTotalCacheBuilt()) {
                const auto shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
                                        fmt::format("{:016x}", OcarinaOfTime);
                u64 recipe_done = 0;
                {
                    std::ifstream rec_in(shader_dir / "precompiled.fork-v54.done");
                    u64 sz = 0;
                    s64 wt = 0;
                    rec_in >> sz >> wt >> recipe_done;
                }
                if (recipe_done == 0) {
                    const u64 init_gfx = VideoCommon::ShaderWarm::FastEssentialGraphicsCount();
                    const u64 init_cmp = VideoCommon::ShaderWarm::FastEssentialComputeCount();
                    const u64 init_total = init_gfx + init_cmp;
                    const u64 tot_gfx = VideoCommon::ShaderWarm::TotalRealGraphicsCount();
                    const u64 tot_cmp = VideoCommon::ShaderWarm::TotalRealComputeCount();
                    const u64 tot_total = tot_gfx + tot_cmp;
                    const QLocale loc;

                    QDialog dlg(window);
                    dlg.setWindowTitle(tr("Choose Shader Cache — First Launch"));
                    dlg.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
                    dlg.setMinimumWidth(480);
                    auto* vb = new QVBoxLayout(&dlg);
                    vb->setContentsMargins(20, 18, 20, 18);
                    vb->setSpacing(10);
                    auto* hdr = new QLabel(
                        tr("<b>Choose which Shader Cache to build on first launch:</b>"), &dlg);
                    vb->addWidget(hdr);

                    const auto make_choice_btn = [&](const QString& label, const QString& desc,
                                                     const char* border_color, const char* bg_color) {
                        auto* btn = new QPushButton(&dlg);
                        btn->setCursor(Qt::PointingHandCursor);
                        btn->setStyleSheet(QStringLiteral(
                            "QPushButton { background-color: %1; color: #f4f4f0; border: 1px solid %2; "
                            "border-radius: 8px; padding: 12px 14px; text-align: left; font-size: 13px; "
                            "font-weight: 600; }"
                            "QPushButton:hover { border-color: #a78bfa; background-color: #1e1a2e; }")
                                               .arg(QString::fromLatin1(bg_color),
                                                    QString::fromLatin1(border_color)));
                        btn->setText(label + QStringLiteral("\n") + desc);
                        vb->addWidget(btn);
                        return btn;
                    };

                    auto* tot_btn = make_choice_btn(
                        tr("▶  Total Shader Cache (%1 shaders) — Recommended")
                            .arg(loc.toString(static_cast<qulonglong>(tot_total))),
                        tr("Prioritizes the %1 initial shaders first and continues up to %2 in the "
                           "background if you click 'Play now'.")
                            .arg(loc.toString(static_cast<qulonglong>(init_total)),
                                 loc.toString(static_cast<qulonglong>(tot_total))),
                        "#7c3aed", "#181526");
                    auto* init_btn = make_choice_btn(
                        tr("▶  Initial Shader Cache (%1 shaders)")
                            .arg(loc.toString(static_cast<qulonglong>(init_total))),
                        tr("1st dungeon up to Hyrule Castle (%1 graphics + %2 compute). On your "
                           "next launch you can upgrade to Total.")
                            .arg(loc.toString(static_cast<qulonglong>(init_gfx)),
                                 loc.toString(static_cast<qulonglong>(init_cmp))),
                        "#2e2e3a", "#14141b");

                    auto* row_cancel = new QHBoxLayout;
                    row_cancel->addStretch(1);
                    auto* cancel_btn = new QPushButton(tr("Cancel"), &dlg);
                    row_cancel->addWidget(cancel_btn);
                    vb->addLayout(row_cancel);

                    int choice = 0;
                    connect(init_btn, &QPushButton::clicked, &dlg, [&] { choice = 1; dlg.accept(); });
                    connect(tot_btn, &QPushButton::clicked, &dlg, [&] { choice = 2; dlg.accept(); });
                    connect(cancel_btn, &QPushButton::clicked, &dlg, &QDialog::reject);
                    if (dlg.exec() != QDialog::Accepted || choice == 0) {
                        return;
                    }
                    VideoCommon::ShaderWarm::SetPlayFullCache(choice == 2);
                } else {
                    static bool asked_upgrade_this_session = false;
                    if (!asked_upgrade_this_session) {
                        asked_upgrade_this_session = true;
                        const u64 tot_total = VideoCommon::ShaderWarm::TotalRealGraphicsCount() +
                                              VideoCommon::ShaderWarm::TotalRealComputeCount();
                        const QLocale loc;
                        QDialog dlg(window);
                        dlg.setWindowTitle(tr("Complete Shader Cache Available"));
                        dlg.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
                        dlg.setMinimumWidth(460);
                        auto* vb = new QVBoxLayout(&dlg);
                        vb->setContentsMargins(20, 18, 20, 18);
                        vb->setSpacing(10);
                        vb->addWidget(new QLabel(
                            tr("<b>Your Initial Shader Cache is ready.</b><br>"
                               "Would you like to complete the Total Shader Cache (%1 shaders)?")
                                .arg(loc.toString(static_cast<qulonglong>(tot_total))),
                            &dlg));

                        const auto make_btn = [&](const QString& label, const QString& desc,
                                                  const char* border_color, const char* bg_color) {
                            auto* btn = new QPushButton(&dlg);
                            btn->setCursor(Qt::PointingHandCursor);
                            btn->setStyleSheet(QStringLiteral(
                                "QPushButton { background-color: %1; color: #f4f4f0; border: 1px solid %2; "
                                "border-radius: 8px; padding: 12px 14px; text-align: left; font-size: 13px; "
                                "font-weight: 600; }"
                                "QPushButton:hover { border-color: #a78bfa; background-color: #1e1a2e; }")
                                                   .arg(QString::fromLatin1(bg_color),
                                                        QString::fromLatin1(border_color)));
                            btn->setText(label + QStringLiteral("\n") + desc);
                            vb->addWidget(btn);
                            return btn;
                        };

                        auto* upg_btn = make_btn(
                            tr("▶  Upgrade to Total Shader Cache (%1 shaders)")
                                .arg(loc.toString(static_cast<qulonglong>(tot_total))),
                            tr("Click 'Play now' on the loading screen to keep playing while it "
                               "finishes in the background (with Pause/Stop controls)."),
                            "#7c3aed", "#181526");
                        auto* keep_btn = make_btn(
                            tr("▶  Play with Initial Cache"),
                            tr("Enter the game right away without building the extra shaders now."),
                            "#2e2e3a", "#14141b");

                        int choice = 0;
                        connect(upg_btn, &QPushButton::clicked, &dlg, [&] { choice = 2; dlg.accept(); });
                        connect(keep_btn, &QPushButton::clicked, &dlg, [&] { choice = 1; dlg.accept(); });
                        if (dlg.exec() != QDialog::Accepted || choice == 0) {
                            return;
                        }
                        VideoCommon::ShaderWarm::SetPlayFullCache(choice == 2);
                    }
                }
            }
            RefreshSaveSlots();
            hooks.play(hero_path, hero_id);
        }
    });
    play_row->addWidget(play);

    save_slot_button = new QPushButton(page);
    save_slot_button->setCursor(Qt::PointingHandCursor);
    save_slot_button->setFocusPolicy(Qt::NoFocus);
    save_slot_button->setToolTip(
        tr("Active Zelda OoT save slot. Click to choose from all 8 slots with previews."));
    save_slot_button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #181820; color: #e4e4ec; border: 1px solid #2e2e3a; "
        "border-radius: 8px; padding: 0 14px; min-height: 40px; font-size: 13px; }"
        "QPushButton:hover { border-color: #a78bfa; color: #ffffff; }"));
    connect(save_slot_button, &QPushButton::clicked, this, [this] {
        if (hooks.emulation_running && hooks.emulation_running()) {
            return;
        }
        ChooseOotSaveSlotBeforePlay(window, hooks.emulation_running, true, false);
        RefreshSaveSlots();
    });
    play_row->addWidget(save_slot_button);
    play_row->addStretch(1);
    side->addLayout(play_row);
    side->addStretch(1);
    hero->addLayout(side, 1);
    layout->addLayout(hero);

    layout->addSpacing(48);
    row_box = new QWidget;
    row = new QHBoxLayout(row_box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->setAlignment(Qt::AlignLeft);
    layout->addWidget(row_box);
    layout->addStretch(1);
    return page;
}

void Home::Refresh() {
    const std::vector<Game> games = CollectGames(hooks.model, window->windowIcon().pixmap(256));

    // The hero: the game picked last, else Ocarina of Time, else the first game.
    const Game* hero = nullptr;
    for (const Game& game : games) {
        if (!hero_path.isEmpty() && game.path == hero_path) {
            hero = &game;
            break;
        }
    }
    if (!hero) {
        for (const Game& game : games) {
            if (game.program_id == OcarinaOfTime) {
                hero = &game;
                break;
            }
        }
    }
    if (!hero && !games.empty()) {
        hero = &games.front();
    }

    if (hero) {
        hero_path = hero->path;
        hero_id = hero->program_id;
        if (hero->icon.width() > hero->icon.height() * 1.2) {
            cover->setFixedSize(CoverWidthWide, CoverSize);
            cover->setPixmap(RoundedBanner(hero->icon, CoverWidthWide, CoverSize, 18));
        } else {
            cover->setFixedSize(CoverSize, CoverSize);
            cover->setPixmap(RoundedBanner(hero->icon, CoverSize, CoverSize, 18));
        }
        cover->show();
        title->setText(hero->title);
        play->setText(tr("Play"));
        play->setIcon(Glyph(QStringLiteral("play"), 0x1c1433, 18));
    } else {
        hero_path.clear();
        hero_id = 0;
        cover->hide();
        title->setText(tr("Add your games"));
        play->setText(tr("Add Folder"));
        play->setIcon(Glyph(QStringLiteral("plus"), 0x1c1433, 18));
    }
    RefreshSaveSlots();

    // The small row: the other games, then "All games" and "Add folder".
    // deleteLater: a tile's own click can land here.
    while (QLayoutItem* item = row->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const auto tile = [this](const QIcon& icon, const QString& text) {
        auto* button = new QToolButton(row_box);
        button->setObjectName(QStringLiteral("FritchiiTile"));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(icon);
        button->setIconSize(QSize(TileIcon, TileIcon));
        button->setFixedWidth(TileIcon + 40);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        const QFontMetrics metrics(button->font());
        button->setText(metrics.elidedText(text, Qt::ElideRight, TileIcon + 24));
        button->setToolTip(text);
        row->addWidget(button);
        return button;
    };
    int shown = 0;
    for (const Game& game : games) {
        if (game.path == hero_path || shown == MaxTiles) {
            continue;
        }
        ++shown;
        QToolButton* button = tile(QIcon(Rounded(game.icon, TileIcon, 12)), game.title);
        connect(button, &QToolButton::clicked, this,
                [this, path = game.path, id = game.program_id] {
                    hero_path = path;
                    hero_id = id;
                    Refresh();
                });
    }
    if (!games.empty()) {
        QToolButton* all = tile(Icon(QStringLiteral("list"), 40), tr("All games"));
        all->setIconSize(QSize(40, 40));
        all->setMinimumHeight(TileIcon + 44);
        connect(all, &QToolButton::clicked, this, &Home::ShowList);
    }
    QToolButton* add = tile(Icon(QStringLiteral("plus"), 40), tr("Add folder"));
    add->setIconSize(QSize(40, 40));
    add->setMinimumHeight(TileIcon + 44);
    connect(add, &QToolButton::clicked, this, [this] { hooks.add_folder(); });
}

void Home::ShowHome() {
    list_mode = false;
    game_list->hide();
    placeholder->hide();
    rail->show();
    page->show();
    library_button->setChecked(true);
    Refresh();
}

void Home::ShowList() {
    list_mode = true;
    page->hide();
    rail->show();
    library_button->setChecked(false);
    if (hooks.game_list_empty()) {
        placeholder->show();
    } else {
        game_list->show();
    }
}

void Home::Restore() {
    // A restart shows the list for a moment, and nothing may cover a running game.
    if (hooks.single_window() && hooks.emulation_running()) {
        return;
    }
    if (list_mode) {
        rail->show();
        page->hide();
    } else {
        ShowHome();
    }
}

void Home::OnEmulationStarting() {
    if (!hooks.single_window()) {
        return;
    }
    rail->hide();
    page->hide();
}

bool Home::eventFilter(QObject* watched, QEvent* event) {
    // The main window shows its game list (start up, a game stopped, the list filled): show the
    // home page instead, unless the player asked for the full list.
    if (event->type() == QEvent::Show && (watched == game_list || watched == placeholder)) {
        QTimer::singleShot(0, this, &Home::Restore);
    }
    return QObject::eventFilter(watched, event);
}

void Home::RefreshSaveSlots() {
    if (!save_slot_button) {
        return;
    }
    if (hero_id != OcarinaOfTime) {
        save_slot_button->hide();
        return;
    }
    const auto save_slots = ListOotSaveSlots();
    if (save_slots.empty()) {
        save_slot_button->hide();
        return;
    }
    const OotSlotBrief* active = &save_slots.front();
    for (const auto& s : save_slots) {
        if (s.active) {
            active = &s;
            break;
        }
    }
    const QString default_name = tr("Slot %1").arg(active->slot);
    const QString label =
        (active->name == default_name)
            ? tr("Save: Slot %1 (%2)")
                  .arg(active->slot)
                  .arg(active->has_data ? tr("Saved") : tr("Empty"))
            : tr("Save: Slot %1 — %2").arg(active->slot).arg(active->name);
    save_slot_button->setText(label);
    save_slot_button->show();
}

} // namespace FritchiiUi
