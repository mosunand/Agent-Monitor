// UiUtil.cpp — see UiUtil.h.

#include "UiUtil.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPaintEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QVariant>

// ── BadgeLabel ────────────────────────────────────────────────

BadgeLabel::BadgeLabel(const QString& text, const QString& kind, QWidget* parent)
    : QWidget(parent)
    , m_text(text)
    , m_kind(kind)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void BadgeLabel::setKind(const QString& kind)
{
    m_kind = kind;
    update();
}

void BadgeLabel::setText(const QString& text)
{
    m_text = text;
    updateGeometry();
    update();
}

QSize BadgeLabel::sizeHint() const
{
    QFont f = font();
    f.setPointSizeF(8.0);
    const int w = QFontMetrics(f).horizontalAdvance(m_text);
    return QSize(w + 16, QFontMetrics(f).height() + 5);
}

void BadgeLabel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QFont f = font();
    f.setPointSizeF(8.0);
    p.setFont(f);

    const QColor color = Theme::instance().badgeColor(m_kind);
    QColor border = color;
    border.setAlpha(qRound(color.alpha() * 0.30));
    QColor bg = color;
    bg.setAlpha(qRound(color.alpha() * 0.08));

    p.setPen(QPen(border, 1));
    p.setBrush(bg);
    const qreal r = qMin<qreal>(9.0, qMin(width(), height()) / 2.0);
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), r, r);
    p.setPen(color);
    p.drawText(rect(), Qt::AlignCenter, m_text);
}

// ── CardFrame ────────────────────────────────────────────────

CardFrame::CardFrame(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("card"));
}

// ── Dot ──────────────────────────────────────────────────────

Dot::Dot(int diameter, QWidget* parent)
    : QWidget(parent)
    , m_d(diameter)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFixedSize(diameter + 2, diameter + 2);
}

void Dot::setColor(const QColor& c)
{
    m_color = c;
    update();
}

QSize Dot::sizeHint() const
{
    return QSize(m_d + 2, m_d + 2);
}

void Dot::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QColor glow = m_color;
    glow.setAlpha(70);
    p.setBrush(glow);
    p.drawEllipse(0, 0, m_d + 2, m_d + 2);
    QColor core = m_color;
    core.setAlpha(255);
    p.setBrush(core);
    p.drawEllipse(1, 1, m_d, m_d);
}

// ── SpinnerWidget ─────────────────────────────────────────────

SpinnerWidget::SpinnerWidget(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(15, 15);
    auto* t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this] {
        m_angle = (m_angle + 30) % 360;
        update();
    });
    t->start(60);
}

QSize SpinnerWidget::sizeHint() const
{
    return QSize(15, 15);
}

void SpinnerWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPen pen(Theme::instance().pal().accent, 2);
    p.setPen(pen);
    p.drawArc(QRect(2, 2, width() - 5, height() - 5), m_angle * 16, 300 * 16);
}

// ── helpers ──────────────────────────────────────────────────

namespace UiUtil {

QString statusBadgeKind(const QString& status)
{
    if (status == QLatin1String("completed") || status == QLatin1String("success"))
        return QStringLiteral("green");
    if (status == QLatin1String("running"))
        return QStringLiteral("blue");
    if (status == QLatin1String("error") || status == QLatin1String("failed"))
        return QStringLiteral("red");
    if (status == QLatin1String("cancelled"))
        return QStringLiteral("yellow");
    return QStringLiteral("dim");
}

QColor speedColor(int tier)
{
    const Palette& p = Theme::instance().pal();
    switch (tier) {
    case 1: return p.sevErr;
    case 2: return p.sevWarn;
    case 3: return p.sevOk;
    default: return p.fg4;
    }
}

QColor catColor(const QString& category)
{
    const Palette& p = Theme::instance().pal();
    if (category == QLatin1String("conversation")) return p.catConversation;
    if (category == QLatin1String("llm")) return p.catLlm;
    if (category == QLatin1String("tool")) return p.catTool;
    if (category == QLatin1String("network")) return p.catNetwork;
    if (category == QLatin1String("usage")) return p.catUsage;
    if (category == QLatin1String("prompt")) return p.catPrompt;
    if (category == QLatin1String("lifecycle")) return p.catLifecycle;
    return p.fg5; // other
}

// ── 渐变分隔线 ──────────────────────────────────────────────

class GradientBar : public QFrame {
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Palette& pal = Theme::instance().pal();
        QLinearGradient g(0, 0, width(), 0);
        QColor a = pal.accent;
        QColor b = pal.accent2;
        a.setAlpha(220);
        b.setAlpha(220);
        g.setColorAt(0, a);
        g.setColorAt(1, b);
        p.setPen(Qt::NoPen);
        p.setBrush(QBrush(g));
        p.drawRoundedRect(QRectF(0, 1, width(), 2), 1, 1);
    }
public:
    QSize sizeHint() const override { return QSize(20, 4); }
};

QWidget* gradientDivider()
{
    auto* bar = new GradientBar;
    bar->setFixedHeight(4);
    bar->setAttribute(Qt::WA_TransparentForMouseEvents);
    return bar;
}

QWidget* sectionLabel(const QString& title, const QString& sub)
{
    auto* w = new QWidget;
    w->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(0, 14, 0, 8);
    lay->setSpacing(8);
    auto* t = new QLabel(title);
    t->setObjectName(QStringLiteral("h2"));
    lay->addWidget(t);
    if (!sub.isEmpty()) {
        auto* s = new QLabel(sub);
        s->setProperty("cls", "sectionSub");
        lay->addWidget(s, 1);
    } else {
        lay->addStretch(1);
    }
    return w;
}

