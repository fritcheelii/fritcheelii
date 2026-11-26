// SPDX-FileCopyrightText: Copyright 2026 CheezyEmu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStatusBar>
#include <QStringList>
#include <QWindow>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "main_window.h"
#include "performance_overlay.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr int FullHeight = 40;
constexpr int CompactHeight = 34;
constexpr int HeaderHeight = 14;
constexpr int Edge = 5;    // the gold edge on the left
constexpr int Pad = 12;    // around the cells
constexpr int Gap = 14;    // between cells, with a hairline in the middle
constexpr int Margin = 6;  // from the game picture's border, in a corner
constexpr int Snap = 18;   // how near a border the dragged bar jumps to it
constexpr int GraphWidth = 112;

const QColor Cheese{0xff, 0xd2, 0x3f};
const QColor Text{0xf4, 0xf4, 0xf0};
const QColor Dim{0x93, 0x93, 0x8c};
const QColor Accent{0xa7, 0x8b, 0xfa};
const QColor Slow{0xff, 0x9f, 0x1c};
const QColor Bad{0xff, 0x4d, 0x4d};

#ifdef _WIN32
double QuerySystemCpuPercent() {
    static ULARGE_INTEGER prev_idle{}, prev_kernel{}, prev_user{};
    static bool initialized = false;
    FILETIME f_idle{}, f_kernel{}, f_user{};
    if (!GetSystemTimes(&f_idle, &f_kernel, &f_user)) {
        return -1.0;
    }
    ULARGE_INTEGER cur_idle{{f_idle.dwLowDateTime, f_idle.dwHighDateTime}};
    ULARGE_INTEGER cur_kernel{{f_kernel.dwLowDateTime, f_kernel.dwHighDateTime}};
    ULARGE_INTEGER cur_user{{f_user.dwLowDateTime, f_user.dwHighDateTime}};
    if (!initialized) {
        prev_idle = cur_idle;
        prev_kernel = cur_kernel;
        prev_user = cur_user;
        initialized = true;
        return -1.0;
    }
    const u64 idle_d = cur_idle.QuadPart - prev_idle.QuadPart;
    const u64 total_d = (cur_kernel.QuadPart - prev_kernel.QuadPart) +
                        (cur_user.QuadPart - prev_user.QuadPart);
    prev_idle = cur_idle;
    prev_kernel = cur_kernel;
    prev_user = cur_user;
    if (total_d == 0) {
        return -1.0;
    }
    return std::clamp(100.0 * static_cast<double>(total_d - idle_d) / static_cast<double>(total_d),
                      0.0, 100.0);
}

double QueryCpuClockGhz() {
    struct ProcPowerInfo {
        ULONG Number;
        ULONG MaxMhz;
        ULONG CurrentMhz;
        ULONG MhzLimit;
        ULONG MaxIdleState;
        ULONG CurrentIdleState;
    };
    using CallNtPowerInfoFn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
    static CallNtPowerInfoFn call_nt_power = []() -> CallNtPowerInfoFn {
        HMODULE mod = GetModuleHandleW(L"powrprof.dll");
        if (!mod) {
            mod = LoadLibraryW(L"powrprof.dll");
        }
        return mod ? reinterpret_cast<CallNtPowerInfoFn>(
                         GetProcAddress(mod, "CallNtPowerInformation"))
                   : nullptr;
    }();
    if (!call_nt_power) {
        return 0.0;
    }
    SYSTEM_INFO sys_info{};
    GetSystemInfo(&sys_info);
    const DWORD count = std::clamp<DWORD>(sys_info.dwNumberOfProcessors, 1, 256);
    std::vector<ProcPowerInfo> info(count);
    if (call_nt_power(11 /* ProcessorInformation */, nullptr, 0, info.data(),
                      static_cast<ULONG>(info.size() * sizeof(ProcPowerInfo))) != 0) {
        return 0.0;
    }
    ULONG base_mhz = 0;
    ULONG current_mhz = 0;
    for (const auto& entry : info) {
        if (entry.MaxMhz > base_mhz) {
            base_mhz = entry.MaxMhz;
        }
        const ULONG mhz = entry.CurrentMhz > 0 ? entry.CurrentMhz : entry.MaxMhz;
        if (mhz > current_mhz) {
            current_mhz = mhz;
        }
    }
    if (base_mhz > 0) {
        // Query live CPU frequency via Windows PDH ("% Processor Performance" * BaseMHz / 100),
        // matching Task Manager's real-time Turbo Boost clock readout without needing a driver.
        using PdhHandle = HANDLE;
        struct PdhFmtCounterValue {
            LONG CStatus;
            double doubleValue;
        };
        using PdhOpenQueryWFn = LONG(WINAPI*)(LPCWSTR, ULONG_PTR, PdhHandle*);
        using PdhAddEnglishCounterWFn = LONG(WINAPI*)(PdhHandle, LPCWSTR, ULONG_PTR, PdhHandle*);
        using PdhCollectQueryDataFn = LONG(WINAPI*)(PdhHandle);
        using PdhGetFormattedCounterValueFn =
            LONG(WINAPI*)(PdhHandle, DWORD, LPDWORD, PdhFmtCounterValue*);

        struct PdhState {
            bool tried = false;
            bool ready = false;
            bool has_prev = false;
            PdhHandle query = nullptr;
            PdhHandle counter = nullptr;
            PdhCollectQueryDataFn collect = nullptr;
            PdhGetFormattedCounterValueFn get_val = nullptr;
        };
        static PdhState pdh;
        if (!pdh.tried) {
            pdh.tried = true;
            if (HMODULE mod = LoadLibraryW(L"pdh.dll")) {
                auto open_q = reinterpret_cast<PdhOpenQueryWFn>(
                    GetProcAddress(mod, "PdhOpenQueryW"));
                auto add_c = reinterpret_cast<PdhAddEnglishCounterWFn>(
                    GetProcAddress(mod, "PdhAddEnglishCounterW"));
                pdh.collect = reinterpret_cast<PdhCollectQueryDataFn>(
                    GetProcAddress(mod, "PdhCollectQueryData"));
                pdh.get_val = reinterpret_cast<PdhGetFormattedCounterValueFn>(
                    GetProcAddress(mod, "PdhGetFormattedCounterValue"));
                if (open_q && add_c && pdh.collect && pdh.get_val &&
                    open_q(nullptr, 0, &pdh.query) == 0 &&
                    add_c(pdh.query, L"\\Processor Information(_Total)\\% Processor Performance", 0,
                          &pdh.counter) == 0) {
                    pdh.collect(pdh.query);
                    pdh.has_prev = true;
                    pdh.ready = true;
                }
            }
        }
        if (pdh.ready && pdh.collect(pdh.query) == 0 && pdh.has_prev) {
            PdhFmtCounterValue val{};
            constexpr DWORD pdh_fmt_double = 0x00000200;
            if (pdh.get_val(pdh.counter, pdh_fmt_double, nullptr, &val) == 0 && val.CStatus == 0 &&
                val.doubleValue > 1.0) {
                return (static_cast<double>(base_mhz) / 1000.0) * (val.doubleValue / 100.0);
            }
        }
    }
    return current_mhz > 0 ? static_cast<double>(current_mhz) / 1000.0 : 0.0;
}

