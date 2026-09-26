#pragma once
// RawPage.h — browser over the unified response records (the SQLite cache):
// filter by agent / model, click a row for its full JSON.

#include <QWidget>

#include "core/Types.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;

class RawPage : public QWidget {
    Q_OBJECT
public:
    explicit RawPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private slots:
    void runQuery();

private:
    void buildUi();
    void showRowJson(int row);

    QComboBox* m_agentCombo = nullptr;
    QLineEdit* m_modelEdit = nullptr;
    QLabel* m_count = nullptr;
    QTableWidget* m_table = nullptr;
    QVector<types::UsageRecord> m_rows;
};
