// GeminiLikeAdapter.cpp — see GeminiLikeAdapter.h.

#include "GeminiLikeAdapter.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QtGlobal>

namespace {

constexpr qint64 kLineCap = 4 * 1024 * 1024;

qint64 isoToMs(const QString& s)
{
    if (s.isEmpty())
        return 0;
    const QDateTime d = QDateTime::fromString(s, Qt::ISODateWithMs);
    return d.isValid() ? d.toMSecsSinceEpoch() : 0;
}

// gemini TokensSummary: {input, output, cached, thoughts, tool, total}
// qwen usageMetadata:  {promptTokenCount, candidatesTokenCount,
//                       cachedContentTokenCount, thoughtsTokenCount}
void readTokens(const QJsonObject& o, types::UsageRecord& r)
{
    QJsonObject t = o.value(QStringLiteral("tokens")).toObject();
    if (t.isEmpty())
        t = o.value(QStringLiteral("usageMetadata")).toObject();
    if (t.isEmpty())
        return;
    if (t.contains(QStringLiteral("input"))) { // gemini TokensSummary
        r.inputTokens = t.value(QStringLiteral("input")).toVariant().toLongLong();
        r.outputTokens = t.value(QStringLiteral("output")).toVariant().toLongLong();
        r.cachedTokens = t.value(QStringLiteral("cached")).toVariant().toLongLong();
        r.reasoningTokens = t.value(QStringLiteral("thoughts")).toVariant().toLongLong();
    } else { // gemini-sdk GenerateContentResponseUsageMetadata (qwen)
        r.inputTokens = t.value(QStringLiteral("promptTokenCount")).toVariant().toLongLong();
        r.outputTokens = t.value(QStringLiteral("candidatesTokenCount")).toVariant().toLongLong();
        r.cachedTokens = t.value(QStringLiteral("cachedContentTokenCount")).toVariant().toLongLong();
        r.reasoningTokens = t.value(QStringLiteral("thoughtsTokenCount")).toVariant().toLongLong();
    }
}

} // namespace

GeminiLikeAdapter::GeminiLikeAdapter(const QString& id, const QString& displayName,
                                     const QColor& color, const QString& homeDirName,
                                     const char* homeEnvVar)
    : m_id(id)
    , m_name(displayName)
    , m_color(color)
    , m_homeDirName(homeDirName)
    , m_homeEnvVar(homeEnvVar)
{
}

bool GeminiLikeAdapter::dataPresent(QString* dir) const
{
    QString home = QDir::homePath() + QLatin1Char('/') + m_homeDirName;
    if (m_homeEnvVar) {
        const QByteArray v = qgetenv(m_homeEnvVar);
        if (!v.isEmpty())
            home = QDir::cleanPath(QString::fromLocal8Bit(v));
    }
    if (dir)
        *dir = home + QStringLiteral("/tmp");
    return QDir(home + QStringLiteral("/tmp")).exists();
}

QVector<SessionFile> GeminiLikeAdapter::listSessionFiles(QString* note) const
{
    QVector<SessionFile> out;
    QString home = QDir::homePath() + QLatin1Char('/') + m_homeDirName;
    if (m_homeEnvVar) {
        const QByteArray v = qgetenv(m_homeEnvVar);
        if (!v.isEmpty())
            home = QDir::cleanPath(QString::fromLocal8Bit(v));
    }
    const QString chatsGlob = home + QStringLiteral("/tmp/*/chats");
    QDirIterator it(chatsGlob, { QStringLiteral("*.jsonl"), QStringLiteral("*.json") },
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFileInfo info(path);
        SessionFile f;
        f.path = path;
        f.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        f.size = info.size();
        out.push_back(f);
    }
    if (note)
        *note = out.isEmpty()
                    ? QStringLiteral("未发现会话文件（chats/*.jsonl|*.json）")
                    : QString();
    return out;
}

