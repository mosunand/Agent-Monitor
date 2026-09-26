// UsageHeatmap.cpp — see UsageHeatmap.h.

#include "UsageHeatmap.h"

#include <QDate>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include "ui/Theme.h"
#include "util/Format.h"

#include <cmath>

#include <algorithm>

namespace {

// 5-step blue ramp over the dim base; level 0 = empty day (still visible),
// 1..4 = increasing token volume
QColor rampColor(const QColor& accent, const QColor& base, int level)
{
    if (level <= 0)
        return base;
    QColor c = accent;
    switch (level) {
    case 1: c.setAlpha(70); break;
    case 2: c.setAlpha(120); break;
    case 3: c.setAlpha(180); break;
    case 4: c.setAlpha(255); break;
    }
    return c;
}

} // namespace

UsageHeatmap::UsageHeatmap(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(150);
}

void UsageHeatmap::setData(const QVector<QPair<qint64, qint64>>& daily)
{
    m_daily = daily;
    m_byDay.clear();
    for (const auto& d : m_daily)
        m_byDay.insert(d.first, d.second);
    rebuildGrid();
    update();
}

void UsageHeatmap::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    rebuildGrid();
    update();
}

void UsageHeatmap::rebuildGrid()
{
    m_grid.clear();
    m_hoverCol = m_hoverRow = -1;

    const QDate today = QDate::currentDate();
    QDate start = today.addDays(-370);
    start = start.addDays(-(start.dayOfWeek() - 1)); // back to Monday
    m_gridStart = start;
    m_cols = int(start.daysTo(today)) / 7 + 1;

    // ── build the day grid first (chronological: col-major, Mon→Sun) ──
    for (int c = 0; c < m_cols; ++c) {
        QVector<Cell> col(7);
        for (int r = 0; r < 7; ++r) {
            const QDate day = start.addDays(c * 7 + r);
            Cell cell;
            cell.dayMs = QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch();
            cell.future = day > today;
            col[r] = cell;
        }
        m_grid.push_back(col);
    }

    if (m_mode == Daily) {
        for (int c = 0; c < m_cols; ++c)
            for (int r = 0; r < 7; ++r)
                m_grid[c][r].value = m_byDay.value(m_grid[c][r].dayMs);
    } else if (m_mode == Weekly) {
        for (int c = 0; c < m_cols; ++c) {
            qint64 weekSum = 0;
            for (int r = 0; r < 7; ++r)
                weekSum += m_byDay.value(m_grid[c][r].dayMs);
            for (int r = 0; r < 7; ++r)
                m_grid[c][r].value = weekSum;
        }
    } else {
        // Cumulative：按时间顺序滚动累加。无数据的日子继承此前
        // 的累计值（绝不是最终总量——那是旧的显示 bug）
        qint64 cum = 0;
        int idx = 0;
        for (int c = 0; c < m_cols; ++c)
            for (int r = 0; r < 7; ++r) {
                const qint64 dayMs = m_grid[c][r].dayMs;
                while (idx < m_daily.size() && m_daily[idx].first <= dayMs)
                    cum += m_daily[idx++].second;
                m_grid[c][r].value = cum;
            }
    }

    // 分级基准：活跃日（有值且非未来）的值升序排列
    m_activeSorted.clear();
    for (int c = 0; c < m_cols; ++c)
        for (int r = 0; r < 7; ++r) {
            const Cell& cell = m_grid[c][r];
            if (!cell.future && cell.value > 0)
                m_activeSorted.push_back(cell.value);
        }
    std::sort(m_activeSorted.begin(), m_activeSorted.end());
}

