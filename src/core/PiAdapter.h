#pragma once
// PiAdapter.h — pi coding agent (Mario Zechner / badlogic pi-mono).
//
// Data shape per the official docs (packages/coding-agent/docs/session-format.md):
//   ~/.pi/agent/sessions/<cwd-derived>/<timestamp>_<uuid>.jsonl  (JSONL tree)
//   line types: session / message / model_change / compaction / custom / branch
//   session header: {type:"session", id, timestamp, cwd, version, parent}
//   assistant messages carry model + provider and a usage object
//   (usage.input / output / cacheRead / cacheWrite per pi-ai's unified API)
//
// NOTE: implemented from the documented format — no local pi install was
// available to verify against; parsing is defensive (unknown lines skipped)
// and the adapter degrades to "未检测到数据" when the directory is absent.

#include <QHash>

#include "AgentAdapter.h"

class PiAdapter final : public AgentAdapter {
public:
    QString id() const override { return QStringLiteral("pi"); }
    QString displayName() const override { return QStringLiteral("pi (badlogic)"); }
    QColor color() const override;
    bool dataPresent(QString* dir = nullptr) const override;
    QVector<SessionFile> listSessionFiles(QString* note = nullptr) const override;
    bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                   QVector<types::UsageRecord>& outRecords,
                   QHash<QString, ToolStat>* outTools = nullptr) const override;
};