int QueryMaxCpuCorePercent() {
    struct ProcPerfInfo {
        LARGE_INTEGER IdleTime;
        LARGE_INTEGER KernelTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER DpcTime;
        LARGE_INTEGER InterruptTime;
        ULONG InterruptCount;
    };
    using NtQuerySysInfoFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    static NtQuerySysInfoFn nt_query = []() -> NtQuerySysInfoFn {
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            return reinterpret_cast<NtQuerySysInfoFn>(
                GetProcAddress(ntdll, "NtQuerySystemInformation"));
        }
        return nullptr;
    }();
    if (!nt_query) {
        return -1;
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const ULONG ncpu = std::clamp<ULONG>(si.dwNumberOfProcessors, 1, 256);
    std::vector<ProcPerfInfo> cur(ncpu);
    ULONG ret_len = 0;
    if (nt_query(8 /* SystemProcessorPerformanceInformation */, cur.data(),
                 static_cast<ULONG>(cur.size() * sizeof(ProcPerfInfo)), &ret_len) != 0) {
        return -1;
    }
    const std::size_t count =
        std::min<std::size_t>(cur.size(), ret_len / sizeof(ProcPerfInfo));
    static std::vector<ProcPerfInfo> prev;
    if (prev.size() != count) {
        prev.assign(cur.begin(), cur.begin() + count);
        return -1;
    }
    double max_pct = -1.0;
    for (std::size_t i = 0; i < count; ++i) {
        const s64 idle_d = cur[i].IdleTime.QuadPart - prev[i].IdleTime.QuadPart;
        const s64 kernel_d = cur[i].KernelTime.QuadPart - prev[i].KernelTime.QuadPart;
        const s64 user_d = cur[i].UserTime.QuadPart - prev[i].UserTime.QuadPart;
        const s64 total_d = kernel_d + user_d;
        if (total_d > 0) {
            const double pct = std::clamp(
                100.0 * static_cast<double>(total_d - idle_d) / static_cast<double>(total_d),
                0.0, 100.0);
            max_pct = std::max(max_pct, pct);
        }
    }
    prev.assign(cur.begin(), cur.begin() + count);
    return max_pct >= 0.0 ? static_cast<int>(std::lround(max_pct)) : -1;
}

struct NvmlGpuMetrics {
    int util = -1;
    int temp_c = -1;
    int power_w = -1;
    QString name;
};

NvmlGpuMetrics QueryNvmlGpuMetrics(bool want_name = false) {
    struct NvmlUtil {
        unsigned int gpu;
        unsigned int memory;
    };
    using InitFn = int (*)();
    using GetDevFn = int (*)(unsigned int, void**);
    using GetUtilFn = int (*)(void*, NvmlUtil*);
    using GetTempFn = int (*)(void*, unsigned int, unsigned int*);
    using GetPowerFn = int (*)(void*, unsigned int*);
    using GetNameFn = int (*)(void*, char*, unsigned int);
    static bool tried = false;
    static void* dev = nullptr;
    static GetUtilFn get_util = nullptr;
    static GetTempFn get_temp = nullptr;
    static GetPowerFn get_power = nullptr;
    static GetNameFn get_name = nullptr;
    if (!tried) {
        tried = true;
        HMODULE mod = LoadLibraryW(L"nvml.dll");
        if (mod) {
            auto init_fn = reinterpret_cast<InitFn>(GetProcAddress(mod, "nvmlInit_v2"));
            if (!init_fn) {
                init_fn = reinterpret_cast<InitFn>(GetProcAddress(mod, "nvmlInit"));
            }
            auto get_dev =
                reinterpret_cast<GetDevFn>(GetProcAddress(mod, "nvmlDeviceGetHandleByIndex_v2"));
            if (!get_dev) {
                get_dev =
                    reinterpret_cast<GetDevFn>(GetProcAddress(mod, "nvmlDeviceGetHandleByIndex"));
            }
            get_util =
                reinterpret_cast<GetUtilFn>(GetProcAddress(mod, "nvmlDeviceGetUtilizationRates"));
            get_temp =
                reinterpret_cast<GetTempFn>(GetProcAddress(mod, "nvmlDeviceGetTemperature"));
            get_power =
                reinterpret_cast<GetPowerFn>(GetProcAddress(mod, "nvmlDeviceGetPowerUsage"));
            get_name = reinterpret_cast<GetNameFn>(GetProcAddress(mod, "nvmlDeviceGetName"));
            if (init_fn && get_dev && get_util && init_fn() == 0) {
                if (get_dev(0, &dev) != 0) {
                    dev = nullptr;
                }
            }
        }
    }
    NvmlGpuMetrics out;
    if (!dev) {
        return out;
    }
    if (want_name && get_name) {
        char buf[128]{};
        if (get_name(dev, buf, sizeof(buf)) == 0) {
            out.name = QString::fromUtf8(buf).simplified();
        }
    }
    if (get_util) {
        NvmlUtil util{};
        if (get_util(dev, &util) == 0) {
            out.util = static_cast<int>(std::clamp(util.gpu, 0U, 100U));
        }
    }
    if (get_temp) {
        unsigned int temp = 0;
        if (get_temp(dev, 0 /* NVML_TEMPERATURE_GPU */, &temp) == 0 && temp > 0 && temp < 150) {
            out.temp_c = static_cast<int>(temp);
        }
    }
    if (get_power) {
        unsigned int mw = 0;
        if (get_power(dev, &mw) == 0) {
            out.power_w = static_cast<int>(std::lround(static_cast<double>(mw) / 1000.0));
        }
    }
    return out;
}

