#pragma once
// LiveFeed.h — the "实时活动" wire-style feed (adapted from zcode-monitor to
// the unified multi-agent model). QListView + custom delegate; newest first,
// capped at 60 rows, 1.2s flash on new rows.

#include <QAbstractListModel>
#include <QListView>
#include <QStyledItemDelegate>
#include <QTimer>

#include "core/Types.h"

class QLabel;

struct FeedRow {
    qint64 seq = 0;
    qint64 timeMs = 0;
    QString agentId;
    QString agentName;
    QColor agentColor;
    QString model;
    QString effort;
    QString summary;
    bool durOk = false;
    bool durApprox = false;
    qint64 durMs = 0;
    bool tpsOk = false;
    double tps = 0;
    qint64 addedAtMs = 0;
};

class LiveFeedModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit LiveFeedModel(QObject* parent = nullptr);

    void pushRecords(const QVector<types::UsageRecord>& rows);
    void clear();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    const FeedRow& rowAt(int i) const { return m_rows.at(i); }

private:
    QVector<FeedRow> m_rows;
    qint64 m_seq = 0;
};

class LiveFeedDelegate : public QStyledItemDelegate {
public:
    explicit LiveFeedDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

class LiveFeed : public QListView {
    Q_OBJECT
public:
    explicit LiveFeed(QWidget* parent = nullptr);
    LiveFeedModel* feedModel() const { return m_model; }

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    LiveFeedModel* m_model;
    QTimer m_flashTimer; // keeps repainting while a row is still "hot"
    QWidget* m_emptyLabel = nullptr;
};
