// CodexAdapter.cpp — see CodexAdapter.h.

#include "CodexAdapter.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "Paths.h"

namespace {

constexpr qint64 kLineCap = 4 * 1024 * 1024; // absurd line guard
// responses farther apart than this carry user/tool think-time — the
// streaming duration is unmeasurable, so no TPS for them
constexpr qint64 kMaxStreamGapMs = 120 * 1000;

QColor agentColor()
{
    return QColor(0x10, 0xa3, 0x7a); // OpenAI teal
}

qint64 isoToMs(const QString& s)
{
    if (s.isEmpty())
        return 0;
    const QDateTime d = QDateTime::fromString(s, Qt::ISODateWithMs);
    return d.isValid() ? d.toMSecsSinceEpoch() : 0;
}

// pull the joined text out of a codex message content array
QString messageText(const QJsonArray& content)
{
    QStringList parts;
    for (const auto& v : content) {
        const QJsonObject o = v.toObject();
        const QString t = o.value(QStringLiteral("text")).toString();
        if (!t.isEmpty())
            parts << t;
    }
    return parts.join(QStringLiteral("\n"));
}

} // namespace

QColor CodexAdapter::color() const { return agentColor(); }

bool CodexAdapter::dataPresent(QString* dir) const
{
    const QString d = Paths::codexSessionsDir();
    if (dir)
        *dir = d;
    return QDir(d).exists();
}

