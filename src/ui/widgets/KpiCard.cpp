// KpiCard.cpp — see KpiCard.h.

#include "KpiCard.h"

#include <QLabel>
#include <QLocale>
#include <QRegularExpression>

#include "util/Format.h"
#include <QPainter>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include "ui/Theme.h"

KpiCard::KpiCard(const QString& labelText, QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("card"));
    m_lay = new QVBoxLayout(this);
    m_lay->setContentsMargins(15, 13, 15, 15);
    m_lay->setSpacing(3);

    m_label = new QLabel(labelText.toUpper());
    m_label->setProperty("cls", "kpi-label");
    m_value = new QLabel(QStringLiteral("—"));
    m_value->setProperty("cls", "value-fg");
    // 等宽表格数字（tnum）：count-up 滚动时每位数字宽度一致，不左右抖。
    // 字体不支持该 OpenType 特性时无副作用
    QFont valueFont = m_value->font();
    valueFont.setFeature(QFont::Tag("tnum"), 1u);
    m_value->setFont(valueFont);
    m_delta = new QLabel();
    m_delta->setProperty("cls", "kpi-delta");
    m_delta->setWordWrap(false);
    m_details = new QLabel();
    m_details->setProperty("cls", "kpi-delta");
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    m_details->hide();

    m_lay->addWidget(m_label);
    m_lay->addSpacing(2);
    m_lay->addWidget(m_value);
    m_lay->addSpacing(1);
    m_lay->addWidget(m_delta);
    m_lay->addWidget(m_details);
    m_lay->addStretch(1);
    setMinimumHeight(100);
    setMinimumWidth(178);

    // 数字滚动：识别「前缀+数字+后缀」，值变化时 450ms 平滑计数
    m_countAnim.setDuration(450);
    m_countAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_countAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
                if (m_prefix.isEmpty() && m_suffix.isEmpty()
                    && m_targetText.isEmpty())
                    return;
                const double n = m_fromNum + (m_toNum - m_fromNum) * v.toDouble();
                QString body;
                if (m_decimals > 0)
                    body = QString::number(n, 'f', m_decimals);
                else if (m_grouped)
                    body = QLocale().toString(qint64(n)); // 保持千分位格式一致
                else
                    body = Format::fmtNumCn(n);
                m_value->setText(m_prefix + body + m_suffix);
            });

    // 数值变化闪光：实时刷新时卡片泛起一次主题色微光
    m_flash.setDuration(500);
    m_flash.setStartValue(1.0);
    m_flash.setEndValue(0.0);
    connect(&m_flash, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
                m_flashA = v.toDouble();
                update();
            });
}

void KpiCard::setValue(const QString& text, const QColor& color)
{
    const bool changed = m_value->text() != text && text != m_targetText;
    // count-up：拆解「前缀+数字+后缀」；两次都是纯数字形态才滚动。
    // 数字部分允许千分位逗号（fmtInt 输出 "1,817"）——否则 "9,999"→"10,000"
    // 会被解析成 9→10，中间帧显示 "9,000" 这种错数
    if (m_countUp && changed && !text.isEmpty()) {
        static const QRegularExpression re(
            QStringLiteral("^([^0-9.,-]*)(-?[0-9][0-9,]*(?:\\.[0-9]+)?)(.*)$"));
        const auto mNew = re.match(text);
        const auto mOld = re.match(m_value->text());
        const bool newOk = mNew.hasMatch();
        const bool oldOk = mOld.hasMatch();
        // 万/亿 缩写后缀保持原样显示（fmtNumCn 输出带单位）
        if (newOk && (oldOk || m_value->text() == QStringLiteral("—"))) {
            m_targetText = text;
            m_prefix = mNew.captured(1);
            m_suffix = mNew.captured(3);
            m_toNum = mNew.captured(2).remove(QLatin1Char(',')).toDouble();
            m_fromNum = oldOk
                ? mOld.captured(2).remove(QLatin1Char(',')).toDouble() : 0.0;
            m_decimals = mNew.captured(2).contains(QLatin1Char('.'))
                ? 1 : 0;
            m_grouped = mNew.captured(2).contains(QLatin1Char(','));
            // SAFETY: put the FINAL text up immediately — the animation
            // then overwrites frames between old and new numbers. If any
            // animation tick is lost (screenshot grabs, blocked loops), the
            // correct value is already on screen and can never be eaten.
            m_value->setText(text);
            m_countAnim.stop();
            m_countAnim.start();
        } else {
            m_targetText = text;
            m_prefix = m_suffix = QString();
            m_value->setText(text);
        }
    } else {
        m_value->setText(text);
    }
    const int pt = m_largeValue ? 26 : 19;
    m_value->setStyleSheet(color.isValid()
        ? QStringLiteral("font-size:%1pt;font-weight:%2;color:%3;")
              .arg(pt).arg(m_largeValue ? 700 : 600).arg(color.name())
        : QString());
    if (m_flashEnabled && changed && !text.isEmpty() && text != QStringLiteral("—")) {
        m_flash.stop();
        m_flash.start();
    }
}

