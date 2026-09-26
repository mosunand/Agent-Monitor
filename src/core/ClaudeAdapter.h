#pragma once
// ClaudeAdapter.h — Anthropic Claude Code (~/.claude/projects/**/*.jsonl).
//
// One JSONL per session (one event per line, top-level "timestamp" UTC):
//   type=user/assistant/system/ai-title/last-prompt/file-history-* ...
//   assistant.message = { id, model, usage{ input_tokens,
//       cache_creation_input_tokens, cache_read_input_tokens, output_tokens } }
//
// A streamed response arrives as several assistant lines sharing message.id
// (usage cumulative) → grouped by id: usage from the last line, streaming
// duration = last − first line → exact per-response TPS.

#include <QHash>

#include "AgentAdapter.h"

class ClaudeAdapter final : public AgentAdapter {
public:
    QString id() const override { return QStringLiteral("claude"); }
    QString displayName() const override { return QStringLiteral("Claude Code"); }
    QColor color() const override;
    bool dataPresent(QString* dir = nullptr) const override;
    QVector<SessionFile> listSessionFiles(QString* note = nullptr) const override;
    bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                   QVector<types::UsageRecord>& outRecords,
                   QHash<QString, ToolStat>* outTools = nullptr) const override;
    bool parseMessages(const SessionFile& f,
                       QVector<types::ChatMessage>& outMessages) const override;

private:
    // first user text of the session (string or content-array form)
    static QString userText(const QJsonObject& message);
};
