// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QBuffer>
#include <QByteArray>
#include <QGraphicsOpacityEffect>
#include <QIODevice>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QStyleOption>
#include <QElapsedTimer>
#include <QTimer>
#include <cmath>
#include <functional>
#include "common/container/unordered_map.h"
#include "yuzu/util/cheezy_theme.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "ui_loading_screen.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "yuzu/loading_screen.h"
#include "yuzu/util/cheezy_backdrop.h"

// Mingw seems to not have QMovie at all. If QMovie is missing then use a single frame instead of an
// showing the full animation
#if !YUZU_QT_MOVIE_MISSING
#include <QMovie>
#endif

constexpr char PROGRESSBAR_STYLE_PREPARE[] = R"(
QProgressBar {}
QProgressBar::chunk {})";

constexpr char PROGRESSBAR_STYLE_BUILD[] = R"(
QProgressBar {
  background-color: black;
  border: 2px solid white;
  border-radius: 4px;
  padding: 2px;
}
QProgressBar::chunk {
  background-color: #ff3c28;
  width: 1px;
})";

constexpr char SKIP_BUTTON_STYLE[] = R"(
QPushButton {
  background-color: rgba(255, 200, 61, 40);
  color: #FFD966;
  border: 2px solid #FFC83D;
  border-radius: 17px;
  padding: 6px 28px;
  font-size: 12pt;
  font-weight: bold;
}
QPushButton:hover {
  background-color: #FFC83D;
  color: #2A1C00;
}
QPushButton:disabled {
  color: #7A705C;
  border-color: #4A3E2C;
  background-color: transparent;
})";

namespace {

const QColor Cheese{0xFF, 0xC8, 0x3D};
const QColor CheeseDeep{0xE8, 0x9E, 0x10};
const QColor Cream{0xFF, 0xF4, 0xDA};

/// The loading bar: a rounded track that fills with cheese (holes drifting along it) and a
/// little wedge riding its edge; while the length is unknown a piece of cheese glides to and fro.
class CheeseProgress : public QWidget {
public:
    explicit CheeseProgress(QWidget* parent) : QWidget{parent} {
        setMinimumHeight(64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_TranslucentBackground);
        // 60 frames a second; the fill glides to each new value instead of jumping.
        timer.setInterval(16);
        timer.setTimerType(Qt::PreciseTimer);
        clock.start();
        connect(&timer, &QTimer::timeout, this, [this] {
            const qreal dt = std::min(0.05, clock.restart() / 1000.0);
            phase += dt;
            const qreal target =
                maximum > minimum
                    ? std::clamp(double(value - minimum) / (maximum - minimum), 0.0, 1.0)
                    : 0.0;
            shown += (target - shown) * (1.0 - std::exp(-dt * 7.0));
            if (on_frame) {
                on_frame(dt); // the screen behind moves too
            } else {
                update();
            }
        });
    }

    /// Called every frame with the time since the last one (the loading screen steps its
    /// backdrop and repaints, this bar included).
    std::function<void(qreal)> on_frame;

    void SetRange(int min_value, int max_value) {
        if (max_value > min_value && maximum <= minimum) {
            shown = 0; // a new length: fill from the start
        }
        minimum = min_value;
        maximum = max_value;
        update();
    }
    void SetValue(int new_value) {
        value = new_value;
        update();
    }

protected:
    void showEvent(QShowEvent*) override {
        timer.start();
    }
    void hideEvent(QHideEvent*) override {
        timer.stop();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        const int width = std::min(this->width() - 80, 560);
        // Clean modern FritCheeLii progress track (Graphite + Violet/Gold gradient)
        const QRectF track{(this->width() - width) / 2.0, height() / 2.0 - 7, double(width), 14};
        const qreal radius = track.height() / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x1e, 0x1e, 0x28));
        painter.drawRoundedRect(track, radius, radius);

