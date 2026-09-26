#pragma once
// SessionsPage.h — unified session browser across all agents: left list
// (agent filter / search / sort) + right detail (概览 / 明细 / JSON tabs).

#include <QWidget>

#include "core/Types.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class MiniChart;
class UsageTableModel;

class SessionsPage : public QWidget {
    Q_OBJECT
public:
    explicit SessionsPage(QWidget* parent = nullptr);

    // deep link (e.g. from other pages)
    void openSession(const QString& agent, const QString& sessionId);

signals:
    void openSessionRequested(const QString& agent, const QString& sessionId);

protected:
    void showEvent(QShowEvent* e) override;

private slots:
    void rebuildList();
    void onSelectionChanged();
    void onDataChanged();
    void onTabChanged(int index);

private:
    void buildUi();
    void loadDetail(const QString& agent, const QString& sessionId);
    void renderDetail();
    void buildRespTable();   // 明细标签页（懒加载）
    void buildJsonView();    // JSON 标签页（懒加载）
    void loadChat();               // lazy: only when the 对话 tab is shown
    QString renderChatHtml() const;

    QComboBox* m_agentCombo = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_sortCombo = nullptr;
    QListWidget* m_list = nullptr;

    QLabel* m_detailTitle = nullptr;
    QLabel* m_detailSub = nullptr;
    QLabel* m_detailStats = nullptr;
    QLabel* m_detailEmpty = nullptr;
    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_infoTable = nullptr;
    MiniChart* m_tpsChart = nullptr;
    QTableView* m_respView = nullptr;      // 明细：虚拟化表格
    UsageTableModel* m_respModel = nullptr;
    QTextBrowser* m_chatView = nullptr;
    QTextBrowser* m_jsonView = nullptr;

    types::SessionInfo m_info;
    QVector<types::UsageRecord> m_records;
    bool m_infoLoaded = false;
    QString m_currentAgent, m_currentSession;
    int m_loadSeq = 0;   // detail loads
    int m_listSeq = 0;   // list rebuilds — independent, so a detail
                         // load can't drop a pending list update
    QString m_listFingerprint; // 列表内容指纹：未变则跳过整表重建
    // conversation replay cache (one lazy load per selected session)
    QVector<types::ChatMessage> m_chat;
    bool m_chatLoaded = false;
    bool m_chatSupported = false;
    int m_chatSeq = 0;
    // 明细 / JSON 标签页同样懒加载 —— 千级响应的会话，全量明细表
    // (上万个 setItem) + JSON 着色一次要几十到几百毫秒
    bool m_respLoaded = false;
    bool m_jsonLoaded = false;
};
