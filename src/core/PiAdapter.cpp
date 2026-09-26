// PiAdapter.cpp — see PiAdapter.h.

#include "PiAdapter.h"

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

QColor agentColor()
{
    return QColor(0xe0, 0x6c, 0x75); // pi red
}

QString sessionsDir()
{
    // PI_CODING_AGENT_SESSION_DIR → dir directly;
    // PI_CODING_AGENT_DIR → <dir>/agent/sessions; default ~/.pi/agent/sessions
    const QByteArray sd = qgetenv("PI_CODING_AGENT_SESSION_DIR");
    if (!sd.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(sd));
    const QByteArray root = qgetenv("PI_CODING_AGENT_DIR");
    if (!root.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(root))
               + QStringLiteral("/agent/sessions");
    return QDir::homePath() + QStringLiteral("/.pi/agent/sessions");
}

qint64 isoToMs(const QString& s)
{
    if (s.isEmpty())
        return 0;
    const QDateTime d = QDateTime::fromString(s, Qt::ISODateWithMs);
    return d.isValid() ? d.toMSecsSinceEpoch() : 0;
}

// tolerant usage extraction: pi uses camelCase (input/output/cacheRead/
// cacheWrite); accept snake_case variants too
void readUsage(const QJsonObject& u, types::UsageRecord& r)
{
    r.inputTokens = u.value(QStringLiteral("input")).toVariant().toLongLong();
    if (!r.inputTokens)
        r.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
    r.outputTokens = u.value(QStringLiteral("output")).toVariant().toLongLong();
    if (!r.outputTokens)
        r.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
    r.cachedTokens = u.value(QStringLiteral("cacheRead")).toVariant().toLongLong();
    if (!r.cachedTokens)
        r.cachedTokens = u.value(QStringLiteral("cache_read_input_tokens")).toVariant().toLongLong();
    r.cacheWriteTokens = u.value(QStringLiteral("cacheWrite")).toVariant().toLongLong();
    if (!r.cacheWriteTokens)
        r.cacheWriteTokens = u.value(QStringLiteral("cache_creation_input_tokens")).toVariant().toLongLong();
}

} // namespace

QColor PiAdapter::color() const { return agentColor(); }

bool PiAdapter::dataPresent(QString* dir) const
{
    const QString d = sessionsDir();
    if (dir)
        *dir = d;
    return QDir(d).exists();
}

QVector<SessionFile> PiAdapter::listSessionFiles(QString* note) const
{
    QVector<SessionFile> out;
    const QString root = sessionsDir();
    if (QDir(root).exists()) {
        QDirIterator it(root, { QStringLiteral("*.jsonl") }, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QFileInfo info(path);
            SessionFile f;
            f.path = path;
            f.mtimeMs = info.lastModified().toMSecsSinceEpoch();
            f.size = info.size();
            out.push_back(f);
        }
    }
    if (note)
        *note = out.isEmpty() ? QStringLiteral("未发现会话文件（按官方文档格式解析）") : QString();
    return out;
}

bool PiAdapter::parseFile(const SessionFile& f, types::SessionInfo& meta,
                          QVector<types::UsageRecord>& records,
                          QHash<QString, ToolStat>* outTools) const
{
    Q_UNUSED(outTools); // pi tool-call shape not documented — none counted
    meta = types::SessionInfo{};
    meta.agentId = id();
    meta.filePath = f.path;
    meta.sessionId = QFileInfo(f.path).completeBaseName();

    QFile file(f.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QString model;
    qint64 lastTs = 0;
    qint64 prevTs = 0;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine(4 * 1024 * 1024);
        if (line.size() <= 1)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.isEmpty())
            continue;
        const QString type = o.value(QStringLiteral("type")).toString();
        const qint64 ts = isoToMs(o.value(QStringLiteral("timestamp")).toString());
        if (ts > 0) {
            lastTs = qMax(lastTs, ts);
            prevTs = ts;
        }

        if (type == QLatin1String("session")) {
            if (meta.startedMs <= 0)
                meta.startedMs = ts;
            meta.cwd = o.value(QStringLiteral("cwd")).toString();
            meta.originator = QStringLiteral("pi ") + o.value(QStringLiteral("version")).toString();
            const QString sid = o.value(QStringLiteral("id")).toString();
            if (!sid.isEmpty())
                meta.sessionId = sid;
        } else if (type == QLatin1String("model_change")) {
            const QString m = o.value(QStringLiteral("model")).toString();
            if (!m.isEmpty())
                model = m;
        } else if (type == QLatin1String("message")) {
            // pi wraps the LLM message; accept both nested and flat shapes
            QJsonObject m = o.value(QStringLiteral("message")).toObject();
            if (m.isEmpty())
                m = o;
            if (m.value(QStringLiteral("role")).toString() != QLatin1String("assistant")
                && !m.contains(QStringLiteral("usage")))
                continue;
            const QJsonObject u = m.value(QStringLiteral("usage")).toObject();
            if (u.isEmpty())
                continue;
            types::UsageRecord r;
            r.agentId = id();
            r.sessionId = meta.sessionId;
            r.timeMs = ts;
            r.model = m.value(QStringLiteral("model")).toString(
                o.value(QStringLiteral("model")).toString());
            if (r.model.isEmpty())
                r.model = model;
            r.source = QStringLiteral("main");
            readUsage(u, r);
            if (r.totalTokens() == 0)
                continue;
            if (prevTs > 0 && ts > prevTs) {
                r.durOk = true;
                r.durApprox = true;
                r.durationMs = double(ts - prevTs);
            }
            records.push_back(r);
        }
    }

    meta.updatedMs = qMax(meta.startedMs, lastTs);
    meta.model = model;
    if (!meta.cwd.isEmpty() && meta.title.isEmpty())
        meta.title = QFileInfo(meta.cwd).fileName();
    for (const types::UsageRecord& r : records) {
        meta.inputTokens += r.inputTokens;
        meta.cachedTokens += r.cachedTokens;
        meta.cacheWriteTokens += r.cacheWriteTokens;
        meta.outputTokens += r.outputTokens;
        meta.reasoningTokens += r.reasoningTokens;
    }
    meta.responses = records.size();
    if (meta.title.isEmpty())
        meta.title = meta.sessionId.left(12);
    return true;
}
