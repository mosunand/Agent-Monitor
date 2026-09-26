#pragma once
// ModelsPage.h — per-model usage across all agents: aggregate table +
// daily generated-token trend per top models.

#include <QWidget>

#include "core/Store.h"
#include "core/Types.h"

class QComboBox;
class QLabel;
class QTableWidget;
class MiniChart;
namespace UiUtil {
class RowHoverTable;
}

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private slots:
    void reload();

private:
    void buildUi();
    void renderData();
    void renderChart();

    QComboBox* m_windowCombo = nullptr;
    UiUtil::RowHoverTable* m_table = nullptr;
    MiniChart* m_chart = nullptr;
    QLabel* m_sub = nullptr;

    QVector<types::ModelBreakdown> m_byModel;
    QVector<Store::ModelDayPoint> m_daily;
    bool m_loaded = false;
    int m_reloadSeq = 0;
};
