#pragma once
// KpiCard.h — the .kpi card: uppercase label, big value, delta line, optional
// 3px progress bar. (Ported from zcode-monitor.)

#include <QFrame>
#include <QString>
#include <QVariantAnimation>

class QLabel;
class QVBoxLayout;

class KpiCard : public QFrame {
    Q_OBJECT
public:
    explicit KpiCard(const QString& labelText, QWidget* parent = nullptr);

    void setValue(const QString& text, const QColor& color = QColor());
    void setDelta(const QString& text);
    void setDetails(const QString& html);
    void setLargeValue(bool on);
    void setCaption(const QString& text);
    void setBar(int pct, const QColor& color);
    // 数字变化时平滑滚动到新值（识别「数字+单位」文本；非数字文本直接落定）
    void setCountUpEnabled(bool on) { m_countUp = on; }
    // modern stats-card layout: big value on top, muted caption below
    void setValueFirst(bool on);
    void setFlashEnabled(bool on) { m_flashEnabled = on; }

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QLabel* m_label;
    QLabel* m_value;
    QLabel* m_delta;
    QLabel* m_details = nullptr;
    int m_barPct = -1;
    QColor m_barColor;
    bool m_largeValue = false;
    bool m_valueFirst = false;
    QVBoxLayout* m_lay = nullptr;
    QVariantAnimation m_flash; // 数值变化时的主题色微光
    bool m_flashEnabled = true;
    QVariantAnimation m_countAnim;
    QString m_targetText;   // 目标文本
    double m_fromNum = 0, m_toNum = 0;
    QString m_prefix, m_suffix; // 数字前后的非数字部分
    int m_decimals = 0;
    bool m_grouped = false;     // 目标数字带千分位（动画帧保持同格式）
    bool m_countUp = true;
    double m_flashA = 0;
};