QString QueryCpuBrandString() {
    wchar_t buf[256]{};
    DWORD bytes = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf,
                     &bytes) != ERROR_SUCCESS) {
        return {};
    }
    QString name = QString::fromWCharArray(buf).simplified();
    name.remove(QStringLiteral("(R)"), Qt::CaseInsensitive);
    name.remove(QStringLiteral("(TM)"), Qt::CaseInsensitive);
    name.remove(QStringLiteral(" CPU"), Qt::CaseInsensitive);
    const int at_idx = name.indexOf(QStringLiteral(" @"));
    if (at_idx > 0) {
        name.truncate(at_idx);
    }
    return name.simplified();
}

QString QueryGpuDisplayName() {
    if (const QString nvml_name = QueryNvmlGpuMetrics(true).name; !nvml_name.isEmpty()) {
        return nvml_name;
    }
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        const QString name = QString::fromWCharArray(dd.DeviceString).simplified();
        if (name.isEmpty()) {
            continue;
        }
        if (name.contains(QStringLiteral("Parsec"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("Microsoft Basic"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("Virtual"), Qt::CaseInsensitive) ||
            name.contains(QStringLiteral("RDP"), Qt::CaseInsensitive)) {
            continue;
        }
        return name;
    }
    return {};
}

QString QueryRamSummary() {
    const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size < 16 || size > 1024 * 1024) {
        return {};
    }
    std::vector<u8> raw(size);
    if (GetSystemFirmwareTable('RSMB', 0, raw.data(), size) != size) {
        return {};
    }
    u32 table_len = 0;
    std::memcpy(&table_len, raw.data() + 4, sizeof(table_len));
    const std::size_t end = std::min<std::size_t>(raw.size(), 8 + static_cast<std::size_t>(table_len));
    std::size_t pos = 8;

    u64 total_mb = 0;
    int sticks = 0;
    u8 mem_type = 0;
    u16 speed_mhz = 0;

    while (pos + 4 <= end) {
        const u8 type = raw[pos];
        const u8 len = raw[pos + 1];
        if (len < 4 || pos + len > end) {
            break;
        }
        if (type == 17 && len >= 0x17) {
            u16 sz = 0;
            std::memcpy(&sz, raw.data() + pos + 0x0C, sizeof(sz));
            u64 stick_mb = 0;
            if (sz == 0x7FFF && len >= 0x20) {
                u32 ext = 0;
                std::memcpy(&ext, raw.data() + pos + 0x1C, sizeof(ext));
                stick_mb = ext;
            } else if (sz != 0 && sz != 0xFFFF) {
                stick_mb = (sz & 0x8000) ? ((sz & 0x7FFF) / 1024ULL) : sz;
            }
            if (stick_mb > 0) {
                total_mb += stick_mb;
                ++sticks;
                if (mem_type == 0) {
                    mem_type = raw[pos + 0x12];
                }
                u16 spd = 0;
                if (len >= 0x22) {
                    std::memcpy(&spd, raw.data() + pos + 0x20, sizeof(spd));
                }
                if (spd == 0 || spd >= 20000) {
                    std::memcpy(&spd, raw.data() + pos + 0x15, sizeof(spd));
                }
                if (spd > 0 && spd < 20000 && speed_mhz == 0) {
                    speed_mhz = spd;
                }
            }
        }
        pos += len;
        while (pos + 1 < end && !(raw[pos] == 0 && raw[pos + 1] == 0)) {
            ++pos;
        }
        pos += 2;
    }

    if (sticks == 0 || total_mb == 0) {
        return {};
    }
    QString type_str = QStringLiteral("RAM");
    switch (mem_type) {
    case 0x13:
        type_str = QStringLiteral("DDR2");
        break;
    case 0x18:
        type_str = QStringLiteral("DDR3");
        break;
    case 0x1A:
        type_str = QStringLiteral("DDR4");
        break;
    case 0x1E:
        type_str = QStringLiteral("LPDDR4");
        break;
    case 0x22:
        type_str = QStringLiteral("DDR5");
        break;
    case 0x23:
        type_str = QStringLiteral("LPDDR5");
        break;
    default:
        break;
    }
    const QString ch_str = sticks >= 4 ? QStringLiteral("Quad Channel")
                         : sticks == 3 ? QStringLiteral("Triple Channel")
                         : sticks == 2 ? QStringLiteral("Dual Channel")
                                       : QStringLiteral("Single Channel");
    if (speed_mhz > 0) {
        return QStringLiteral("%1 %2 MHz (%3)").arg(type_str).arg(speed_mhz).arg(ch_str);
    }
    return QStringLiteral("%1 (%2)").arg(type_str, ch_str);
}

QString BuildHardwareBannerText() {
    QStringList parts;
    if (const QString cpu = QueryCpuBrandString(); !cpu.isEmpty()) {
        parts << cpu;
    }
    if (const QString gpu = QueryGpuDisplayName(); !gpu.isEmpty()) {
        parts << gpu;
    }
    if (const QString ram = QueryRamSummary(); !ram.isEmpty()) {
        parts << ram;
    }
    return parts.join(QStringLiteral("  |  "));
}
#endif

QString Gigabytes(u64 bytes) {
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1);
}
} // Anonymous namespace