        const bool busy = maximum <= minimum;
        const QRectF inner = track.adjusted(2, 2, -2, -2);
        QRectF fill;
        if (busy) {
            const qreal piece = inner.width() * 0.30;
            const qreal swing = (1 - std::cos(phase * 2.2)) / 2;
            fill = {inner.left() + (inner.width() - piece) * swing, inner.top(), piece,
                    inner.height()};
        } else {
            fill = {inner.left(), inner.top(), std::max(inner.height(), inner.width() * shown),
                    inner.height()};
        }
        painter.setBrush(QColor(0x9B, 0x72, 0xF2));
        painter.drawRoundedRect(fill, inner.height() / 2, inner.height() / 2);

        if (!busy) {
            QFont font = this->font();
            font.setBold(true);
            font.setPixelSize(14);
            painter.setFont(font);
            painter.setPen(QColor(0xec, 0xec, 0xf1));
            const int percent =
                int(100.0 * std::clamp(double(value - minimum) / (maximum - minimum), 0.0, 1.0));
            painter.drawText(QRectF(track.right() + 12, track.top() - 4, 70, track.height() + 8),
                             Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("%1%").arg(percent));
        }
    }

private:
    QTimer timer;
    qreal phase = 0;
    qreal shown = 0; ///< The filled share on screen, easing towards the real one
    QElapsedTimer clock;
    int minimum = 0;
    int maximum = 0;
    int value = 0;
};

} // Anonymous namespace

constexpr char PROGRESSBAR_STYLE_COMPLETE[] = R"(
QProgressBar {
  background-color: #0ab9e6;
  border: 2px solid white;
  border-radius: 4px;
  padding: 2px;
}
QProgressBar::chunk {
  background-color: #ff3c28;
})";

LoadingScreen::LoadingScreen(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::LoadingScreen>()),
      previous_stage(VideoCore::LoadCallbackStage::Complete) {
    ui->setupUi(this);
    setMinimumSize(Layout::MinimumSize::Width, Layout::MinimumSize::Height);

    // CheezyEmu's look: the background is painted (paintEvent), the labels sit on it.
    setStyleSheet(QString{});
    ui->fade_parent->setStyleSheet(QStringLiteral("background: transparent;"));
    ui->logo->setStyleSheet(QStringLiteral("background: transparent;"));
    ui->banner->setStyleSheet(QStringLiteral("background: transparent;"));
    ui->stage->setStyleSheet(QStringLiteral(
        "background: transparent; color: #FFF4DA; font-size: 18pt; font-weight: bold;"));
    ui->stage->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    ui->value->setStyleSheet(
        QStringLiteral("background: transparent; color: #E8D9B5; font-size: 12pt;"));
    ui->value->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    // The progress bar's values drive the cheese bar drawn in its place; its frames move the
    // backdrop too.
    scene = std::make_shared<CheezyBackdrop::Scene>();
    cheese_progress = new CheeseProgress(this);
    static_cast<CheeseProgress*>(cheese_progress)->on_frame = [this](qreal dt) {
        scene->Step(dt, QRectF(rect()));
        update();
    };
    ui->verticalLayout->insertWidget(ui->verticalLayout->indexOf(ui->progress_bar) + 1,
                                     cheese_progress);
    ui->progress_bar->setMaximumHeight(0);
    ui->progress_bar->setVisible(false);

    // Create a fade out effect to hide this loading screen widget.
    // When fading opacity, it will fade to the parent widgets background color, which is why we
    // create an internal widget named fade_widget that we use the effect on, while keeping the
    // loading screen widget's background color black. This way we can create a fade to black effect
    opacity_effect = new QGraphicsOpacityEffect(this);
    opacity_effect->setOpacity(1);
    ui->fade_parent->setGraphicsEffect(opacity_effect);
    fadeout_animation = std::make_unique<QPropertyAnimation>(opacity_effect, "opacity");
    fadeout_animation->setDuration(500);
    fadeout_animation->setStartValue(1);
    fadeout_animation->setEndValue(0);
    fadeout_animation->setEasingCurve(QEasingCurve::OutBack);

    // After the fade completes, hide the widget and reset the opacity
    connect(fadeout_animation.get(), &QPropertyAnimation::finished, [this] {
        hide();
        opacity_effect->setOpacity(1);
        emit Hidden();
    });
    connect(this, &LoadingScreen::LoadProgress, this, &LoadingScreen::OnLoadProgress,
            Qt::QueuedConnection);

    // Shown while shaders are precached: starts the game without waiting for the rest.
    skip_button = new QPushButton(tr("Skip"), this);
    skip_button->setStyleSheet(CheezyTheme::Transform(QString::fromUtf8(SKIP_BUTTON_STYLE)));
    skip_button->setToolTip(tr("Start the game now. The shaders not built yet are built the next "
                               "time the game starts, or while you play when the game needs "
                               "them."));
    skip_button->hide();
    ui->verticalLayout->insertWidget(2, skip_button, 0, Qt::AlignHCenter | Qt::AlignTop);
    connect(skip_button, &QPushButton::clicked, this, [this] {
        VideoCommon::ShaderWarm::RequestSkipWarm();
        skip_button->setEnabled(false);
    });
    qRegisterMetaType<VideoCore::LoadCallbackStage>();

    stage_translations = {
        {VideoCore::LoadCallbackStage::Prepare, tr("Getting your game ready...")},
        {VideoCore::LoadCallbackStage::Build, tr("Loading saved shaders %1 / %2")},
        {VideoCore::LoadCallbackStage::Complete, tr("Here we go!")},
        {VideoCore::LoadCallbackStage::Warm,
         tr("Building shaders %1 / %2\n(Just this once — If you close, progress will continue from "
            "where it stopped.)")},
    };
    progressbar_style = {
        {VideoCore::LoadCallbackStage::Prepare, PROGRESSBAR_STYLE_PREPARE},
        {VideoCore::LoadCallbackStage::Build, PROGRESSBAR_STYLE_BUILD},
        {VideoCore::LoadCallbackStage::Complete, PROGRESSBAR_STYLE_COMPLETE},
        {VideoCore::LoadCallbackStage::Warm, PROGRESSBAR_STYLE_BUILD},
    };
}

LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    std::vector<u8> buffer;
    if (loader.ReadBanner(buffer) == Loader::ResultStatus::Success) {
#ifdef YUZU_QT_MOVIE_MISSING
        QPixmap map;
        map.loadFromData(buffer.data(), buffer.size());
        ui->banner->setPixmap(map);
#else
        backing_mem = std::make_unique<QByteArray>(reinterpret_cast<char*>(buffer.data()),
                                                   static_cast<int>(buffer.size()));
        backing_buf = std::make_unique<QBuffer>(backing_mem.get());
        backing_buf->open(QIODevice::ReadOnly);
        animation = std::make_unique<QMovie>(backing_buf.get(), QByteArray());
        animation->start();
        ui->banner->setMovie(animation.get());
#endif
        buffer.clear();
    }
    // The game's icon, drawn as a big tile over the stage text (paintEvent).
    game_icon = {};
    if (std::vector<u8> icon; loader.ReadIcon(icon) == Loader::ResultStatus::Success) {
        game_icon.loadFromData(icon.data(), static_cast<uint>(icon.size()));
    }
    ambient = {}; // made at the screen's size when painted
    if (loader.ReadLogo(buffer) == Loader::ResultStatus::Success) {
        QPixmap map;
        map.loadFromData(buffer.data(), static_cast<uint>(buffer.size()));
        ui->logo->setPixmap(map);
    }

    slow_shader_compile_start = false;
    OnLoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
}

void LoadingScreen::OnLoadComplete() {
    fadeout_animation->start(QPropertyAnimation::KeepWhenStopped);
}

