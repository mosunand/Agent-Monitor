#pragma once
// UsageHeatmap.h — GitHub-style token activity calendar: 7 weekday rows ×
// ~53 week columns, blue intensity ramp. Modes: 每日 / 每周 / 累计.

#include <QDate>
#include <QHash>
#include <QWidget>

class UsageHeatmap : public QWidget {
    Q_OBJECT
public:
    enum Mode { Daily, Weekly, Cumulative };

    explicit UsageHeatmap(QWidget* parent = nullptr);

    // daily = local-midnight-aligned (dayMs, tokens), ascending
    void setData(const QVector<QPair<qint64, qint64>>& daily);
    void setMode(Mode mode);

    QSize minimumSizeHint() const override { return QSize(560, 150); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Cell {
        qint64 dayMs = 0;
        qint64 value = 0;
        bool future = false;
    };
    void rebuildGrid();

    QVector<QPair<qint64, qint64>> m_daily;
    QHash<qint64, qint64> m_byDay;
    Mode m_mode = Daily;
    QVector<QVector<Cell>> m_grid;   // [col][row=weekday 0=Mon]
    QDate m_gridStart;               // Monday of the first column
    int m_cols = 0;
    // 分级基准：活跃日值的升序序列。按序号分位定级（GitHub 风格），
    // 与绝对量级无关 —— 若按最大值归一化，只有接近历史峰值的那几天
    // 会全亮，典型活跃日全都偏淡
    QVector<qint64> m_activeSorted;
    int m_hoverCol = -1, m_hoverRow = -1;
    int m_colStep = 0, m_rowStep = 0, m_cellW = 0, m_cellH = 0; // paint geometry, for hit-testing
};
