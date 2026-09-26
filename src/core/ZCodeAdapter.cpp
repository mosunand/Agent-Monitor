// ZCodeAdapter.cpp — see ZCodeAdapter.h.

#include "ZCodeAdapter.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QThreadStorage>
#include <QVariant>

#include "Paths.h"

namespace {

QColor agentColor()
{
    return QColor(0x3b, 0x82, 0xf6); // zcode blue
}

constexpr int kBusyTimeoutMs = 5000;

// per-thread read-only source connection — QSqlDatabase must never cross
// threads, and ZCode's WAL db tolerates concurrent readers
QThreadStorage<QString> t_srcConn;

QSqlDatabase sourceConnection(QString* errOut)
{
    if (t_srcConn.hasLocalData()) {
        QSqlDatabase db = QSqlDatabase::database(t_srcConn.localData(), false);
        if (db.isOpen())
            return db;
    }
    const QString name = QStringLiteral("zcode-src-%1")
                             .arg(quintptr(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name))
        QSqlDatabase::removeDatabase(name);
    t_srcConn.setLocalData(name);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(Paths::zcodeDbPath());
    db.setConnectOptions(
        QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=%1")
            .arg(kBusyTimeoutMs));
    if (!db.open()) {
        if (errOut)
            *errOut = db.lastError().text();
        return db;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA busy_timeout = %1").arg(kBusyTimeoutMs));
    q.exec(QStringLiteral("PRAGMA query_only = ON"));
    return db;
}

} // namespace

QColor ZCodeAdapter::color() const { return agentColor(); }

bool ZCodeAdapter::dataPresent(QString* dir) const
{
    const QString d = Paths::zcodeDbPath();
    if (dir)
        *dir = d;
    return QFile::exists(d);
}

QVector<SessionFile> ZCodeAdapter::listSessionFiles(QString* note) const
{
    QVector<SessionFile> out;
    QString err;
    QSqlDatabase db = sourceConnection(&err);
    if (!db.isOpen()) {
        if (note)
            *note = QStringLiteral("数据库打不开：") + err.left(80);
        return out;
    }
    QSqlQuery q(db);
    // one pseudo-file per session; mtime = session.time_updated so the
    // incremental scanner only re-reads sessions that actually changed
    if (!q.exec(QStringLiteral(
            "SELECT id, time_updated FROM session ORDER BY time_updated DESC"))) {
        if (note)
            *note = QStringLiteral("查询失败：") + q.lastError().text().left(80);
        return out;
    }
    const QString dbPath = Paths::zcodeDbPath();
    while (q.next()) {
        SessionFile f;
        f.path = dbPath + QLatin1Char('|') + q.value(0).toString();
        f.mtimeMs = q.value(1).toLongLong();
        f.size = 0;
        out.push_back(f);
    }
    if (note)
        *note = out.isEmpty() ? QStringLiteral("数据库中还没有会话") : QString();
    return out;
}

QString ZCodeAdapter::sessionIdFromPseudoPath(const QString& path)
{
    const int bar = path.lastIndexOf(QLatin1Char('|'));
    return bar >= 0 ? path.mid(bar + 1) : path;
}

// ── conversation replay (on demand, from message/part tables) ──
namespace {

// part 自带 time{start|created}；比 message 的创建时间更贴近真实发生点
qint64 partTimeMs(const QJsonObject& part, qint64 fallback)
{
    const QJsonObject t = part.value(QStringLiteral("time")).toObject();
    const qint64 v = t.contains(QStringLiteral("start"))
        ? t.value(QStringLiteral("start")).toVariant().toLongLong()
        : t.value(QStringLiteral("created")).toVariant().toLongLong();
    return v > 0 ? v : fallback;
}

} // namespace

bool ZCodeAdapter::parseMessages(const SessionFile& f,
                                 QVector<types::ChatMessage>& out) const
{
    const QString sessionId = sessionIdFromPseudoPath(f.path);
    QString err;
    QSqlDatabase db = sourceConnection(&err);
    if (!db.isOpen()) {
        qWarning("zcode: source db unavailable: %s", qPrintable(err));
        return false;
    }

    // message.data = {role, time{created}, modelID, semantics{kind…}};
    // part.data = 内容块（text / reasoning / tool / step-start…）。
    // 只回放真人输入与助手响应：agent_runtime 注入的隐藏消息（todo 提醒、
    // 压缩摘要、系统提醒）与 timeline 事件按 zcode 自己的显示语义跳过
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT m.data, p.data FROM part p"
        " JOIN message m ON m.id = p.message_id"
        " WHERE p.session_id = ? ORDER BY m.time_created, p.sequence"));
    q.addBindValue(sessionId);
    if (!q.exec())
        return false;
    while (q.next()) {
        const QJsonObject msg = QJsonDocument::fromJson(
            q.value(0).toString().toUtf8()).object();
        const QJsonObject part = QJsonDocument::fromJson(
            q.value(1).toString().toUtf8()).object();
        const QString type = part.value(QStringLiteral("type")).toString();
        const QString role = msg.value(QStringLiteral("role")).toString();
        const QString kind = msg.value(QStringLiteral("semantics")).toObject()
                                 .value(QStringLiteral("kind")).toString();
        const qint64 baseMs = msg.value(QStringLiteral("time")).toObject()
                                  .value(QStringLiteral("created"))
                                  .toVariant().toLongLong();

        if (type == QLatin1String("text")) {
            const QString text = part.value(QStringLiteral("text")).toString().trimmed();
            if (text.isEmpty())
                continue;
            types::ChatMessage m;
            if (role == QLatin1String("user")) {
                if (kind != QLatin1String("user_prompt"))
                    continue; // 隐藏注入（todo 提醒 / 压缩摘要 / 系统提醒）
                m.role = QStringLiteral("user");
            } else {
                if (kind != QLatin1String("assistant_response"))
                    continue; // timeline 事件等非对话内容
                m.role = QStringLiteral("assistant");
                m.model = msg.value(QStringLiteral("modelID")).toString();
            }
            m.text = text.left(20000);
            m.timeMs = partTimeMs(part, baseMs);
            out.push_back(m);
        } else if (type == QLatin1String("reasoning")) {
            const QString text = part.value(QStringLiteral("text")).toString();
            if (text.isEmpty())
                continue;
            types::ChatMessage m;
            m.role = QStringLiteral("reasoning");
            m.text = text.left(8000);
            m.timeMs = partTimeMs(part, baseMs);
            out.push_back(m);
        } else if (type == QLatin1String("tool")) {
            // 调用与输出在同一个 part 的 state 里：input=参数，output=结果
            const QJsonObject state = part.value(QStringLiteral("state")).toObject();
            types::ChatMessage call;
            call.role = QStringLiteral("tool");
            call.toolName = part.value(QStringLiteral("tool")).toString();
            call.text = QString::fromUtf8(QJsonDocument(
                state.value(QStringLiteral("input")).toObject())
                .toJson(QJsonDocument::Compact)).left(4000);
            call.timeMs = partTimeMs(part, baseMs);
            out.push_back(call);
            const QString output = state.value(QStringLiteral("output")).toString();
            if (!output.isEmpty()) {
                types::ChatMessage res;
                res.role = QStringLiteral("tool_output");
                res.text = output.left(6000);
                res.timeMs = state.value(QStringLiteral("time")).toObject()
                                 .value(QStringLiteral("end")).toVariant().toLongLong();
                if (res.timeMs <= 0)
                    res.timeMs = call.timeMs;
                res.isError = state.value(QStringLiteral("status")).toString()
                              == QLatin1String("error");
                out.push_back(res);
            }
        }
        // step-start / step-finish / file / timeline / compaction：非对话内容
    }
    return true;
}

