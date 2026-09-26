#pragma once
// CodexAdapter.h — OpenAI Codex CLI/Desktop (~/.codex/sessions rollouts).
//
// Rollout JSONL (one file per session, event per line, top-level timestamp):
//   session_meta      id / cwd / originator / cli_version / context_window
//   turn_context      per-turn model + reasoning effort (turn_id keyed)
//   token_usage_record (new format)  per-response usage + response_id
//   event_msg:token_count (both)     cumulative + per-response usage snapshots
//   response_item     message/function_call/reasoning/... items
//
// TPS note: codex does not log per-token timings; per-response duration is
// approximated from the gap to the previous response/turn start (includes
// tool gaps → durApprox = true, UI renders "≈").

#include <QHash>
#include <QPair>

#include "AgentAdapter.h"

class CodexAdapter final : public AgentAdapter {
public:
    QString id() const override { return QStringLiteral("codex"); }
    QString displayName() const override { return QStringLiteral("Codex (OpenAI)"); }
    QColor color() const override;
    bool dataPresent(QString* dir = nullptr) const override;
    QVector<SessionFile> listSessionFiles(QString* note = nullptr) const override;
    bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                   QVector<types::UsageRecord>& outRecords,
                   QHash<QString, ToolStat>* outTools = nullptr) const override;
    bool parseMessages(const SessionFile& f,
                       QVector<types::ChatMessage>& outMessages) const override;

private:
    // session id from the rollout filename (trailing 5 dash segments)
    static QString sessionIdFromPath(const QString& path);
};
