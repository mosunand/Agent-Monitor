#pragma once
// UsageStatsPage.h — 使用统计: lifetime stats cards, token-activity heatmap,
// daily per-model trend (7/30 days), model-share donut.

#include <QVariantAnimation>
#include <QWidget>

#include "core/Store.h"
#include "core/Types.h"

class QButtonGroup;
class QComboBox;
class KpiCard;
class MiniChart;
class QTableWidget;
class UsageHeatmap;
class UsageDonut;
namespace UiUtil {
class RowHoverTable;
}

class UsageStatsPage : public QWidget {
    Q_OBJECT
public:
    explicit UsageStatsPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private slots:
    void reload();
    void onModeChanged();
    void onWindowChanged();

private:
    void buildUi();
    void refreshFilters(); // rebuild agent/model combo options from data
    void renderData();
    void renderHeatmap();
    void renderTrend();
    void renderDonut();
    void renderAgentTable();

    QComboBox* m_filterAgent = nullptr;
    QComboBox* m_filterModel = nullptr;
    KpiCard* m_kTotal = nullptr;
    KpiCard* m_kPeak = nullptr;
    KpiCard* m_kLongest = nullptr;
    KpiCard* m_kStreak = nullptr;
    KpiCard* m_kMaxStreak = nullptr;

    UsageHeatmap* m_heatmap = nullptr;
    MiniChart* m_trend = nullptr;
    UsageDonut* m_donut = nullptr;
    QTableWidget* m_legendTable = nullptr;
    UiUtil::RowHoverTable* m_agentTable = nullptr;

    QButtonGroup* m_modeGroup = nullptr;
    QButtonGroup* m_windowGroup = nullptr;

    QVector<Store::DailyTotal> m_daily;
    QVector<Store::ModelDayPoint> m_byModel30;
    QVector<Store::AgentTotal> m_byAgent;
    qint64 m_longestSessionMs = 0;
    bool m_loaded = false;
    int m_reloadSeq = 0;

    // KPI count-up：首次进入页面数字从 0 滚到目标值
    QVariantAnimation m_countAnim;
    double m_countProg = 1.0;
    bool m_counted = false;
    qint64 m_tTotal = 0, m_tPeak = 0, m_tLongest = 0;
    int m_tStreak = 0, m_tMaxStreak = 0;
    void updateKpiValues();
};
