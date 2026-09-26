#pragma once
// ZCodeAdapter.h — ZCode (~/.zcode/cli/db/db.sqlite).
//
// Unlike the file-based agents, ZCode writes everything into a WAL SQLite
// database (session / model_usage / tool_usage tables, timestamps = epoch ms).
// The adapter maps it onto the file-oriented interface as one pseudo-file per
// session (path = "<db>|<sessionId>", mtime = session.time_updated), so the
// incremental scanner only re-reads sessions that actually changed.
//
// The source database is opened READ-ONLY (query_only + busy ladder, per-
// thread connection) — ZCode keeps writing while we watch; we never checkpoint
// or modify anything.
//
// Conversation replay reads the message/part tables on demand: message.data
// carries role/modelID/semantics, part.data is the content block (text /
// reasoning / tool). Hidden runtime injections (todo reminders, compaction
// summaries…) are skipped per ZCode's own uiVisibility semantics.

#include <QHash>

#include "AgentAdapter.h"

class ZCodeAdapter final : public AgentAdapter {
public:
    QString id() const override { return QStringLiteral("zcode"); }
    QString displayName() const override { return QStringLiteral("ZCode"); }
    QColor color() const override;
    bool dataPresent(QString* dir = nullptr) const override;
    QVector<SessionFile> listSessionFiles(QString* note = nullptr) const override;
    bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                   QVector<types::UsageRecord>& outRecords,
                   QHash<QString, ToolStat>* outTools = nullptr) const override;
    bool parseMessages(const SessionFile& f,
                       QVector<types::ChatMessage>& outMessages) const override;
    qint64 longestChatMs(const QString& familyLike = QString()) const override;

private:
    static QString sessionIdFromPseudoPath(const QString& path);
};