PerformanceOverlay::PerformanceOverlay(MainWindow* parent)
    : QWidget(parent), m_main_window{parent} {
    // CHEEZYEMU_OVERLAY_MODE=opaque: a window without transparency, to measure what the
    // transparent one costs the game's presentation.
    m_opaque = qEnvironmentVariable("CHEEZYEMU_OVERLAY_MODE") == QStringLiteral("opaque");
    if (!m_opaque) {
        setAttribute(Qt::WA_TranslucentBackground);
    }
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Drag to move. Right-click for position, parts and graphics options."));

    m_big = font();
    m_big.setPixelSize(27);
    m_big.setWeight(QFont::Black);
    m_value = font();
    m_value.setPixelSize(13);
    m_value.setWeight(QFont::DemiBold);
    m_label = font();
    m_label.setPixelSize(10);
    m_label.setWeight(QFont::Bold);
    m_label.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);

#ifdef _WIN32
    m_hw_banner_text = BuildHardwareBannerText();
#endif

    Relayout();
    Place();

    // The bar follows the game picture: the window moving, resizing or going fullscreen.
    m_main_window->installEventFilter(this);
    if (QWidget* const surface = m_main_window->GameSurface()) {
        surface->installEventFilter(this);
    }
    connect(m_main_window, &MainWindow::positionChanged, this, [this] { Place(); });
    connect(m_main_window, &MainWindow::statsUpdated, this, &PerformanceOverlay::UpdateStats);
    raise();
}

PerformanceOverlay::~PerformanceOverlay() = default;

bool& PerformanceOverlay::ShowDetailsFlag() {
    static bool show = true;
    return show;
}

bool& PerformanceOverlay::ShowHwBannerFlag() {
    static bool show = true;
    return show;
}

void PerformanceOverlay::RefreshFromSettings() {
    Relayout();
    Place();
    update();
}

QRect PerformanceOverlay::Area() const {
    const QWidget* const surface = m_main_window->GameSurface();
    if (surface == nullptr) {
        return m_main_window->geometry();
    }
    return QRect{surface->mapToGlobal(QPoint{0, 0}), surface->size()};
}

void PerformanceOverlay::Relayout() {
    const auto& ui = UISettings::values;
    const bool compact = ui.perf_overlay_compact.GetValue();
    const QFontMetrics big{m_big};
    const QFontMetrics value{m_value};
    const QFontMetrics label{m_label};

    m_cells.clear();
    int x = Edge + Pad;
    const auto add = [this, &x](Cell::Kind kind, int width) {
        if (!m_cells.empty()) {
            x += Gap;
        }
        m_cells.push_back({kind, x, width});
        x += width;
    };
    // Wide enough for three digits, so the bar doesn't change width as the rate moves.
    const int digits = std::max(big.horizontalAdvance(QStringLiteral("000")),
                                big.horizontalAdvance(m_fps_text));
    const int beside = std::max(label.horizontalAdvance(tr("FPS")),
                                label.horizontalAdvance(m_fps_detail));
    add(Cell::Kind::Fps, digits + 7 + beside);
    add(Cell::Kind::Frame, std::max({value.horizontalAdvance(QStringLiteral("000.0 ms")),
                                     value.horizontalAdvance(m_frame_text),
                                     label.horizontalAdvance(m_low_text)}));
    if (!compact) {
        if (ui.perf_overlay_graph.GetValue()) {
            add(Cell::Kind::Graph, GraphWidth);
        }
        {
            const int lbl_w = std::max(label.horizontalAdvance(tr("CPU")),
                                       label.horizontalAdvance(tr("GPU")));
            const int r1 = lbl_w + 6 + value.horizontalAdvance(m_cpu_text) + 8 +
                           label.horizontalAdvance(m_limit_text);
            const int r2 = lbl_w + 6 + value.horizontalAdvance(m_gpu_ms_text) + 8 +
                           label.horizontalAdvance(m_gpu_busy_text);
            const int min_w = lbl_w + 6 + value.horizontalAdvance(QStringLiteral("100.0 ms")) + 8 +
                              label.horizontalAdvance(tr("Limited by VRAM"));
            add(Cell::Kind::CpuGpu, std::max({r1, r2, min_w}));
        }
        if (!m_upscale_text.isEmpty() || !m_frame_gen_text.isEmpty()) {
            add(Cell::Kind::Status, std::max(value.horizontalAdvance(m_upscale_text),
                                             value.horizontalAdvance(m_frame_gen_text)));
        }
        if (ui.perf_overlay_memory.GetValue() && !m_ram_text.isEmpty()) {
            const int names = std::max(label.horizontalAdvance(tr("RAM")),
                                       label.horizontalAdvance(tr("VRAM")));
            add(Cell::Kind::Memory, names + 8 +
                                        std::max(value.horizontalAdvance(m_ram_text),
                                                 value.horizontalAdvance(m_vram_text)));
        }
        if (ShowDetailsFlag() && (!m_cpu_core_text.isEmpty() || !m_gpu_detail_text.isEmpty())) {
            const int names = std::max(label.horizontalAdvance(tr("CPU")),
                                       label.horizontalAdvance(tr("GPU")));
            add(Cell::Kind::Details, names + 8 +
                                         std::max(value.horizontalAdvance(m_cpu_core_text),
                                                  value.horizontalAdvance(m_gpu_detail_text)));
        }
    }
    const bool has_header = !compact && ShowHwBannerFlag() && !m_hw_banner_text.isEmpty();
    const int min_banner_w =
        has_header ? Edge + Pad + label.horizontalAdvance(m_hw_banner_text) + Pad : 0;
    const QSize wanted{std::max(x + Pad, min_banner_w),
                       compact ? CompactHeight : (FullHeight + (has_header ? HeaderHeight : 0))};
    if (wanted != size()) {
        setFixedSize(wanted);
    }
}

