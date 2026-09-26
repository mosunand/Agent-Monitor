// ClaudeAdapter.cpp — see ClaudeAdapter.h.

#include "ClaudeAdapter.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

#include "Paths.h"

namespace {

constexpr qint64 kLineCap = 4 * 1024 * 1024;

QColor agentColor()
{
    return QColor(0xd9, 0x77, 0x57); // Anthropic coral
}

qint64 isoToMs(const QString& s)
{
    if (s.isEmpty())
        return 0;
    const QDateTime d = QDateTime::fromString(s, Qt::ISODateWithMs);
    return d.isValid() ? d.toMSecsSinceEpoch() : 0;
}

} // namespace

QColor ClaudeAdapter::color() const { return agentColor(); }

bool ClaudeAdapter::dataPresent(QString* dir) const
{
    const QString d = Paths::claudeProjectsDir();
    if (dir)
        *dir = d;
    return QDir(d).exists();
}

QVector<SessionFile> ClaudeAdapter::listSessionFiles(QString* note) const
{
    QVector<SessionFile> out;
    const QString root = Paths::claudeProjectsDir();
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
        *note = out.isEmpty() ? QStringLiteral("未发现会话 JSONL（先跑一次 claude）") : QString();
    return out;
}

QString ClaudeAdapter::userText(const QJsonObject& message)
{
    const QJsonValue c = message.value(QStringLiteral("content"));
    if (c.isString())
        return c.toString();
    QStringList parts;
    const QJsonArray arr = c.toArray();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            parts << o.value(QStringLiteral("text")).toString();
    }
    return parts.join(QStringLiteral("\n"));
}

bool ClaudeAdapter::parseFile(const SessionFile& f, types::SessionInfo& meta,
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

    struct Acc {
        types::UsageRecord rec;
        qint64 firstMs = 0;
        int lines = 0;
    };
    QHash<QString, Acc> acc;             // message.id → streaming accumulator
    QHash<QString, int> modelCount;      // session model = most frequent
    QString firstUserTitle, aiTitle, lastPromptTitle;
    bool titleSet = false;
    qint64 toolUses = 0;
    qint64 lastTs = 0;

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

        if (type == QLatin1String("user")) {
            if (meta.startedMs <= 0)
                meta.startedMs = ts;
            const QString cwd = o.value(QStringLiteral("cwd")).toString();
            if (!cwd.isEmpty())
                meta.cwd = cwd;
            if (!titleSet) {
                const QString t = userText(o.value(QStringLiteral("message")).toObject());
                const QString trimmed = t.trimmed();
                // skip tool_result-only / interrupt noise
                if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('<'))
                    && o.value(QStringLiteral("origin")).toObject()
                            .value(QStringLiteral("kind")).toString()
                            == QLatin1String("human")) {
                    firstUserTitle = trimmed;
                    titleSet = true;
                }
            }
        } else if (type == QLatin1String("ai-title")) {
            aiTitle = o.value(QStringLiteral("aiTitle")).toString();
        } else if (type == QLatin1String("last-prompt")) {
            lastPromptTitle = o.value(QStringLiteral("lastPrompt")).toString();
        } else if (type == QLatin1String("assistant")) {
            const QJsonObject m = o.value(QStringLiteral("message")).toObject();
            const QString mid = m.value(QStringLiteral("id")).toString();
            const QJsonObject u = m.value(QStringLiteral("usage")).toObject();
            const bool apiError = o.value(QStringLiteral("isApiErrorMessage")).toBool();

            if (apiError && u.isEmpty()) {
                // API error line — keep it as a zero-token error record so
                // the error rate covers failed calls too
                types::UsageRecord r;
                r.agentId = id();
                r.sessionId = o.value(QStringLiteral("sessionId")).toString(meta.sessionId);
                r.responseId = mid;
                r.timeMs = ts;
                r.model = m.value(QStringLiteral("model")).toString();
                r.source = o.value(QStringLiteral("isSidechain")).toBool()
                               ? QStringLiteral("sidechain")
                               : QStringLiteral("main");
                r.error = QStringLiteral("api_error");
                records.push_back(r);
                continue;
            }
            if (mid.isEmpty() || u.isEmpty())
                continue; // error/empty assistant line — nothing to count

            Acc& a = acc[mid];
            if (a.lines == 0) {
                a.rec.agentId = id();
                a.rec.sessionId = o.value(QStringLiteral("sessionId")).toString(
                    meta.sessionId);
                a.rec.responseId = mid;
                a.rec.timeMs = ts;
                a.rec.model = m.value(QStringLiteral("model")).toString();
                a.rec.source = o.value(QStringLiteral("isSidechain")).toBool()
                                   ? QStringLiteral("sidechain")
                                   : QStringLiteral("main");
                a.firstMs = ts;
            }
            a.rec.timeMs = ts;
            // usage on each line is cumulative for the message → keep last
            a.rec.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
            a.rec.cacheWriteTokens = u.value(QStringLiteral("cache_creation_input_tokens"))
                                         .toVariant().toLongLong();
            a.rec.cachedTokens = u.value(QStringLiteral("cache_read_input_tokens"))
                                     .toVariant().toLongLong();
            a.rec.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
            ++a.lines;

            const QJsonArray content = m.value(QStringLiteral("content")).toArray();
            for (const auto& v : content) {
                const QJsonObject c = v.toObject();
                if (c.value(QStringLiteral("type")).toString() == QLatin1String("tool_use")) {
                    ++toolUses;
                    if (outTools) {
                        const QString name = c.value(QStringLiteral("name")).toString();
                        if (!name.isEmpty()) {
                            ToolStat& st = (*outTools)[name];
                            st.calls += 1;
                            if (st.firstMs == 0 || ts < st.firstMs)
                                st.firstMs = ts;
                            if (ts > st.lastMs)
                                st.lastMs = ts;
                        }
                    }
                }
            }
        }
    }

    // finalize streaming groups → per-response records
    records.reserve(acc.size());
    for (auto it = acc.constBegin(); it != acc.constEnd(); ++it) {
        types::UsageRecord r = it->rec;
        if (r.timeMs > it->firstMs) {
            r.durOk = true;
            r.durationMs = double(r.timeMs - it->firstMs);
            // sub-100ms "streams" are single-chunk responses — the timing
            // granularity makes their TPS physically meaningless
            if (r.durationMs >= 100.0 && r.outputTokens > 0) {
                r.tpsOk = true;
                r.tps = double(r.outputTokens) / (r.durationMs / 1000.0);
            }
        }
        records.push_back(r);
        ++modelCount[r.model];
    }
    std::sort(records.begin(), records.end(),
              [](const types::UsageRecord& a, const types::UsageRecord& b) {
                  return a.timeMs < b.timeMs;
              });

    // session aggregates
    meta.updatedMs = qMax(meta.startedMs, lastTs);
    QString bestModel;
    int bestN = -1;
    for (auto it = modelCount.constBegin(); it != modelCount.constEnd(); ++it)
        if (it.value() > bestN) {
            bestN = it.value();
            bestModel = it.key();
        }
    meta.model = bestModel;
    if (!aiTitle.isEmpty())
        meta.title = aiTitle.left(120);
    else if (!firstUserTitle.isEmpty())
        meta.title = firstUserTitle.left(120);
    else if (!lastPromptTitle.isEmpty())
        meta.title = lastPromptTitle.left(120);
    else if (!meta.cwd.isEmpty())
        meta.title = QFileInfo(meta.cwd).fileName();
    if (meta.title.isEmpty())
        meta.title = meta.sessionId.left(12);
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
    meta.toolCalls = toolUses;
    return true;
}

