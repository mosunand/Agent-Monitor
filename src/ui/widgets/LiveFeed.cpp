// LiveFeed.cpp — see LiveFeed.h.

#include "LiveFeed.h"

#include <QDateTime>
#include <QLabel>
#include <QPainter>

#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "util/Format.h"

using namespace UiUtil;

// 空态：与 MiniChart 同一套「淡环 + 文案」语言，比一行灰字更有
// "即将有数据"的语义
class FeedEmptyHint : public QWidget {
public:
    explicit FeedEmptyHint(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette& pal = Theme::instance().pal();
        const int side = qMin(width(), height()) - 64;
        if (side > 44) {
            const QRect ring((width() - side) / 2, (height() - side) / 2,
                             side, side);
            p.setPen(QPen(pal.surface3, 8, Qt::SolidLine, Qt::FlatCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(ring, 90 * 16, -300 * 16);
            QColor tip = pal.accent;
            tip.setAlpha(140);
            p.setPen(QPen(tip, 8, Qt::SolidLine, Qt::FlatCap));
            p.drawArc(ring, 90 * 16, -40 * 16);
        }
        p.setPen(pal.fg4);
        QFont f = p.font();
        f.setPointSizeF(9);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待新事件…"));
    }
};

// ── model ────────────────────────────────────────────────────

LiveFeedModel::LiveFeedModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

void LiveFeedModel::pushRecords(const QVector<types::UsageRecord>& rows)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<FeedRow> feed;
    for (const types::UsageRecord& r : rows) {
        FeedRow f;
        f.timeMs = r.timeMs;
        f.agentId = r.agentId;
        f.model = r.model.isEmpty() ? QStringLiteral("?") : r.model;
        f.effort = r.effort;
        f.summary = QStringLiteral("in %1 / out %2")
                        .arg(Format::fmtNum(double(r.inputTokens)),
                             Format::fmtNum(double(r.outputTokens)));
        if (r.cachedTokens > 0)
            f.summary += QStringLiteral(" (缓存 %1)")
                             .arg(Format::fmtNum(double(r.cachedTokens)));
        f.durOk = r.durOk;
        f.durApprox = r.durApprox;
        f.durMs = qint64(r.durationMs);
        f.tpsOk = r.tpsOk;
        f.tps = r.tps;
        f.addedAtMs = now;
        feed.push_back(f);
    }
    if (feed.isEmpty())
        return;
    beginInsertRows(QModelIndex(), 0, feed.size() - 1);
    // rows arrive oldest → newest; list is newest-first. Prepend FORWARD so
    // the batch's newest row ends up at the very top, and give it the
    // highest seq (seq grows with time).
    for (int i = 0; i < feed.size(); ++i)
        feed[i].seq = m_seq + qint64(i);
    m_seq += feed.size();
    for (int i = 0; i < feed.size(); ++i)
        m_rows.push_front(feed[i]);
    endInsertRows();
    while (m_rows.size() > 60) {
        beginRemoveRows(QModelIndex(), m_rows.size() - 1, m_rows.size() - 1);
        m_rows.removeLast();
        endRemoveRows();
    }
}

void LiveFeedModel::clear()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

int LiveFeedModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QVariant LiveFeedModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    if (role == Qt::UserRole + 1)
        return QVariant::fromValue(index.row());
    return {};
}

// ── delegate ──────────────────────────────────────────────────

LiveFeedDelegate::LiveFeedDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QSize LiveFeedDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    return QSize(200, 38);
}

void LiveFeedDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    const auto* feed = qobject_cast<const LiveFeedModel*>(index.model());
    if (!feed)
        return;
    if (index.row() < 0 || index.row() >= feed->rowCount())
        return;
    const FeedRow& r = feed->rowAt(index.row());
    const Palette& pal = Theme::instance().pal();
    const QRect rect = option.rect;

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    // 新行 300ms 内整体上滑 3px + 透明度爬升（一次性插值，零额外开销）
    const qint64 bornAge = QDateTime::currentMSecsSinceEpoch() - r.addedAtMs;
    if (bornAge < 300) {
        const double k = double(bornAge) / 300.0;
        p->translate(0.0, (1.0 - k) * 3.0);
        p->setOpacity(qMin(1.0, 0.4 + 0.6 * k));
    }

    // background: flash (newest) > hover
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - r.addedAtMs;
    if (age < 1200) {
        QColor flash = pal.accent;
        flash.setAlpha(qRound(0.18 * 255 * (1.0 - double(age) / 1200.0)));
        p->fillRect(rect, flash);
    } else if (option.state & QStyle::State_MouseOver) {
        p->fillRect(rect, pal.surface1);
    }
    p->setPen(QPen(pal.borderSoft, 1));
    p->drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom());

    QFont mono(QStringLiteral("Consolas"));
    QFont smallF = mono;
    smallF.setPointSizeF(8.5);
    QFont bodyF = mono;
    bodyF.setPointSizeF(9.5);
    QFont labelF = mono;
    labelF.setPointSizeF(9.5);
    labelF.setBold(true);
    QFontMetrics sm(smallF);
    QFontMetrics bm(bodyF);
    QFontMetrics lfm(labelF);

    // seq + time
    p->setFont(smallF);
    p->setPen(pal.fg5);
    p->drawText(QRect(rect.left() + 8, rect.y(), 42, rect.height()),
                Qt::AlignRight | Qt::AlignVCenter, QString::number(r.seq));
    p->setPen(pal.fg4);
    p->drawText(QRect(rect.left() + 56, rect.y(), 84, rect.height()),
                Qt::AlignLeft | Qt::AlignVCenter, Format::fmtTime(r.timeMs));

    // agent dot：新行 1.2s 内呼吸放大 + 光晕，之后回落为静点
    {
        const qint64 age2 = QDateTime::currentMSecsSinceEpoch() - r.addedAtMs;
        double radius = 4.0;
        if (age2 < 1200)
            radius += 1.6 * (1.0 - double(age2) / 1200.0);
        p->setPen(Qt::NoPen);
        QColor dot = r.agentColor.isValid() ? r.agentColor : pal.catLlm;
        if (age2 < 1200) {
            QColor glow = dot;
            glow.setAlpha(90);
            p->setBrush(glow);
            p->drawEllipse(QPointF(rect.left() + 152, rect.center().y()),
                           radius + 3, radius + 3);
        }
        p->setBrush(dot);
        p->drawEllipse(QPointF(rect.left() + 152, rect.center().y()),
                       radius, radius);
    }

    // ── right block: 从右往左排（速度 chip → 时长），绝不与描述重叠 ──
    int rightEdge = rect.right() - 12;

    // speed chip
    if (r.tpsOk) {
        const QString chipText =
            (r.durApprox ? QStringLiteral("≈") : QString())
            + QString::number(r.tps, 'f', 1) + QStringLiteral(" t/s");
        const QColor chipColor = UiUtil::speedColor(Format::speedTier(r.tps));
        const int chipW = sm.horizontalAdvance(chipText) + 18;
        paintPill(p, QRect(rightEdge - chipW, rect.center().y() - 9, chipW, 18),
                  chipText, chipColor);
        rightEdge -= chipW + 12;
    }

    // duration
    const QString durText = r.durOk ? Format::fmtMs(double(r.durMs)) : QString();
    if (!durText.isEmpty()) {
        const int dw = sm.horizontalAdvance(durText);
        p->setFont(smallF);
        p->setPen(pal.fg4);
        p->drawText(QRect(rightEdge - dw, rect.y(), dw + 4, rect.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, durText);
        rightEdge -= dw + 12;
    }

    // ── desc: agent label（彩色粗体）+ model + summary + 短 id ──
    const int descX = rect.left() + 168;
    int x = descX;
    const QString labelText = r.agentId + QStringLiteral("→");
    const QColor labelColor = r.agentColor.isValid() ? r.agentColor : pal.catLlm2;
    p->setFont(labelF);
    p->setPen(labelColor);
    p->drawText(QRect(x, rect.y(), lfm.horizontalAdvance(labelText) + 2, rect.height()),
                Qt::AlignLeft | Qt::AlignVCenter, labelText);
    x += lfm.horizontalAdvance(labelText) + 8;

    QString modelText = r.model;
    if (!r.effort.isEmpty())
        modelText += QStringLiteral(" · ") + r.effort;
    const int modelW = bm.horizontalAdvance(modelText) + 8;
    p->setFont(bodyF);
    p->setPen(pal.catLlm2);
    p->drawText(QRect(x, rect.y(), modelW, rect.height()),
                Qt::AlignLeft | Qt::AlignVCenter, modelText);
    x += modelW;

    const int avail = qMax(40, (rightEdge - 4) - x);
    const QString summary = bm.elidedText(r.summary, Qt::ElideRight, avail);
    p->setFont(bodyF);
    p->setPen(pal.fg1);
    p->drawText(QRect(x, rect.y(), bm.horizontalAdvance(summary) + 4, rect.height()),
                Qt::AlignLeft | Qt::AlignVCenter, summary);

    p->restore();
}

// ── view ─────────────────────────────────────────────────────

LiveFeed::LiveFeed(QWidget* parent)
    : QListView(parent)
    , m_model(new LiveFeedModel(this))
{
    setModel(m_model);
    setItemDelegate(new LiveFeedDelegate(this));
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAttribute(Qt::WA_Hover);

    m_emptyLabel = new FeedEmptyHint(this);
    m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto updateEmpty = [this] {
        m_emptyLabel->setVisible(m_model->rowCount() == 0);
        m_emptyLabel->setGeometry(0, 0, width(), qMax(height(), 60));
    };
    updateEmpty();
    connect(m_model, &LiveFeedModel::rowsInserted, this, updateEmpty);
    connect(m_model, &LiveFeedModel::rowsRemoved, this, updateEmpty);
    connect(m_model, &LiveFeedModel::modelReset, this, updateEmpty);

    m_flashTimer.setInterval(100);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        viewport()->update();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < m_model->rowCount() && i < 4; ++i) {
            if (now - m_model->rowAt(i).addedAtMs < 1200)
                return;
        }
        m_flashTimer.stop();
    });
    connect(m_model, &LiveFeedModel::rowsInserted, this,
            [this] { m_flashTimer.start(); });
}

void LiveFeed::resizeEvent(QResizeEvent* e)
{
    QListView::resizeEvent(e);
    if (m_emptyLabel)
        m_emptyLabel->setGeometry(0, 0, width(), qMax(height(), 60));
}
