// UsageDonut.cpp — see UsageDonut.h.

#include "UsageDonut.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <cmath>

#include "ui/Theme.h"
#include "util/Format.h"

UsageDonut::UsageDonut(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(220);
    setMouseTracking(true);
    m_anim.setDuration(900);
    m_anim.setStartValue(0.0);
    m_anim.setEndValue(1.0);
    m_anim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_progress = v.toDouble();
        update();
    });
}

QRect UsageDonut::ringRect() const
{
    const int avail = qMin(width(), height()) - 24;
    const int penW = penWidth();
    const int side = avail - penW;
    return QRect((width() - side) / 2, (height() - side) / 2, side, side);
}

int UsageDonut::penWidth() const
{
    const int avail = qMin(width(), height()) - 24;
    return qMax(16, avail / 5);
}

void UsageDonut::animate()
{
    m_anim.stop();
    m_anim.start();
}

void UsageDonut::setData(const QVector<Slice>& slices)
{
    m_slices = slices;
    m_hoverSlice = -1;
    update();
}

void UsageDonut::mouseMoveEvent(QMouseEvent* e)
{
    const QRect ring = ringRect();
    const int pw = penWidth();
    const QPointF c = ring.center();
    const double dx = e->pos().x() - c.x();
    const double dy = c.y() - e->pos().y(); // y 翻转 → 数学期望方向
    const double dist = std::sqrt(dx * dx + dy * dy);
    const double inner = ring.width() / 2.0 - pw / 2.0;
    const double outer = ring.width() / 2.0 + pw / 2.0;

    int found = -1;
    if (dist >= inner - 4 && dist <= outer + 4) {
        double ang = qRadiansToDegrees(std::atan2(dy, dx)); // -180..180, 0=3点钟
        double p = 90.0 - ang; // 0 = 12 点钟，顺时针
        while (p < 0)
            p += 360;
        while (p >= 360)
            p -= 360;
        qint64 total = 0;
        for (const Slice& s : m_slices)
            total += s.value;
        if (total > 0) {
            double acc = 0;
            for (int i = 0; i < m_slices.size(); ++i) {
                const double span = double(m_slices[i].value) / double(total) * 360.0;
                if (m_slices[i].value > 0 && p >= acc && p < acc + span) {
                    found = i;
                    break;
                }
                acc += span;
            }
        }
    }
    if (found != m_hoverSlice) {
        m_hoverSlice = found;
        update();
    }
    if (found >= 0) {
        const Slice& s = m_slices[found];
        qint64 total = 0;
        for (const Slice& s2 : m_slices)
            total += s2.value;
        const double pct = double(s.value) * 100.0 / double(total);
        const QString tip = QStringLiteral(
            "<span style='color:%1'>●</span> <b>%2</b><br/>%3 tokens · %4%")
            .arg(s.color.name(), s.label.toHtmlEscaped(),
                 Format::fmtNumCn(double(s.value)), QString::number(pct, 'f', 1));
        QToolTip::showText(e->globalPosition().toPoint(), tip, this);
    }
}

void UsageDonut::leaveEvent(QEvent* e)
{
    QWidget::leaveEvent(e);
    if (m_hoverSlice != -1) {
        m_hoverSlice = -1;
        update();
    }
}

void UsageDonut::setCenterSuffix(const QString& suffix)
{
    m_suffix = suffix;
    update();
}

void UsageDonut::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Palette& pal = Theme::instance().pal();

    qint64 total = 0;
    for (const Slice& s : m_slices)
        total += s.value;

    // the ring is a STROKE on the ellipse path: the stroke extends penW/2
    // beyond the path on both sides, so the path rect must shrink by the pen
    // width or the outer edge (and thus the whole donut) overflows the widget
    const int avail = qMin(width(), height()) - 24;
    if (avail < 60)
        return;
    const int penW = penWidth();
    const QRect ring = ringRect();

    p.setFont(p.font());
    const double prog = m_progress;
    if (total <= 0 || prog <= 0) {
        p.setPen(QPen(pal.surface3, penW, Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(ring, 0, 360 * 16);
    } else {
        // soft halo behind the ring — a glow that lifts it off the card
        {
            QColor halo = pal.accent;
            halo.setAlpha(38);
            p.setPen(QPen(halo, penW + 8, Qt::SolidLine, Qt::FlatCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(ring, 0, 360 * 16);
        }
        // segments from 12 o'clock, clockwise, sweeping in with the
        // animation. Boundaries are accumulated in DOUBLE and rounded once —
        // adjacent slices share the SAME rounded boundary value, and the
        // final boundary is exactly (start − full): the ring is seamless,
        // with no inter-slice gaps and no integer-drift seam.
        const double full = 360.0 * 16.0 * prog;
        double cum = 0.0; // cumulative fraction [0..1]
        int sliceIdx = -1;
        for (const Slice& s : m_slices) {
            ++sliceIdx;
            if (s.value <= 0)
                continue;
            const double nextCum = cum + double(s.value) / double(total);
            const int a0 = qRound(90.0 * 16.0 - cum * full);
            const int a1 = qRound(90.0 * 16.0 - nextCum * full);
            const int span = a0 - a1;
            cum = nextCum;
            if (span <= 0)
                continue; // sub-1/16° slice — invisible either way
            const QColor c = (sliceIdx == m_hoverSlice) ? s.color.lighter(125)
                                                        : s.color;
            p.setPen(QPen(c, penW, Qt::SolidLine, Qt::FlatCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(ring, a0, -span);
        }
    }

    // center total (fades in with the sweep)
    p.setOpacity(prog);
    QFont f = p.font();
    f.setPointSizeF(15);
    f.setBold(true);
    p.setFont(f);
    p.setPen(pal.fg);
    QFontMetrics fm(f);
    const QString totalStr = Format::fmtNumCn(double(total));
    p.drawText(QRect(0, ring.center().y() - fm.height() - 2, width(), fm.height()),
               Qt::AlignHCenter | Qt::AlignVCenter, totalStr);
    QFont f2 = p.font();
    f2.setBold(false);
    f2.setPointSizeF(8.5);
    p.setFont(f2);
    p.setPen(pal.fg3);
    p.drawText(QRect(0, ring.center().y() + 2, width(), fm.height()),
               Qt::AlignHCenter | Qt::AlignVCenter, m_suffix);
}