void LoadingScreen::OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value,
                                   std::size_t total) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    // reset the timer if the stage changes
    if (stage != previous_stage) {
        // The cheese bar shows from the first stage on; the old bar stays hidden.
        cheese_progress->setVisible(true);
        // Mandatory Fast Essential on Play: hide Skip during normal Play launch; show Skip/Cancel
        // only when pre-building shaders from the standalone "Shader Cache" button on Home.
        skip_button->setVisible(stage == VideoCore::LoadCallbackStage::Warm &&
                                VideoCommon::ShaderWarm::IsStandaloneBuildOnly());
        skip_button->setEnabled(true);
        previous_stage = stage;
        // reset back to fast shader compiling since the stage changed
        slow_shader_compile_start = false;
        smoothed_eta_ms = -1.0;
        min_eta_sec = -1;
        last_eta_value = 0;
        last_eta_update = {};
        window_eta_time = {};
        window_eta_value = 0;
        recent_ms_per_shader = -1.0;
        cached_estimate.clear();
    }
    // update the max of the progress bar if the number of shaders change
    if (total != previous_total) {
        ui->progress_bar->setMaximum(static_cast<int>(total));
        previous_total = total;
    }
    // Reset the progress bar ranges if compilation is done
    if (stage == VideoCore::LoadCallbackStage::Complete) {
        ui->progress_bar->setRange(0, 0);
    }

    QString estimate;
    // If there's a drastic slowdown in the rate, then display an estimate
    if (now - previous_time > milliseconds{50} || slow_shader_compile_start) {
        if (!slow_shader_compile_start) {
            slow_shader_start = steady_clock::now();
            slow_shader_compile_start = true;
            slow_shader_first_value = value;
            window_eta_time = slow_shader_start;
            window_eta_value = value;
            recent_ms_per_shader = -1.0;
        }
        // Update a 4-second sliding-window rate so heavy stretches (e.g. compute shaders near the
        // end) are detected quickly instead of letting an optimistic cumulative average burn down
        // to 00:01 prematurely.
        const auto win_diff = duration_cast<milliseconds>(now - window_eta_time);
        if (win_diff >= seconds{4} && value > window_eta_value) {
            const double win_rate =
                static_cast<double>(win_diff.count()) / static_cast<double>(value - window_eta_value);
            recent_ms_per_shader = (recent_ms_per_shader < 0.0)
                                       ? win_rate
                                       : (0.60 * recent_ms_per_shader + 0.40 * win_rate);
            window_eta_time = now;
            window_eta_value = value;
        }
        // Calculate a monotonic (non-increasing) ETA after a 5s warmup and 80+ shaders:
        // Pad the initial estimate by 30% for background system load and use the slower of
        // cumulative vs recent rate, only stepping down when the real estimate is lower so the
        // countdown holds steady during slow stretches instead of getting stuck at 00:01.
        const auto diff = duration_cast<milliseconds>(now - slow_shader_start);
        if (diff >= seconds{5} && value >= slow_shader_first_value + 80 && total > value) {
            if (min_eta_sec < 0 || (now - last_eta_update >= milliseconds{1000})) {
                const double cum_ms_per_shader =
                    static_cast<double>(diff.count()) /
                    static_cast<double>(value - slow_shader_first_value);
                const double effective_ms_per_shader =
                    (recent_ms_per_shader > 0.0)
                        ? std::max(cum_ms_per_shader, recent_ms_per_shader)
                        : cum_ms_per_shader;
                const double remaining_ms =
                    static_cast<double>(total - value) * effective_ms_per_shader;
                smoothed_eta_ms = (smoothed_eta_ms < 0.0)
                                      ? (remaining_ms * 1.30)
                                      : (0.70 * smoothed_eta_ms + 0.30 * remaining_ms);
                const long long candidate_sec =
                    std::max<long long>(1, static_cast<long long>(smoothed_eta_ms / 1000.0 + 0.5));
                if (min_eta_sec < 0) {
                    min_eta_sec = candidate_sec;
                } else if (candidate_sec < min_eta_sec) {
                    const long long delta = min_eta_sec - candidate_sec;
                    const long long step = std::max<long long>(1, delta / 2);
                    min_eta_sec = std::max<long long>(candidate_sec, min_eta_sec - step);
                }
                last_eta_value = value;
                if (min_eta_sec <= 1) {
                    cached_estimate = tr("Finishing...");
                } else {
                    const long long total_sec = min_eta_sec;
                    const long long hrs = total_sec / 3600;
                    const long long mins = (total_sec % 3600) / 60;
                    const long long secs = total_sec % 60;
                    const QString formatted =
                        hrs > 0 ? QStringLiteral("%1:%2:%3")
                                      .arg(hrs)
                                      .arg(mins, 2, 10, QLatin1Char('0'))
                                      .arg(secs, 2, 10, QLatin1Char('0'))
                                : QStringLiteral("%1:%2")
                                      .arg(mins, 2, 10, QLatin1Char('0'))
                                      .arg(secs, 2, 10, QLatin1Char('0'));
                    cached_estimate = tr("Estimated Time %1").arg(formatted);
                }
                last_eta_update = now;
            }
            estimate = cached_estimate;
        }
    }

    // update labels and progress bar
    if (stage == VideoCore::LoadCallbackStage::Warm && total == 0) {
        // Before the count is known: the game's files are read for their shader programs.
        ui->stage->setText(tr("Gathering this game's shaders (just once, one moment)..."));
        ui->progress_bar->setRange(0, 0);
    } else if (stage == VideoCore::LoadCallbackStage::Build ||
               stage == VideoCore::LoadCallbackStage::Warm) {
        ui->stage->setText(stage_translations[stage].arg(value).arg(total));
    } else {
        ui->stage->setText(stage_translations[stage]);
    }
    if (stage == VideoCore::LoadCallbackStage::Warm ||
        stage == VideoCore::LoadCallbackStage::Build) {
        const QString tip =
            tr("Tip: Shaders, saves & settings are saved in the 'user' folder — keep or copy "
               "'user' when updating FritCheeLii so you never have to rebuild.");
        estimate = estimate.isEmpty() ? tip : estimate + QStringLiteral("\n\n") + tip;
    }
    ui->value->setText(estimate);
    ui->progress_bar->setValue(static_cast<int>(value));
    auto* const bar = static_cast<CheeseProgress*>(cheese_progress);
    if (stage == VideoCore::LoadCallbackStage::Prepare) {
        bar->SetRange(0, 0); // no length known yet: the gliding piece
    } else {
        bar->SetRange(ui->progress_bar->minimum(), ui->progress_bar->maximum());
    }
    bar->SetValue(static_cast<int>(value));
    previous_time = now;

    // TEST: CHEEZYEMU_LOADING_SHOT=<png> saves a picture of the loading screen 3 s in.
    if (static bool shot_taken = false; !shot_taken) {
        if (const QByteArray shot = qgetenv("CHEEZYEMU_LOADING_SHOT"); !shot.isEmpty()) {
            shot_taken = true;
            QTimer::singleShot(3000, this, [this, path = QString::fromUtf8(shot)] {
                window()->grab().save(path);
            });
        }
    }
}