void UsageHeatmap::paintEvent(QPaintEvent*)
{
    // 2x 离屏渲染：小格子的抗锯齿台阶在高分辨率画布上被压到亚像素，
    // 再平滑缩小贴回 → 视觉完全平滑（与光晕背景同一套思路）
    QPixmap canvas(width() * 2, height() * 2);
    canvas.setDevicePixelRatio(2.0);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Palette& pal = Theme::instance().pal();
    const int leftPad = 22;   // weekday labels
    const int bottomPad = 18; // month labels
    const int topPad = 4;
    const int gap = 3;
    const int availW = width() - leftPad - 8;
    const int availH = height() - topPad - bottomPad;
    if (m_cols <= 0 || availW <= 0 || availH <= 0)
        return;
    const int cellW = qBound(12, availW / m_cols - gap, 24);
    const int cellH = qBound(12, availH / 7 - gap, 24);
    if (cellW < 4 || cellH < 4)
        return;
    const int colStep = cellW + gap;
    const int rowStep = cellH + gap;
    m_colStep = colStep;
    m_rowStep = rowStep;
    m_cellW = cellW;
    m_cellH = cellH;

    QFont small = p.font();
    small.setPointSizeF(7.5);
    p.setFont(small);
    QFontMetrics fm(small);

    const QDate today = QDate::currentDate();
    for (int c = 0; c < m_cols; ++c) {
        const int x = leftPad + c * colStep;
        for (int r = 0; r < 7; ++r) {
            const Cell& cell = m_grid[c][r];
            const int y = topPad + r * rowStep;
            QColor fill;
            if (!cell.future) {
                int level = 0;
                if (cell.value > 0 && !m_activeSorted.isEmpty()) {
                    // 按序号分位定级：活跃日里最小的 25% 为 1 档，
                    // 前 25% 直接满格 —— 典型活跃日就能全亮
                    const int rank = int(std::upper_bound(m_activeSorted.constBegin(),
                                                          m_activeSorted.constEnd(),
                                                          cell.value)
                                         - m_activeSorted.constBegin());
                    const double frac = double(rank) / double(m_activeSorted.size());
                    level = 1 + qMin(3, int(frac * 4.0));
                }
                fill = rampColor(pal.accent, pal.surface3, level);
            }
            if (fill.isValid()) {
                p.setPen(Qt::NoPen);
                p.setBrush(fill);
                // 圆形格子：小尺寸下圆比圆角矩形平滑得多（无锯齿边）
                const int d = qMin(cellW, cellH);
                p.drawEllipse(x + (cellW - d) / 2, y + (cellH - d) / 2, d, d);
            }
        }
        // hover outline
        if (c == m_hoverCol) {
            for (int r = 0; r < 7; ++r) {
                if (r == m_hoverRow) {
                    p.setPen(QPen(pal.fg3, 1));
                    p.setBrush(Qt::NoBrush);
                    const int d = qMin(cellW, cellH) + 2;
                    p.drawEllipse(leftPad + c * colStep - 1
                                      + (cellW - d) / 2 + 1,
                                  topPad + r * rowStep - 1
                                      + (cellH - d) / 2 + 1, d, d);
                }
            }
        }
        // month label on the first column of each new month
        const QDate colDay = m_gridStart.addDays(c * 7);
        const QDate prevDay = c > 0 ? m_gridStart.addDays((c - 1) * 7) : QDate();
        if (c == 0 || colDay.month() != prevDay.month()) {
            p.setPen(pal.fg4);
            p.drawText(QRect(x - 4, height() - bottomPad - 2, 40, fm.height()),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(colDay.month()) + QStringLiteral("月"));
        }
    }
    // weekday hints (一 / 三 / 五)
    p.setPen(pal.fg5);
    const QStringList wd = { QStringLiteral("一"), QString(), QStringLiteral("三"),
                             QString(), QStringLiteral("五"), QString(), QString() };
    for (int r = 0; r < 7; ++r)
        if (!wd[r].isEmpty())
            p.drawText(QRect(0, topPad + r * rowStep, leftPad - 4, cellH),
                       Qt::AlignRight | Qt::AlignVCenter, wd[r]);
    p.end();

    QPainter screen(this);
    screen.setRenderHint(QPainter::SmoothPixmapTransform, true);
    screen.drawPixmap(0, 0, canvas);
}

void UsageHeatmap::mouseMoveEvent(QMouseEvent* e)
{
    const int leftPad = 22, topPad = 4;
    if (m_colStep <= 0 || m_rowStep <= 0 || m_cols <= 0) {
        return;
    }
    const int col = (e->pos().x() - leftPad) / m_colStep;
    const int row = (e->pos().y() - topPad) / m_rowStep;
    if (col < 0 || col >= m_cols || row < 0 || row >= 7
        || e->pos().x() < leftPad || e->pos().y() < topPad) {
        if (m_hoverCol != -1) {
            m_hoverCol = m_hoverRow = -1;
            update();
        }
        return;
    }
    m_hoverCol = col;
    m_hoverRow = row;
    update();
    const Cell& cell = m_grid[col][row];
    if (!cell.future) {
        QString prefix;
        if (m_mode == Cumulative)
            prefix = QStringLiteral("累计 ");
        else if (m_mode == Weekly)
            prefix = QStringLiteral("本周 ");
        const QString tip = QStringLiteral("%1\n%2%3 tokens")
            .arg(QDateTime::fromMSecsSinceEpoch(cell.dayMs).date().toString(
                     QStringLiteral("yyyy/M/d (ddd)")),
                 prefix, Format::fmtNumCn(double(cell.value)));
        QToolTip::showText(e->globalPosition().toPoint(), tip, this);
    }
}

void UsageHeatmap::leaveEvent(QEvent* e)
{
    QWidget::leaveEvent(e);
    if (m_hoverCol != -1) {
        m_hoverCol = m_hoverRow = -1;
        update();
    }
}
