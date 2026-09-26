#pragma once
// AgentAdapter.h — the plugin interface every monitored agent implements.
// An adapter knows where the agent writes its local session files, how to
// list them cheaply (mtime/size probe) and how to normalize one file into
// the unified types::SessionInfo + types::UsageRecord model.
//
// Adding an agent = subclass + register in AgentRegistry::createAdapters().

#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include "Types.h"

struct SessionFile {
    QString path;
    qint64 mtimeMs = 0;
    qint64 size = 0;
};

// per-tool aggregate for one session: call count + first/last occurrence —
// the time window lets the SQL filter tools by the selected usage window
struct ToolStat {
    qint64 calls = 0;
    qint64 firstMs = 0;
    qint64 lastMs = 0;
};

class AgentAdapter {
public:
    virtual ~AgentAdapter() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual QColor color() const = 0;

    // data directory present? (out = the directory checked)
    virtual bool dataPresent(QString* dir = nullptr) const = 0;

    // cheap directory walk: file list + mtime/size, no content parsing
    virtual QVector<SessionFile> listSessionFiles(QString* note = nullptr) const = 0;

    // parse one file into the unified model. Returns false when the file
    // could not be read at all; malformed lines are skipped defensively.
    // outTools (optional): tool name → call count for this session.
    virtual bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                           QVector<types::UsageRecord>& outRecords,
                           QHash<QString, ToolStat>* outTools = nullptr) const = 0;

    // conversation replay — parsed ON DEMAND from the source file (never
    // stored in the cache db). Default: not supported by this adapter.
    virtual bool parseMessages(const SessionFile& f,
                               QVector<types::ChatMessage>& outMessages) const
    {
        Q_UNUSED(f);
        Q_UNUSED(outMessages);
        return false;
    }

    // 最长聊天时长 in the agent's own official semantics (ZCode: Σ completed
    // turn durations; file-based agents: no dedicated source → -1, the
    // archive's per-session stream-time sum is used instead).
    // familyLike: optional lowercase "family-%" LIKE pattern for model filter.
    virtual qint64 longestChatMs(const QString& familyLike = QString()) const
    {
        Q_UNUSED(familyLike);
        return -1;
    }

    types::AgentStatus status() const;
};

namespace AgentRegistry {

// all built-in adapters, in display order
QVector<AgentAdapter*> createAdapters();

} // namespace AgentRegistry