qint64 ZCodeAdapter::longestChatMs(const QString& familyLike) const
{
    // matches ZCode's own usage dashboard: Σ duration of COMPLETED turns
    // per session, best session wins; familyLike narrows by model family
    QString err;
    QSqlDatabase db = sourceConnection(&err);
    if (!db.isOpen())
        return -1;
    QSqlQuery q(db);
    QString sql = QStringLiteral(
        "SELECT MAX(agg) FROM (SELECT t.session_id, SUM(t.duration_ms) agg"
        " FROM turn_usage t WHERE t.status = 'completed'");
    if (!familyLike.isEmpty())
        sql += QStringLiteral(
            " AND EXISTS (SELECT 1 FROM model_usage m WHERE m.turn_id = t.turn_id"
            " AND lower(m.model) LIKE :fam)");
    sql += QStringLiteral(" GROUP BY t.session_id)");
    q.prepare(sql);
    if (!familyLike.isEmpty())
        q.bindValue(QStringLiteral(":fam"), familyLike);
    if (!q.exec())
        return -1;
    return q.next() ? q.value(0).toLongLong() : -1;
}

bool ZCodeAdapter::parseFile(const SessionFile& f, types::SessionInfo& meta,
                             QVector<types::UsageRecord>& records,
                             QHash<QString, ToolStat>* outTools) const
{
    meta = types::SessionInfo{};
    meta.agentId = id();
    meta.filePath = f.path;
    meta.sessionId = sessionIdFromPseudoPath(f.path);
    meta.originator = QStringLiteral("zcode");

    QString err;
    QSqlDatabase db = sourceConnection(&err);
    if (!db.isOpen()) {
        qWarning("zcode: source db unavailable: %s", qPrintable(err));
        return false;
    }

    // ── session meta ──
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT title, directory, time_created, time_updated FROM session"
            " WHERE id = ?"));
        q.addBindValue(meta.sessionId);
        q.exec();
        if (!q.next())
            return false; // session vanished between list and parse
        meta.title = q.value(0).toString();
        meta.cwd = q.value(1).toString();
        meta.startedMs = q.value(2).toLongLong();
        meta.updatedMs = q.value(3).toLongLong();
        if (meta.title.isEmpty())
            meta.title = QFileInfo(meta.cwd).fileName();
        if (meta.title.isEmpty())
            meta.title = meta.sessionId.left(12);
    }

    // ── model usage rows → one UsageRecord per request ──
    // durOk gate: exclude only turns that ended in failure/cancel — a
    // turn still RUNNING has valid per-request server durations (the
    // longest-chat metric is computed separately from turn_usage and is
    // not affected by this set)
    QSet<QString> badTurns;
    {
        QSqlQuery ct(db);
        ct.prepare(QStringLiteral(
            "SELECT turn_id FROM turn_usage WHERE session_id = ?"
            " AND status IN ('error', 'cancelled', 'aborted')"));
        ct.addBindValue(meta.sessionId);
        ct.exec();
        while (ct.next())
            badTurns.insert(ct.value(0).toString());
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT completed_at, model_id, variant, duration_ms,"
        " input_tokens, output_tokens, reasoning_tokens,"
        " cache_read_input_tokens, cache_creation_input_tokens,"
        " query_source, status, error_type, cancelled_by_user, turn_id"
        " FROM model_usage WHERE session_id = ? ORDER BY started_at"));
    q.addBindValue(meta.sessionId);
    q.exec();
    while (q.next()) {
        const qint64 completedMs = q.value(0).toLongLong();
        const qint64 durMs = q.value(3).toLongLong();
        const QString errorType = q.value(11).toString();
        const bool cancelled = q.value(12).toBool();
        const QString rowTurn = q.value(13).toString();

        types::UsageRecord r;
        r.agentId = id();
        r.sessionId = meta.sessionId;
        r.turnId = rowTurn;
        r.timeMs = completedMs > 0 ? completedMs : meta.updatedMs;
        r.model = q.value(1).toString();
        r.effort = q.value(2).toString();
        r.source = q.value(9).toString();
        r.durOk = durMs > 0 && !badTurns.contains(rowTurn);
        r.durationMs = double(durMs);
        // server-measured duration → exact TPS (same 100ms floor as claude:
        // sub-100ms rows have no meaningful tokens/sec)
        if (r.durOk && r.durationMs >= 100.0) {
            const qint64 outTok = q.value(5).toLongLong();
            const qint64 reasonTok = q.value(6).toLongLong();
            if (outTok + reasonTok > 0) {
                r.tpsOk = true;
                r.tps = double(outTok + reasonTok) / (r.durationMs / 1000.0);
            }
        }
        // error/cancelled rows KEEP their tokens — they were consumed.
        // Only the streaming-duration gate excludes them (official zcode
        // semantics: completed-turn filter applies to chat duration only)
        if (!errorType.isEmpty() || cancelled)
            r.error = cancelled ? QStringLiteral("cancelled_by_user") : errorType;
        r.inputTokens = q.value(4).toLongLong();
        r.outputTokens = q.value(5).toLongLong();
        r.reasoningTokens = q.value(6).toLongLong();
        r.cachedTokens = q.value(7).toLongLong();
        r.cacheWriteTokens = q.value(8).toLongLong();
        records.push_back(r);
    }

    // ── tool usage → per-name counts ──
    QSqlQuery tq(db);
    tq.prepare(QStringLiteral(
        "SELECT tool_name, COUNT(*), MIN(started_at), MAX(started_at)"
        " FROM tool_usage WHERE session_id = ?"
        " GROUP BY tool_name ORDER BY 2 DESC"));
    tq.addBindValue(meta.sessionId);
    tq.exec();
    qint64 toolCalls = 0;
    while (tq.next()) {
        const qint64 calls = tq.value(1).toLongLong();
        toolCalls += calls;
        if (outTools && calls > 0) {
            const QString name = tq.value(0).toString();
            if (!name.isEmpty()) {
                ToolStat& st = (*outTools)[name];
                st.calls += calls;
                const qint64 first = tq.value(2).toLongLong();
                const qint64 last = tq.value(3).toLongLong();
                if (st.firstMs == 0 || (first > 0 && first < st.firstMs))
                    st.firstMs = first;
                if (last > st.lastMs)
                    st.lastMs = last;
            }
        }
    }

    // ── session aggregates ──
    for (const types::UsageRecord& r : records) {
        if (!r.error.isEmpty())
            meta.errors++;
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