void PerformanceOverlay::Place() {
    if (m_dragging) {
        return;
    }
    const auto& ui = UISettings::values;
    const QRect area = Area();
    const int free_x = std::max(0, area.width() - width());
    const int free_y = std::max(0, area.height() - height());
    const int margin_x = std::min(Margin, free_x / 2);
    const int margin_y = std::min(Margin, free_y / 2);
    int x = margin_x;
    int y = margin_y;
    switch (static_cast<Anchor>(ui.perf_overlay_anchor.GetValue())) {
    case TopLeft:
        break;
    case TopCentre:
        x = free_x / 2;
        break;
    case TopRight:
        x = free_x - margin_x;
        break;
    case BottomLeft:
        y = free_y - margin_y;
        break;
    case BottomCentre:
        x = free_x / 2;
        y = free_y - margin_y;
        break;
    case BottomRight:
        x = free_x - margin_x;
        y = free_y - margin_y;
        break;
    case Custom:
    default:
        // Kept as a share of the free room, so it holds its spot at any window size.
        x = free_x * std::clamp(ui.perf_overlay_x.GetValue(), 0, 1000) / 1000;
        y = free_y * std::clamp(ui.perf_overlay_y.GetValue(), 0, 1000) / 1000;
        break;
    }
    move(area.left() + x, area.top() + y);
}

void PerformanceOverlay::SavePlace() {
    auto& ui = UISettings::values;
    const QRect area = Area();
    const int free_x = std::max(1, area.width() - width());
    const int free_y = std::max(1, area.height() - height());
    const int x = std::clamp(pos().x() - area.left(), 0, free_x);
    const int y = std::clamp(pos().y() - area.top(), 0, free_y);
    const int margin_x = std::min(Margin, free_x / 2);
    const int margin_y = std::min(Margin, free_y / 2);
    const bool left = x == margin_x;
    const bool right = x == free_x - margin_x;
    const bool centre = std::abs(x - free_x / 2) <= 1;
    const bool top = y == margin_y;
    const bool bottom = y == free_y - margin_y;
    Anchor anchor = Custom;
    if (top || bottom) {
        if (left) {
            anchor = top ? TopLeft : BottomLeft;
        } else if (right) {
            anchor = top ? TopRight : BottomRight;
        } else if (centre) {
            anchor = top ? TopCentre : BottomCentre;
        }
    }
    ui.perf_overlay_anchor.SetValue(static_cast<int>(anchor));
    ui.perf_overlay_x.SetValue(x * 1000 / free_x);
    ui.perf_overlay_y.SetValue(y * 1000 / free_y);
}

void PerformanceOverlay::UpdateStats(const Core::PerfStatsResults& results,
                                     const VideoCore::ShaderNotify&) {
    const double fps = results.average_game_fps;
    if (!std::isnan(fps)) {
        // What reaches the screen with frame generation, as the status bar shows it.
        const double shown = m_main_window->DisplayedFps(fps);
        m_fps_text = QString::number(std::lround(shown));
        m_fps_detail = shown > fps + 0.5 ? tr("%1 + FG").arg(std::lround(fps)) : QString{};
        // Under 3 fps is a load or a freeze, not a frame rate to chart.
        if (fps > 3.0) {
            m_history.push_back(fps);
            if (m_history.size() > HistorySize) {
                m_history.pop_front();
            }
        }
        if (m_history.size() >= 4) {
            const double low = *std::min_element(m_history.begin(), m_history.end());
            m_low_text = tr("LOW %1").arg(std::lround(low));
        }
    }
    if (!std::isnan(results.frametime)) {
        m_frame_text = tr("%1 ms").arg(results.frametime * 1000.0, 0, 'f', 1);
    }
    m_upscale_text = m_main_window->UpscaleStatusText();
    m_frame_gen_text = m_main_window->FrameGenStatusText();

    m_ram_text.clear();
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
        const u64 used_ram =
            ms.ullTotalPhys > ms.ullAvailPhys ? (ms.ullTotalPhys - ms.ullAvailPhys) : 0ULL;
        m_ram_text = tr("%1 / %2 GB").arg(Gigabytes(used_ram), Gigabytes(ms.ullTotalPhys));
    }
#endif
    if (m_ram_text.isEmpty()) {
        const auto memory = Common::GetProcessMemoryUsage();
        const u64 ram = memory.working_set != 0 ? memory.working_set : memory.commit;
        m_ram_text = ram != 0 ? tr("%1 GB").arg(Gigabytes(ram)) : QString{};
    }
    m_vram_text.clear();
    m_vram_share = 0.0;
    if (QtCommon::system != nullptr && QtCommon::system->IsPoweredOn()) {
        const auto video = QtCommon::system->GPU().Renderer().GetDeviceMemoryStatus();
        if (video.used != 0 && video.budget != 0) {
            m_vram_text = tr("%1 / %2 GB").arg(Gigabytes(video.used), Gigabytes(video.budget));
            m_vram_share = std::clamp(static_cast<double>(video.used) /
                                          static_cast<double>(video.budget),
                                      0.0, 1.0);
        } else if (video.used != 0) {
            m_vram_text = tr("%1 GB").arg(Gigabytes(video.used));
        }
    }

