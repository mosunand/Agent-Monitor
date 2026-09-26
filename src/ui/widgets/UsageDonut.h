#pragma once
// UsageDonut.h — token-share donut (chart.js style): thick ring segments
// starting at 12 o'clock, center total.

#include <QColor>
#include <QString>
#include <QVariantAnimation>
#include <QVector>
#include <QWidget>

class UsageDonut : public QWidget {
    Q_OBJECT
public:
    struct Slice {
        QString label;
        qint64 value = 0;
        QColor color;
    };

    explicit UsageDonut(QWidget* parent = nullptr);

    void setData(const QVector<Slice>& slices);
    void setCenterSuffix(const QString& suffix); // default "tokens"
    // replay the sweep-in animation (call when the page becomes visible)
    void animate();

    QSize sizeHint() const override { return QSize(300, 300); }
    QSize minimumSizeHint() const override { return QSize(240, 240); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRect ringRect() const; // ellipse path rect (stroke centered on it)
    int penWidth() const;

    QVector<Slice> m_slices;
    QString m_suffix = QStringLiteral("tokens");
    double m_progress = 1.0;          // sweep animation 0→1
    QVariantAnimation m_anim;
    int m_hoverSlice = -1;
};