bool GeminiLikeAdapter::parseFile(const SessionFile& f, types::SessionInfo& meta,
                                  QVector<types::UsageRecord>& records,
                                  QHash<QString, ToolStat>* outTools) const
{
    meta = types::SessionInfo{};
    meta.agentId = id();
    meta.filePath = f.path;
    meta.sessionId = QFileInfo(f.path).completeBaseName();

    QFile file(f.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    qint64 lastTs = 0;
    QHash<QString, int> modelCount;
    QString metaDirectories;

    const auto finishModelCount = [&] {
        QString best;
        int bestN = -1;
        for (auto it = modelCount.constBegin(); it != modelCount.constEnd(); ++it)
            if (it.value() > bestN) {
                bestN = it.value();
                best = it.key();
            }
        meta.model = best;
    };
    const auto addRecord = [&](const types::UsageRecord& r) {
        records.push_back(r);
        if (!r.model.isEmpty())
            modelCount[r.model]++;
    };

    const auto readMessage = [&](const QJsonObject& o, const QString& type) {
        if (type != QLatin1String("gemini") && type != QLatin1String("assistant"))
            return;
        types::UsageRecord r;
        r.agentId = id();
        r.sessionId = meta.sessionId;
        r.timeMs = isoToMs(o.value(QStringLiteral("timestamp")).toString());
        r.model = o.value(QStringLiteral("model")).toString();
        r.source = QStringLiteral("main");
        readTokens(o, r);
        if (r.totalTokens() <= 0)
            return;
        addRecord(r);
    };

    if (f.path.endsWith(QLatin1String(".jsonl"), Qt::CaseInsensitive)) {
        // ── current JSONL tree format ──
        while (!file.atEnd()) {
            const QByteArray line = file.readLine(kLineCap);
            if (line.size() <= 1)
                continue;
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (o.isEmpty())
                continue;
            const QString type = o.value(QStringLiteral("type")).toString();
            const qint64 ts = isoToMs(o.value(QStringLiteral("timestamp")).toString());
            if (ts > 0)
                lastTs = qMax(lastTs, ts);

            if (type == QLatin1String("metadata") || o.contains(QStringLiteral("sessionId"))) {
                const QString sid = o.value(QStringLiteral("sessionId")).toString();
                if (!sid.isEmpty())
                    meta.sessionId = sid;
                if (meta.startedMs <= 0)
                    meta.startedMs = ts > 0 ? ts
                        : isoToMs(o.value(QStringLiteral("startTime")).toString());
                const QJsonArray dirs = o.value(QStringLiteral("directories")).toArray();
                if (!dirs.isEmpty())
                    metaDirectories = dirs.first().toString();
                if (o.value(QStringLiteral("kind")).toString() == QLatin1String("subagent"))
                    meta.originator = QStringLiteral("subagent");
            }
            readMessage(o, type);
        }
    } else {
        // ── legacy single-JSON: {sessionId, startTime, messages:[...]} ──
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QString sid = root.value(QStringLiteral("sessionId")).toString();
        if (!sid.isEmpty())
            meta.sessionId = sid;
        meta.startedMs = isoToMs(root.value(QStringLiteral("startTime")).toString());
        const QJsonArray msgs = root.value(QStringLiteral("messages")).toArray();
        for (const auto& v : msgs) {
            const QJsonObject o = v.toObject();
            const QString type = o.value(QStringLiteral("type")).toString();
            const qint64 ts = isoToMs(o.value(QStringLiteral("timestamp")).toString());
            if (ts > 0)
                lastTs = qMax(lastTs, ts);
            readMessage(o, type);
        }
    }

    // aggregates
    meta.updatedMs = qMax(meta.startedMs, lastTs);
    if (!metaDirectories.isEmpty())
        meta.cwd = metaDirectories;
    if (!meta.cwd.isEmpty())
        meta.title = QFileInfo(meta.cwd).fileName();
    if (meta.title.isEmpty())
        meta.title = meta.sessionId.left(12);
    finishModelCount();
    for (const types::UsageRecord& r : records) {
        if (!r.error.isEmpty()) {
            meta.errors++;
            continue;
        }
        meta.inputTokens += r.inputTokens;
        meta.cachedTokens += r.cachedTokens;
        meta.cacheWriteTokens += r.cacheWriteTokens;
        meta.outputTokens += r.outputTokens;
        meta.reasoningTokens += r.reasoningTokens;
    }
    meta.responses = records.size();
    Q_UNUSED(outTools); // gemini-family files record tool calls without names
    return true;
}
