#pragma once
// GeminiLikeAdapter.h — Gemini CLI and its forks (Qwen Code) share the same
// session recording layout:
//   <home>/tmp/<projectDir>/chats/  with
//     · current:  JSONL  (gemini: session-<ts>-<uuid>.jsonl, first record =
//       metadata {sessionId, startTime, kind, directories}; message records
//       {timestamp, type: user|gemini, tokens{input,output,cached,thoughts,
//       tool,total}, model}; qwen: <uuid>.jsonl, records {type: assistant,
//       usageMetadata{promptTokenCount, candidatesTokenCount,
//       cachedContentTokenCount, thoughtsTokenCount}, model})
//     · legacy:   session-*.json single JSON {sessionId, startTime,
//       messages:[{type, model, tokens{...}}]}
// One parameterized adapter covers both (id/name/color/dir differ).

#include "AgentAdapter.h"

class GeminiLikeAdapter final : public AgentAdapter {
public:
    GeminiLikeAdapter(const QString& id, const QString& displayName,
                      const QColor& color, const QString& homeDirName,
                      const char* homeEnvVar);

    QString id() const override { return m_id; }
    QString displayName() const override { return m_name; }
    QColor color() const override { return m_color; }
    bool dataPresent(QString* dir = nullptr) const override;
    QVector<SessionFile> listSessionFiles(QString* note = nullptr) const override;
    bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                   QVector<types::UsageRecord>& outRecords,
                   QHash<QString, ToolStat>* outTools = nullptr) const override;

private:
    QString m_id, m_name;
    QColor m_color;
    QString m_homeDirName; // ".gemini" / ".qwen"
    const char* m_homeEnvVar;
};
