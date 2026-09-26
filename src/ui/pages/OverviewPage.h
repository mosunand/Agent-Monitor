#pragma once
// OverviewPage.h — the live dashboard: KPI cards, trend charts, speed table,
// live activity feed, per-model / per-agent distribution.

#include <QWidget>

#include "core/Types.h"

class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QTableWidget;
class QVBoxLayout;
class BadgeLabel;
class KpiCard;
class MiniChart;
class LiveFeed;
namespace UiUtil {
class RowHoverTable;
}
class OverviewPage : public QWidget {
    Q_OBJECT
public:
    explicit OverviewPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private slots:
    void reload();
    void onSpeedFilterChanged();
    void onDataChanged();
    void onNewRecords(const QVector<types::UsageRecord>& records);
    void onScanFinished(bool ok, const QString& err, int changed, int total);

private:
    void buildUi();
    void renderData();
    void renderCharts();
    void renderTotalCard(KpiCard* card, const types::OverviewData& d);

    QComboBox* m_windowCombo = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QLabel* m_seriesRange = nullptr;
    BadgeLabel* m_liveStatus = nullptr;

    QGridLayout* m_kpiGrid = nullptr;
    KpiCard* m_kTodayTotal = nullptr;
    KpiCard* m_kRequests = nullptr;
    KpiCard* m_kAvgDur = nullptr;
    KpiCard* m_kIn = nullptr;
    KpiCard* m_kOut = nullptr;
    KpiCard* m_kTotal = nullptr;
    KpiCard* m_kReason = nullptr;
    KpiCard* m_kCache = nullptr;
    KpiCard* m_kSessions = nullptr;
    KpiCard* m_kSpeed = nullptr;
    KpiCard* m_kErrors = nullptr;

    MiniChart* m_chCalls = nullptr;
    MiniChart* m_chTokens = nullptr;
    MiniChart* m_chSpeed = nullptr;

    QTableWidget* m_speedTable = nullptr;
    QWidget* m_speedFoot = nullptr;
    UiUtil::RowHoverTable* m_byModelTable = nullptr;
    QTableWidget* m_byToolTable = nullptr;
    UiUtil::RowHoverTable* m_byAgentTable = nullptr;
    LiveFeed* m_feed = nullptr;

    types::OverviewData m_data;
    bool m_loaded = false;
    int m_reloadSeq = 0;
    QComboBox* m_speedModelFilter = nullptr; // 速度表模型大类筛选
};
