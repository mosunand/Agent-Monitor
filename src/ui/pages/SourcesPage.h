#pragma once
// SourcesPage.h — which agents were detected (data dirs, session counts) +
// documentation: data source table, TPS semantics, how to add an adapter.

#include <QWidget>

class QTableWidget;

class SourcesPage : public QWidget {
    Q_OBJECT
public:
    explicit SourcesPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void buildUi();
    void reloadStatus();

    QTableWidget* m_statusTable = nullptr;
};