QVector<SessionFile> CodexAdapter::listSessionFiles(QString* note) const
{
    QVector<SessionFile> out;
    const QStringList roots = { Paths::codexSessionsDir(), Paths::codexArchiveDir() };
    for (const QString& root : roots) {
        if (!QDir(root).exists())
            continue;
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
        *note = out.isEmpty() ? QStringLiteral("未发现 rollout 会话文件") : QString();
    return out;
}

QString CodexAdapter::sessionIdFromPath(const QString& path)
{
    // rollout-2026-09-17T22-23-51-01a0afc0-a059-7220-9155-3a14094261d0.jsonl
    static const QRegularExpression re(
        QStringLiteral("([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\.jsonl$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(path);
    if (m.hasMatch())
        return m.captured(1);
    const QString stem = QFileInfo(path).completeBaseName();
    const QStringList parts = stem.split(QLatin1Char('-'));
    if (parts.size() >= 5)
        return QStringList(parts.mid(parts.size() - 5)).join(QLatin1Char('-'));
    return stem;
}

bool CodexAdapter::parseFile(const SessionFile& f, types::SessionInfo& meta,
                             QVector<types::UsageRecord>& records,
                             QHash<QString, ToolStat>* outTools) const
{
    meta = types::SessionInfo{};
    meta.agentId = id();
    meta.filePath = f.path;
    meta.sessionId = sessionIdFromPath(f.path);

    QFile file(f.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    // pre-pass: does this file carry the NEW token_usage_record format?
    // (substring probe — no JSON parse). If yes, token_count snapshots are
    // never synthesized, even ones preceding the first usage record —
    // otherwise those would double-count.
    bool fileHasUsageRecords = false;
    {
        while (!file.atEnd()) {
            if (file.readLine(kLineCap).contains("token_usage_record")) {
                fileHasUsageRecords = true;
                break;
            }
        }
        file.seek(0);
    }

    struct TurnInfo { QString model, effort; qint64 startMs = 0; };
    QHash<QString, TurnInfo> turns;
    QString lastTurnId;

    qint64 lastCountMs = 0;          // previous token_count timestamp
    // new-format flag: pre-pass verdict (records re-confirm as they stream)
    bool sawUsageRecord = fileHasUsageRecords;
    bool titleSet = false;
    QString lastPromptTitle;
    qint64 toolCalls = 0;
    qint64 lastTs = 0;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine(kLineCap);
        if (line.size() <= 1)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.isEmpty())
            continue;
        const QString type = o.value(QStringLiteral("type")).toString();
        const QJsonObject p = o.value(QStringLiteral("payload")).toObject();
        const qint64 ts = isoToMs(o.value(QStringLiteral("timestamp")).toString());
        if (ts > 0)
            lastTs = qMax(lastTs, ts);

        if (type == QLatin1String("session_meta")) {
            if (meta.startedMs <= 0)
                meta.startedMs = ts;
            meta.cwd = p.value(QStringLiteral("cwd")).toString();
            meta.originator = p.value(QStringLiteral("originator")).toString();
            meta.contextWindow = p.value(QStringLiteral("context_window"))
                                     .toVariant().toLongLong();
            // NOTE: session identity stays filename-derived (unique per file).
            // session_meta.session_id is the THREAD id — resume forks create
            // new rollout files sharing it, which would collide records.
        } else if (type == QLatin1String("turn_context")) {
            TurnInfo ti;
            ti.model = p.value(QStringLiteral("model")).toString();
            ti.effort = p.value(QStringLiteral("effort")).toString();
            ti.startMs = ts;
            lastTurnId = p.value(QStringLiteral("turn_id")).toString();
            turns.insert(lastTurnId, ti);
            if (!ti.model.isEmpty())
                meta.model = ti.model;
        } else if (type == QLatin1String("token_usage_record")) {
            sawUsageRecord = true;
            const QJsonObject u = p.value(QStringLiteral("usage")).toObject();
            types::UsageRecord r;
            r.agentId = id();
            r.sessionId = meta.sessionId;
            r.turnId = p.value(QStringLiteral("turn_id")).toString();
            r.responseId = p.value(QStringLiteral("response_id")).toString();
            r.timeMs = ts;
            r.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
            r.cachedTokens = u.value(QStringLiteral("cached_input_tokens")).toVariant().toLongLong();
            r.cacheWriteTokens = u.value(QStringLiteral("cache_write_input_tokens")).toVariant().toLongLong();
            r.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
            r.reasoningTokens = u.value(QStringLiteral("reasoning_output_tokens")).toVariant().toLongLong();
            const TurnInfo ti = turns.value(r.turnId);
            r.model = ti.model;
            r.effort = ti.effort;
            r.source = QStringLiteral("main");
            // duration ≈ gap to the previous response of the same turn
            // (first response: gap to the turn start) — includes tool gaps,
            // and a long gap means the user paused → unmeasurable, drop it
            const TurnInfo cur = turns.value(r.turnId);
            const qint64 ref = (lastCountMs > cur.startMs) ? lastCountMs : cur.startMs;
            if (ts > ref && ref > 0 && ts - ref <= kMaxStreamGapMs) {
                r.durOk = true;
                r.durApprox = true;
                r.durationMs = double(ts - ref);
                if (r.outputTokens + r.reasoningTokens > 0) {
                    r.tpsOk = true;
                    r.tps = double(r.outputTokens + r.reasoningTokens)
                            / (r.durationMs / 1000.0);
                }
            }
            records.push_back(r);
            lastCountMs = ts;
        } else if (type == QLatin1String("event_msg")) {
            const QString et = p.value(QStringLiteral("type")).toString();
            if (et == QLatin1String("token_count") && !sawUsageRecord) {
                // old-format synthesis: one token_count per response with
                // last_token_usage = that response's usage
                const QJsonObject info = p.value(QStringLiteral("info")).toObject();
                const QJsonObject last = info.value(QStringLiteral("last_token_usage")).toObject();
                if (!meta.contextWindow)
                    meta.contextWindow = info.value(QStringLiteral("model_context_window"))
                                             .toVariant().toLongLong();
                types::UsageRecord r;
                r.agentId = id();
                r.sessionId = meta.sessionId;
                r.turnId = lastTurnId;
                r.timeMs = ts;
                r.inputTokens = last.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
                r.cachedTokens = last.value(QStringLiteral("cached_input_tokens")).toVariant().toLongLong();
                r.outputTokens = last.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
                r.reasoningTokens = last.value(QStringLiteral("reasoning_output_tokens")).toVariant().toLongLong();
                const TurnInfo ti = turns.value(lastTurnId);
                r.model = ti.model;
                r.effort = ti.effort;
                r.source = QStringLiteral("main");
                const qint64 ref = lastCountMs > 0 ? lastCountMs : ti.startMs;
                if (ts > ref && ref > 0 && ts - ref <= kMaxStreamGapMs) {
                    r.durOk = true;
                    r.durApprox = true;
                    r.durationMs = double(ts - ref);
                    if (r.outputTokens + r.reasoningTokens > 0) {
                        r.tpsOk = true;
                        r.tps = double(r.outputTokens + r.reasoningTokens)
                                / (r.durationMs / 1000.0);
                    }
                }
                if (r.totalTokens() > 0)
                    records.push_back(r);
                lastCountMs = ts;
            } else if (et == QLatin1String("user_message") && !titleSet) {
                const QString t = p.value(QStringLiteral("message")).toString();
                if (!t.isEmpty() && !t.startsWith(QLatin1Char('<')))
                    lastPromptTitle = t;
            } else if (et == QLatin1String("turn_aborted")) {
                // aborted turn → a zero-token error record keeps the error
                // rate uniform with completed requests
                types::UsageRecord r;
                r.agentId = id();
                r.sessionId = meta.sessionId;
                r.turnId = lastTurnId;
                r.timeMs = ts;
                const TurnInfo ti = turns.value(lastTurnId);
                r.model = ti.model;
                r.effort = ti.effort;
                r.source = QStringLiteral("main");
                r.error = QStringLiteral("turn_aborted");
                records.push_back(r);
            }
        } else if (type == QLatin1String("response_item")) {
            const QString it = p.value(QStringLiteral("type")).toString();
            if (it == QLatin1String("function_call")) {
                ++toolCalls;
                if (outTools) {
                    const QString name = p.value(QStringLiteral("name")).toString();
                    if (!name.isEmpty()) {
                        ToolStat& st = (*outTools)[name];
                        st.calls += 1;
                        if (st.firstMs == 0 || ts < st.firstMs)
                            st.firstMs = ts;
                        if (ts > st.lastMs)
                            st.lastMs = ts;
                    }
                }
            } else if (it == QLatin1String("message")
                       && p.value(QStringLiteral("role")).toString() == QLatin1String("user")
                       && !titleSet) {
                const QString t = messageText(p.value(QStringLiteral("content")).toArray());
                const QString trimmed = t.trimmed();
                // skip the <environment_context>/<app-context> wrapper messages
                if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('<'))) {
                    lastPromptTitle = trimmed;
                    titleSet = true;
                }
            }
        }
    }

    // session aggregates
    meta.updatedMs = qMax(meta.startedMs, lastTs);
    if (!lastPromptTitle.isEmpty())
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
    meta.toolCalls = toolCalls;
    return true;
}