void LoadingScreen::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect area = rect();
    // The home view's moving backdrop, with the game's icon as the ambient glow.
    if (ambient.size() != area.size() && !game_icon.isNull()) {
        ambient = CheezyBackdrop::MakeAmbient(game_icon, area.size());
    }
    scene->Paint(p, area, ambient, 0.45);

    // The game's icon as a glowing rounded tile above the stage text.
    if (!game_icon.isNull()) {
        const int tile = std::clamp(area.height() / 4, 96, 220);
        const QPoint stage_top = ui->stage->mapTo(this, QPoint(0, 0));
        const QRectF tile_rect{(area.width() - tile) / 2.0,
                               std::max(24.0, stage_top.y() - tile - 28.0), double(tile),
                               double(tile)};
        const qreal radius = tile * 0.12;
        for (int i = 6; i >= 1; --i) {
            QColor glow{0xFF, 0xC8, 0x3D};
            glow.setAlpha(12 + (6 - i) * 6);
            p.setPen(QPen(glow, 2));
            p.setBrush(Qt::NoBrush);
            const qreal spread = 3.0 + i * 2.5;
            p.drawRoundedRect(tile_rect.adjusted(-spread, -spread, spread, spread),
                              radius + spread, radius + spread);
        }
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(tile_rect, radius, radius);
        p.setClipPath(clip);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(tile_rect.toRect(), game_icon);
        p.restore();
        p.setPen(QPen(CheezyTheme::Recolor(QColor(0xFF, 0xC8, 0x3D)), 4));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(tile_rect, radius, radius);
    }
    QWidget::paintEvent(event);
}

void LoadingScreen::Clear() {
#ifndef YUZU_QT_MOVIE_MISSING
    animation.reset();
    backing_buf.reset();
    backing_mem.reset();
#endif
}