void KpiCard::setDelta(const QString& text)
{
    m_delta->setText(text);
}

void KpiCard::setLargeValue(bool on)
{
    if (m_largeValue == on)
        return;
    m_largeValue = on;
    if (on) {
        // re-apply the large style directly on the CURRENT text (which may
        // be a mid-animation value) — going through setValue would restart
        // the count-up from it
        const int pt = 26;
        m_value->setStyleSheet(
            QStringLiteral("font-size:%1pt;font-weight:700;").arg(pt));
    } else {
        m_value->setStyleSheet(QString());
    }
}

void KpiCard::setDetails(const QString& html)
{
    m_details->setText(html);
    m_details->setVisible(!html.isEmpty());
}

void KpiCard::setCaption(const QString& text)
{
    m_label->setText(text.toUpper());
}

void KpiCard::setBar(int pct, const QColor& color)
{
    m_barPct = pct;
    m_barColor = color;
    update();
}

void KpiCard::setValueFirst(bool on)
{
    if (m_valueFirst == on)
        return;
    m_valueFirst = on;
    // reorder the top of the card: value above the muted caption
    m_lay->removeWidget(m_label);
    m_lay->removeWidget(m_value);
    if (on) {
        m_lay->insertWidget(0, m_value);
        m_lay->insertWidget(1, m_label);
    } else {
        m_lay->insertWidget(0, m_label);
        m_lay->insertWidget(1, m_value);
    }
}

void KpiCard::paintEvent(QPaintEvent* e)
{
    QFrame::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // 左侧主题色竖条：hover 时提亮（QLabel 不透传 hover，用 underMouse）
    QColor bar = Theme::instance().pal().accent;
    bar.setAlpha(underMouse() ? 255 : 170);
    p.setPen(Qt::NoPen);
    p.setBrush(bar);
    p.drawRoundedRect(QRectF(8, 16, 3, height() - 32), 1.5, 1.5);

    if (m_barPct >= 0 && m_barColor.isValid()) {
        const QRect track(15, height() - 12, width() - 30, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::instance().pal().surface3);
        p.drawRoundedRect(track, 1.5, 1.5);
        if (m_barPct > 0) {
            p.setBrush(m_barColor);
            QRect fill = track;
            fill.setWidth(qMax(3, int(track.width() * qMin(100, m_barPct)) / 100));
            p.drawRoundedRect(fill, 1.5, 1.5);
        }
    }
    // 数值变化微光
    if (m_flashA > 0.01) {
        QColor f = Theme::instance().pal().accent;
        f.setAlpha(int(m_flashA * 36));
        p.setPen(Qt::NoPen);
        p.setBrush(f);
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);
    }
}