// ── conversation replay (on demand) ─────────────────────────

bool CodexAdapter::parseMessages(const SessionFile& f,
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
        if (o.value(QStringLiteral("type")).toString() != QLatin1String("response_item"))
            continue;
        const QJsonObject p = o.value(QStringLiteral("payload")).toObject();
        const QString it = p.value(QStringLiteral("type")).toString();
        const qint64 ts = isoToMs(o.value(QStringLiteral("timestamp")).toString());

        if (it == QLatin1String("message")) {
            const QString role = p.value(QStringLiteral("role")).toString();
            const QString text = messageText(p.value(QStringLiteral("content")).toArray());
            if (text.isEmpty())
                continue;
            if (role == QLatin1String("developer"))
                continue; // internal instructions — noise for replay
            if (role == QLatin1String("user") && text.trimmed().startsWith(QLatin1Char('<')))
                continue; // environment/app context wrappers
            types::ChatMessage m;
            m.role = role;
            m.text = text.left(20000);
            m.timeMs = ts;
            out.push_back(m);
        } else if (it == QLatin1String("reasoning")) {
            QStringList parts;
            for (const auto& v : p.value(QStringLiteral("summary")).toArray())
                parts << v.toObject().value(QStringLiteral("text")).toString();
            const QString text = parts.join(QStringLiteral("\n"));
            if (text.isEmpty())
                continue;
            types::ChatMessage m;
            m.role = QStringLiteral("reasoning");
            m.text = text.left(8000);
            m.timeMs = ts;
            out.push_back(m);
        } else if (it == QLatin1String("function_call")) {
            types::ChatMessage m;
            m.role = QStringLiteral("tool");
            m.toolName = p.value(QStringLiteral("name")).toString();
            m.text = p.value(QStringLiteral("arguments")).toString().left(4000);
            m.timeMs = ts;
            out.push_back(m);
        } else if (it == QLatin1String("function_call_output")) {
            types::ChatMessage m;
            m.role = QStringLiteral("tool_output");
            m.text = p.value(QStringLiteral("output")).toString().left(6000);
            m.timeMs = ts;
            out.push_back(m);
        } else if (it == QLatin1String("web_search_call")) {
            types::ChatMessage m;
            m.role = QStringLiteral("tool");
            m.toolName = QStringLiteral("web_search");
            m.text = p.value(QStringLiteral("action")).toObject()
                         .value(QStringLiteral("query")).toString().left(1000);
            m.timeMs = ts;
            out.push_back(m);
        }
    }
    return true;
}