// ── conversation replay (on demand) ─────────────────────────

namespace {

// content array → joined text of the given item types
QString joinContent(const QJsonArray& arr, const QStringList& types)
{
    QStringList parts;
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        if (types.contains(o.value(QStringLiteral("type")).toString()))
            parts << o.value(o.contains(QStringLiteral("thinking"))
                                 ? QStringLiteral("thinking")
                                 : QStringLiteral("text")).toString();
    }
    return parts.join(QStringLiteral("\n"));
}

// tool_result content → text (string or array form)
QString toolResultText(const QJsonObject& item)
{
    const QJsonValue c = item.value(QStringLiteral("content"));
    if (c.isString())
        return c.toString();
    return joinContent(c.toArray(), { QStringLiteral("text") });
}

} // namespace

bool ClaudeAdapter::parseMessages(const SessionFile& f,
                                  QVector<types::ChatMessage>& out) const
{
    QFile file(f.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine(kLineCap);
        if (line.size() <= 1)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.isEmpty())
            continue;
        const QString type = o.value(QStringLiteral("type")).toString();
        const qint64 ts = isoToMs(o.value(QStringLiteral("timestamp")).toString());

        if (type == QLatin1String("user")) {
            const QJsonValue c = o.value(QStringLiteral("message"))
                                     .toObject().value(QStringLiteral("content"));
            if (c.isString()) {
                const QString t = c.toString().trimmed();
                if (!t.isEmpty() && !t.startsWith(QLatin1Char('<'))) {
                    types::ChatMessage m;
                    m.role = QStringLiteral("user");
                    m.text = t.left(20000);
                    m.timeMs = ts;
                    out.push_back(m);
                }
                continue;
            }
            for (const auto& v : c.toArray()) {
                const QJsonObject item = v.toObject();
                const QString it = item.value(QStringLiteral("type")).toString();
                types::ChatMessage m;
                m.timeMs = ts;
                if (it == QLatin1String("text")) {
                    const QString t = item.value(QStringLiteral("text")).toString().trimmed();
                    if (t.isEmpty())
                        continue;
                    m.role = QStringLiteral("user");
                    m.text = t.left(20000);
                } else if (it == QLatin1String("tool_result")) {
                    m.role = QStringLiteral("tool_output");
                    m.text = toolResultText(item).left(6000);
                } else {
                    continue;
                }
                out.push_back(m);
            }
        } else if (type == QLatin1String("assistant")) {
            const QJsonObject m = o.value(QStringLiteral("message")).toObject();
            const QJsonArray content = m.value(QStringLiteral("content")).toArray();
            for (const auto& v : content) {
                const QJsonObject item = v.toObject();
                const QString it = item.value(QStringLiteral("type")).toString();
                types::ChatMessage cm;
                cm.timeMs = ts;
                cm.model = m.value(QStringLiteral("model")).toString();
                if (it == QLatin1String("text")) {
                    cm.role = QStringLiteral("assistant");
                    cm.text = item.value(QStringLiteral("text")).toString().left(20000);
                    cm.isError = o.value(QStringLiteral("isApiErrorMessage")).toBool();
                } else if (it == QLatin1String("thinking")) {
                    cm.role = QStringLiteral("reasoning");
                    cm.text = item.value(QStringLiteral("thinking")).toString().left(8000);
                } else if (it == QLatin1String("tool_use")) {
                    cm.role = QStringLiteral("tool");
                    cm.toolName = item.value(QStringLiteral("name")).toString();
                    cm.text = QString::fromUtf8(
                        QJsonDocument(item.value(QStringLiteral("input")).toObject())
                            .toJson(QJsonDocument::Compact)).left(4000);
                } else {
                    continue;
                }
                if (!cm.text.isEmpty() || !cm.toolName.isEmpty())
                    out.push_back(cm);
            }
        }
    }
    return true;
}
