#pragma once
// OpenCodeAdapter.h — OpenCode v2 (sst/opencode): sessions/messages live in a
// WAL SQLite db (opencode.db); token data is inside the message `data` JSON
// blob: { role, tokens{input,output,reasoning,cache{read,write}},
//         time{created,completed}, model{id,providerID,variant}, ... }
// Legacy JSON-tree installs and older db generations are probed defensively.

#include <QHash>

#include "AgentAdapter.h"

class OpenCodeAdapter final : public AgentAdapter {
public:
    QString id() const override { return QStringLiteral("opencode"); }
    QString displayName() const override { return QStringLiteral("OpenCode"); }
    QColor color() const override;
    bool dataPresent(QString* dir = nullptr) const override;
    QVector<SessionFile> listSessionFiles(QString* note = nullptr) const override;
    bool parseFile(const SessionFile& f, types::SessionInfo& outMeta,
                   QVector<types::UsageRecord>& outRecords,
                   QHash<QString, ToolStat>* outTools = nullptr) const override;

private:
    static QString sessionIdFromPseudoPath(const QString& path);
};