QLabel* label(const QString& text, const QString& cls)
{
    auto* l = new QLabel(text);
    if (!cls.isEmpty())
        l->setProperty("cls", cls);
    return l;
}

void paintPill(QPainter* p, const QRect& rect, const QString& text, const QColor& color,
               bool bold)
{
    QColor border = color;
    border.setAlpha(qRound(color.alpha() * 0.35));
    QColor bg = color;
    bg.setAlpha(qRound(color.alpha() * 0.10));
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    QFont f = p->font();
    f.setPointSizeF(8.5);
    f.setBold(bold);
    f.setFamily(QStringLiteral("Consolas"));
    p->setFont(f);
    const qreal r = qMin<qreal>(8.0, rect.height() / 2.0);
    p->setPen(QPen(border, 1));
    p->setBrush(bg);
    p->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), r, r);
    p->setPen(color);
    p->setBrush(Qt::NoBrush);
    p->drawText(rect, Qt::AlignCenter, text);
    p->restore();
}

} // namespace

// ── RowHoverTable / RowHoverDelegate ─────────────────────────

UiUtil::RowHoverTable::RowHoverTable(QWidget* parent)
    : QTableWidget(parent)
{
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    m_bars = new ValueBarDelegate(this);
    setItemDelegate(m_bars);
}

void UiUtil::RowHoverTable::setColumnMax(int column, double max)
{
    m_bars->setColumnMax(column, max);
    viewport()->update();
}

bool UiUtil::RowHoverTable::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == viewport()) {
        if (e->type() == QEvent::MouseMove) {
            const int row = rowAt(static_cast<QMouseEvent*>(e)->pos().y());
            if (row != m_hoverRow) {
                m_hoverRow = row;
                viewport()->update();
            }
        } else if (e->type() == QEvent::Leave) {
            if (m_hoverRow != -1) {
                m_hoverRow = -1;
                viewport()->update();
            }
        }
    }
    return QTableWidget::eventFilter(watched, e);
}

// ── ValueBarDelegate ─────────────────────────────────────────

UiUtil::ValueBarDelegate::ValueBarDelegate(RowHoverTable* table)
    : QStyledItemDelegate(table)
    , m_table(table)
{
}

void UiUtil::ValueBarDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const
{
    const Palette& pal = Theme::instance().pal();
    const double max = m_max.value(index.column(), 0.0);
    const double v = index.data(Qt::UserRole + 1).toDouble();
    const bool hot = m_table && index.row() == m_table->hoverRow();

    if (max <= 0 || v <= 0) { // 非数值列 / 空值：普通绘制
        if (hot)
            p->fillRect(option.rect, pal.surface2);
        QStyledItemDelegate::paint(p, option, index);
        return;
    }

    // 背景（含斑马纹与选中）→ 悬停高亮 → 比例条 → 数字，分三层画，
    // 否则底层的斑马纹会把比例条盖掉
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();
    QStyle* style = option.widget ? option.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, option.widget);
    if (hot)
        p->fillRect(option.rect, pal.surface2);

    const int w = qMax(2, int(double(option.rect.width())
                              * qBound(0.0, v / max, 1.0)));
    QColor bar = pal.accent;
    bar.setAlpha(30);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    p->setBrush(bar);
    p->drawRoundedRect(QRect(option.rect.left(), option.rect.top() + 4, w,
                             option.rect.height() - 8), 3, 3);
    p->restore();

    QColor fg = index.data(Qt::ForegroundRole).value<QBrush>().color();
    if (!fg.isValid())
        fg = pal.fg1;
    p->save();
    p->setFont(option.font);
    p->setPen(fg);
    p->drawText(option.rect.adjusted(8, 0, -10, 0),
                Qt::AlignRight | Qt::AlignVCenter, text);
    p->restore();
}

// ── JSON 语法着色 ────────────────────────────────────────────

QString UiUtil::jsonHighlightHtml(const QString& prettyJson, const Palette& pal)
{
    QString body = prettyJson.toHtmlEscaped();
    // NOTE: C++ 字符串里 regex 转义必须双写（\\s、\\d、\\[），replacement
    // 里的反向引用也是 \\1 形式；单写会被编译器吞成无效转义
    body.replace(QRegularExpression(QStringLiteral("(\\\")([^\"]*)(\\\")(:)")),
                 QStringLiteral("<span style='color:%1'>\\1\\2\\3</span>\\4")
                     .arg(pal.accent.name()));
    body.replace(QRegularExpression(QStringLiteral("(:\\s*)(\\\"[^\"]*\\\")")),
                 QStringLiteral("\\1<span style='color:%1'>\\2</span>")
                     .arg(pal.catUsage.name()));
    body.replace(QRegularExpression(QStringLiteral("(:\\s*)(-?[0-9]+\\.?[0-9]*)")),
                 QStringLiteral("\\1<span style='color:%1'>\\2</span>")
                     .arg(pal.sevWarn.name()));
    body.replace(QRegularExpression(QStringLiteral("(:\\s*)(true|false|null)")),
                 QStringLiteral("\\1<span style='color:%1'>\\2</span>")
                     .arg(pal.accent2.name()));
    body.replace(QRegularExpression(QStringLiteral("([{}\\[\\],])")),
                 QStringLiteral("<span style='color:%1'>\\1</span>")
                     .arg(pal.fg4.name()));
    return body;
}

int UiUtil::tableContentHeight(QTableWidget* t)
{
    int h = t->horizontalHeader()->height();
    for (int r = 0; r < t->rowCount(); ++r)
        h += t->rowHeight(r);
    h += 2 * t->frameWidth() + 1;
    return h;
}

int UiUtil::tableContentHeight(QTableWidget* t, int maxHeight)
{
    return qMin(tableContentHeight(t), maxHeight);
}