#ifdef _WIN32
    const double cpu_pct = QuerySystemCpuPercent();
    if (cpu_pct >= 0.0) {
        m_cpu_text = tr("%1%").arg(std::lround(cpu_pct));
    }
    m_cpu_max_core = QueryMaxCpuCorePercent();
    if (m_cpu_max_core >= 0) {
        const double cpu_ghz = QueryCpuClockGhz();
        m_cpu_core_text = cpu_ghz > 0.1
                              ? tr("Single Core %1% • %2 GHz").arg(m_cpu_max_core).arg(cpu_ghz, 0, 'f', 2)
                              : tr("Single Core %1%").arg(m_cpu_max_core);
    } else {
        m_cpu_core_text.clear();
    }
    const NvmlGpuMetrics nvml = QueryNvmlGpuMetrics();
    m_gpu_util = nvml.util;
    m_gpu_temp = nvml.temp_c;
    if (nvml.power_w >= 0 && nvml.temp_c > 0) {
        m_gpu_detail_text = tr("%1W • %2°C").arg(nvml.power_w).arg(nvml.temp_c);
    } else if (nvml.temp_c > 0) {
        m_gpu_detail_text = tr("%1°C").arg(nvml.temp_c);
    } else if (nvml.power_w >= 0) {
        m_gpu_detail_text = tr("%1W").arg(nvml.power_w);
    } else {
        m_gpu_detail_text.clear();
    }
    const double ft_ms = !std::isnan(results.frametime) ? results.frametime * 1000.0 : -1.0;
    if (m_gpu_util >= 0 && ft_ms > 0.0) {
        m_gpu_ms_text = tr("%1 ms").arg(ft_ms * (m_gpu_util / 100.0), 0, 'f', 1);
        m_gpu_busy_text = tr("%1% busy").arg(m_gpu_util);
    } else if (m_gpu_util >= 0) {
        m_gpu_ms_text = QStringLiteral("-- ms");
        m_gpu_busy_text = tr("%1% busy").arg(m_gpu_util);
    } else {
        m_gpu_ms_text = tr("n/a");
        m_gpu_busy_text.clear();
    }
    if (m_vram_share >= 0.94) {
        m_limit_text = tr("Limited by VRAM");
        m_limit_kind = 3;
    } else if (m_gpu_util >= 92) {
        m_limit_text = tr("Limited by GPU");
        m_limit_kind = 2;
    } else if (cpu_pct >= 82.0) {
        m_limit_text = tr("Limited by CPU");
        m_limit_kind = 1;
    } else if (m_cpu_max_core >= 90 && (m_gpu_util < 0 || m_gpu_util < 85)) {
        m_limit_text = tr("Limited by 1 Core");
        m_limit_kind = 1;
    } else if (!std::isnan(fps) && ((fps >= 29.0 && fps <= 31.0) || (fps >= 58.5 && fps <= 61.5))) {
        m_limit_text = tr("Capped");
        m_limit_kind = 0;
    } else {
        m_limit_text = tr("Balanced");
        m_limit_kind = 0;
    }
#endif

    const QSize before = size();
    Relayout();
    if (size() != before) {
        Place(); // a bar on the right or in the middle keeps its border
    }
    update();

    // CHEEZYEMU_OVERLAY_TOGGLE=<updates>: shows and hides the bar every so many updates (two a
    // second) and logs it, to measure the frame rate with and without it in one run.
    static const int toggle = qEnvironmentVariableIntValue("CHEEZYEMU_OVERLAY_TOGGLE");
    if (toggle > 0 && ++m_toggle_updates % toggle == 0) {
        setVisible(!isVisible());
        LOG_INFO(Frontend, "Performance bar {}", isVisible() ? "shown" : "hidden");
    }

    // CHEEZYEMU_OVERLAY_SHOT=<file.png>: saves a picture of the bar every few updates and logs
    // where it sits, to check it without capturing the screen.
    static const QString shot = qEnvironmentVariable("CHEEZYEMU_OVERLAY_SHOT");
    if (!shot.isEmpty() && ++m_updates % 6 == 0) {
        grab().save(shot);
        // The status bar too, next to it: "<file>.statusbar.png".
        if (QStatusBar* const bar = m_main_window->statusBar()) {
            bar->grab().save(shot + QStringLiteral(".statusbar.png"));
        }
        const QRect area = Area();
        LOG_INFO(Frontend, "Performance bar at {},{} {}x{} over the game picture at {},{} {}x{}",
                 x(), y(), width(), height(), area.x(), area.y(), area.width(), area.height());
    }
}

void PerformanceOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const int opacity = std::clamp(UISettings::values.perf_overlay_opacity.GetValue(), 20, 100);
    const QRectF box{0.5, 0.5, width() - 1.0, height() - 1.0};
    QPainterPath shape;
    if (m_opaque) {
        painter.fillRect(rect(), QColor{9, 9, 11});
        shape.addRect(box);
    } else {
        shape.addRoundedRect(box, 5.0, 5.0);
    }
    painter.fillPath(shape, QColor{9, 9, 11, opacity * 255 / 100});
    // The yellow edge, cut by the box's corners.
    painter.save();
    painter.setClipPath(shape);
    painter.fillRect(QRect{0, 0, Edge, height()}, Cheese);
    painter.restore();
    painter.setPen(QPen{QColor{0xff, 0xd2, 0x3f, 70}, 1.0});
    painter.drawPath(shape);

    const QFontMetrics big{m_big};
    const QFontMetrics value{m_value};
    const QFontMetrics label{m_label};
    const bool has_header = !UISettings::values.perf_overlay_compact.GetValue() &&
                            ShowHwBannerFlag() && !m_hw_banner_text.isEmpty();
    const int y0 = has_header ? HeaderHeight : 0;
    const int h = height() - y0;
    if (has_header) {
        QFont header_font = m_label;
        header_font.setPointSizeF(7.5);
        painter.setFont(header_font);
        painter.setPen(QColor{0xdc, 0xdc, 0xe4});
        painter.drawText(QRect{Edge + Pad, 0, width() - Edge - Pad * 2, HeaderHeight},
                         Qt::AlignLeft | Qt::AlignVCenter, m_hw_banner_text);
        painter.setPen(QPen{QColor{255, 255, 255, 28}, 1.0});
        painter.drawLine(QPointF{static_cast<qreal>(Edge), static_cast<qreal>(y0)},
                         QPointF{width() - 1.0, static_cast<qreal>(y0)});
    }
    // Two text rows, centred in the bar below the optional hardware header.
    const int rows = value.height() + 2 + label.height();
    const int row1 = y0 + (h - rows) / 2 + value.ascent();
    const int row2 = y0 + (h - rows) / 2 + value.height() + 2 + label.ascent();

    for (size_t index = 0; index < m_cells.size(); ++index) {
        const Cell& cell = m_cells[index];
        if (index != 0) {
            painter.setPen(QPen{QColor{255, 255, 255, 34}, 1.0});
            const qreal line = cell.x - Gap / 2.0;
            painter.drawLine(QPointF{line, y0 + 9.0}, QPointF{line, y0 + h - 9.0});
        }
        switch (cell.kind) {
        case Cell::Kind::Fps: {
            painter.setFont(m_big);
            painter.setPen(Cheese);
            const int digits = std::max(big.horizontalAdvance(QStringLiteral("000")),
                                        big.horizontalAdvance(m_fps_text));
            // Right-aligned in their room, so "60" and "120" end at the same place.
            painter.drawText(QRect{cell.x, y0, digits, h}, Qt::AlignRight | Qt::AlignVCenter,
                             m_fps_text);
            painter.setFont(m_label);
            painter.setPen(Text);
            const int beside = cell.x + digits + 7;
            if (m_fps_detail.isEmpty()) {
                painter.drawText(QRect{beside, y0, cell.width, h}, Qt::AlignLeft | Qt::AlignVCenter,
                                 tr("FPS"));
            } else {
                painter.drawText(beside, row1 - 1, tr("FPS"));
                painter.setPen(Dim);
                painter.drawText(beside, row2, m_fps_detail);
            }
            break;
        }
        case Cell::Kind::Frame:
            painter.setFont(m_value);
            painter.setPen(Text);
            painter.drawText(cell.x, row1, m_frame_text);
            painter.setFont(m_label);
            painter.setPen(Dim);
            painter.drawText(cell.x, row2, m_low_text.isEmpty() ? tr("FRAME TIME") : m_low_text);
            break;
        case Cell::Kind::Graph: {
            // One bar per half second, newest on the right; a dip stands out in orange or red.
            const QRectF plot{static_cast<qreal>(cell.x), y0 + 10.0,
                              static_cast<qreal>(cell.width), h - 20.0};
            painter.setPen(Qt::NoPen);
            painter.fillRect(QRectF{plot.left(), plot.bottom(), plot.width(), 1.0},
                             QColor{255, 255, 255, 40});
            if (m_history.empty()) {
                break;
            }
            std::vector<double> sorted(m_history.begin(), m_history.end());
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            const double top = std::max(*std::max_element(m_history.begin(), m_history.end()),
                                        1.0);
            const qreal step = plot.width() / static_cast<qreal>(HistorySize);
            const size_t first = HistorySize - m_history.size();
            for (size_t i = 0; i < m_history.size(); ++i) {
                const double sample = m_history[i];
                const qreal bar = std::max<qreal>(1.0, plot.height() * sample / top);
                const QColor colour =
                    sample < median * 0.6 ? Bad : sample < median * 0.8 ? Slow : Cheese;
                painter.fillRect(QRectF{plot.left() + step * static_cast<qreal>(first + i),
                                        plot.bottom() - bar, std::max<qreal>(step - 0.25, 0.5),
                                        bar},
                                 colour);
            }
            break;
        }
        case Cell::Kind::CpuGpu: {
            const int names = std::max(label.horizontalAdvance(tr("CPU")),
                                       label.horizontalAdvance(tr("GPU"))) +
                              6;
            const int block = value.height() * 2 + 2;
            const int first = y0 + (h - block) / 2 + value.ascent();
            const int second = first + value.height() + 2;
            painter.setFont(m_label);
            painter.setPen(Dim);
            painter.drawText(cell.x, first, tr("CPU"));
            painter.drawText(cell.x, second, tr("GPU"));

            painter.setFont(m_value);
            painter.setPen(Text);
            painter.drawText(cell.x + names, first, m_cpu_text);
            painter.drawText(cell.x + names, second, m_gpu_ms_text);

            painter.setFont(m_label);
            const QColor lim_col = m_limit_kind == 3   ? Bad
                                 : m_limit_kind == 2   ? Slow
                                 : m_limit_kind == 1   ? Accent
                                                       : Dim;
            painter.setPen(lim_col);
            painter.drawText(cell.x + names + value.horizontalAdvance(m_cpu_text) + 8, first,
                             m_limit_text);
            if (!m_gpu_busy_text.isEmpty()) {
                const QColor busy_col = m_gpu_util >= 95 ? Bad : m_gpu_util >= 85 ? Slow : Dim;
                painter.setPen(busy_col);
                painter.drawText(cell.x + names + value.horizontalAdvance(m_gpu_ms_text) + 8,
                                 second, m_gpu_busy_text);
            }
            break;
        }
        case Cell::Kind::Status: {
            painter.setFont(m_value);
            const bool both = !m_upscale_text.isEmpty() && !m_frame_gen_text.isEmpty();
            if (both) {
                // Two lines of the same size, centred together.
                const int block = value.height() * 2 + 2;
                const int first = y0 + (h - block) / 2 + value.ascent();
                painter.setPen(Cheese);
                painter.drawText(cell.x, first, m_upscale_text);
                painter.setPen(Text);
                painter.drawText(cell.x, first + value.height() + 2, m_frame_gen_text);
            } else {
                painter.setPen(Cheese);
                painter.drawText(QRect{cell.x, y0, cell.width, h}, Qt::AlignLeft | Qt::AlignVCenter,
                                 m_upscale_text.isEmpty() ? m_frame_gen_text : m_upscale_text);
            }
            break;
        }
        case Cell::Kind::Memory: {
            const int names = std::max(label.horizontalAdvance(tr("RAM")),
                                       label.horizontalAdvance(tr("VRAM"))) +
                              8;
            const int block = value.height() * 2 + 2;
            const int first = y0 + (h - block) / 2 + value.ascent();
            const int second = first + value.height() + 2;
            painter.setFont(m_label);
            painter.setPen(Dim);
            painter.drawText(cell.x, first, tr("RAM"));
            if (!m_vram_text.isEmpty()) {
                painter.drawText(cell.x, second, tr("VRAM"));
            }
            painter.setFont(m_value);
            painter.setPen(Text);
            painter.drawText(cell.x + names, first, m_ram_text);
            if (!m_vram_text.isEmpty()) {
                // Video memory near its budget is where textures start to be thrown out.
                painter.setPen(m_vram_share > 0.95 ? Bad : m_vram_share > 0.85 ? Slow : Text);
                painter.drawText(cell.x + names, second, m_vram_text);
            }
            break;
        }
        case Cell::Kind::Details: {
            const int names = std::max(label.horizontalAdvance(tr("CPU")),
                                       label.horizontalAdvance(tr("GPU"))) +
                              8;
            const int block = value.height() * 2 + 2;
            const int first = y0 + (h - block) / 2 + value.ascent();
            const int second = first + value.height() + 2;
            painter.setFont(m_label);
            painter.setPen(Dim);
            if (!m_cpu_core_text.isEmpty()) {
                painter.drawText(cell.x, first, tr("CPU"));
            }
            if (!m_gpu_detail_text.isEmpty()) {
                painter.drawText(cell.x, second, tr("GPU"));
            }
            painter.setFont(m_value);
            if (!m_cpu_core_text.isEmpty()) {
                const QColor cpu_col = m_cpu_max_core >= 90 ? Accent : Text;
                painter.setPen(cpu_col);
                painter.drawText(cell.x + names, first, m_cpu_core_text);
            }
            if (!m_gpu_detail_text.isEmpty()) {
                const QColor gpu_col = m_gpu_temp >= 85 ? Bad
                                     : m_gpu_temp >= 78 ? Slow
                                                        : Text;
                painter.setPen(gpu_col);
                painter.drawText(cell.x + names, second, m_gpu_detail_text);
            }
            break;
        }
        }
    }
}

void PerformanceOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // Wayland ignores move() on top-level windows: the compositor drags it instead.
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) && windowHandle() &&
        windowHandle()->startSystemMove()) {
        return;
    }
    m_dragging = true;
    m_drag_offset = event->globalPosition().toPoint() - pos();
    setCursor(Qt::ClosedHandCursor);
}

void PerformanceOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        return;
    }
    const QRect area = Area();
    const int free_x = std::max(0, area.width() - width());
    const int free_y = std::max(0, area.height() - height());
    const int margin_x = std::min(Margin, free_x / 2);
    const int margin_y = std::min(Margin, free_y / 2);
    const QPoint target = event->globalPosition().toPoint() - m_drag_offset - area.topLeft();
    // Near a border, or the middle of the top or bottom one, the bar jumps to its place there.
    const auto snap = [](int at, int to) { return std::abs(at - to) <= Snap ? to : at; };
    int x = snap(snap(target.x(), margin_x), free_x - margin_x);
    int y = snap(snap(target.y(), margin_y), free_y - margin_y);
    if (y == margin_y || y == free_y - margin_y) {
        x = snap(x, free_x / 2);
    }
    // Anywhere over the picture, never lost outside it.
    x = std::clamp(x, 0, free_x);
    y = std::clamp(y, 0, free_y);
    move(area.left() + x, area.top() + y);
}

void PerformanceOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !m_dragging) {
        return;
    }
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
    SavePlace();
}

void PerformanceOverlay::contextMenuEvent(QContextMenuEvent* event) {
    auto& ui = UISettings::values;
    QMenu menu(this);

    QMenu* const position = menu.addMenu(tr("Position"));
    auto* const places = new QActionGroup(position);
    const std::pair<Anchor, QString> anchors[]{
        {TopLeft, tr("Top left")},
        {TopCentre, tr("Top centre")},
        {TopRight, tr("Top right")},
        {BottomLeft, tr("Bottom left")},
        {BottomCentre, tr("Bottom centre")},
        {BottomRight, tr("Bottom right")},
    };
    for (const auto& [choice, name] : anchors) {
        QAction* const action = position->addAction(name, [this, anchor = choice] {
            UISettings::values.perf_overlay_anchor.SetValue(static_cast<int>(anchor));
            Place();
        });
        action->setCheckable(true);
        action->setChecked(ui.perf_overlay_anchor.GetValue() == static_cast<int>(choice));
        places->addAction(action);
    }
    if (ui.perf_overlay_anchor.GetValue() == static_cast<int>(Custom)) {
        position->addSeparator();
        QAction* const dragged = position->addAction(tr("Where I dragged it"));
        dragged->setCheckable(true);
        dragged->setChecked(true);
        dragged->setEnabled(false);
    }

    const auto toggle = [this, &menu](const QString& name, Settings::Setting<bool>& setting) {
        QAction* const action = menu.addAction(name, [this, &setting] {
            setting.SetValue(!setting.GetValue());
            Relayout();
            Place();
            update();
        });
        action->setCheckable(true);
        action->setChecked(setting.GetValue());
    };
    toggle(tr("Compact (frame rate and frame time only)"), ui.perf_overlay_compact);
    toggle(tr("Frame rate history"), ui.perf_overlay_graph);
    toggle(tr("Memory"), ui.perf_overlay_memory);
    {
        QAction* const details_act =
            menu.addAction(tr("Details (CPU Single Core • GPU W/°C)"), [this] {
                ShowDetailsFlag() = !ShowDetailsFlag();
                RefreshFromSettings();
            });
        details_act->setCheckable(true);
        details_act->setChecked(ShowDetailsFlag());
    }
    {
        QAction* const hw_act =
            menu.addAction(tr("Hardware info (CPU | GPU | RAM)"), [this] {
                ShowHwBannerFlag() = !ShowHwBannerFlag();
                RefreshFromSettings();
            });
        hw_act->setCheckable(true);
        hw_act->setChecked(ShowHwBannerFlag());
    }

    QMenu* const opacity = menu.addMenu(tr("Background"));
    auto* const levels = new QActionGroup(opacity);
    for (const int percent : {100, 85, 70, 50, 30}) {
        QAction* const action = opacity->addAction(tr("%1%").arg(percent), [this, percent] {
            UISettings::values.perf_overlay_opacity.SetValue(percent);
            update();
        });
        action->setCheckable(true);
        action->setChecked(ui.perf_overlay_opacity.GetValue() == percent);
        levels->addAction(action);
    }

    // The status bar's graphics choices: fullscreen hides the status bar, not this bar.
    menu.addSeparator();
    m_main_window->AddQuickGraphicsMenu(menu);

    menu.addSeparator();
    menu.addAction(tr("Hide the performance bar"), [this] { close(); });
    menu.exec(event->globalPos());
}

void PerformanceOverlay::closeEvent(QCloseEvent*) {
    emit closed();
}

bool PerformanceOverlay::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::WindowStateChange:
        Place();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}
