// Store.cpp — see Store.h.

#include "Store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QThreadStorage>
#include <QVariant>

#include "Paths.h"
#include "util/Async.h"

namespace {

// per-thread connection names — QSqlDatabase must never cross threads
QThreadStorage<QString> t_connName;

// v2: responses gained stored tps_ok/tps (gating decided at parse time)
// v3: tools table (per-tool aggregation) + response error text + session errors
// v4: tools rows gained first_ms/last_ms — time-window filtering by tool time
constexpr int kSchemaVersion = 4;

// binary archive (self-owned history): little-endian, versioned
constexpr char kArchiveMagic[8] = {'A', 'G', 'M', 'O', 'N', 0x41, 0x52, 0x01};
constexpr quint32 kArchiveVersion = 2;

QString archiveKey(const QString& agent, const QString& sessionId,
                   const QString& model, const QString& effort, qint64 dayMs)
{
    return agent + QLatin1Char('\x1F') + sessionId + QLatin1Char('\x1F')
         + model + QLatin1Char('\x1F') + effort + QLatin1Char('\x1F')
         + QString::number(dayMs);
}

void putStr(QDataStream& out, const QString& s)
{
    const QByteArray b = s.toUtf8();
    out << quint16(b.size());
    out.writeRawData(b.constData(), b.size());
}

QString getStr(QDataStream& in)
{
    quint16 len = 0;
    in >> len;
    QByteArray b(len, Qt::Uninitialized);
    if (len > 0)
        in.readRawData(b.data(), len);
    return QString::fromUtf8(b);
}

} // namespace

Store& Store::instance()
{
    static Store s;
    return s;
}

Store::Store()
    : QObject(nullptr)
    , m_adapters(AgentRegistry::createAdapters())
{
    m_scanBusy.storeRelaxed(0);
    m_pollPaused.storeRelaxed(0);
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &Store::pollTick);

    QSettings settings;
    qint64 retention = settings.value(QStringLiteral("archiveRetentionDays"), 730)
                           .toLongLong();
    if (retention < 30)
        retention = 730;
    m_retentionDays = retention;
}

void Store::setRetentionDays(qint64 days)
{
    if (days < 30)
        days = 30;
    m_retentionDays = days;
    QSettings settings;
    settings.setValue(QStringLiteral("archiveRetentionDays"), days);
    if (m_archiveLoaded)
        mergeArchiveFromCache(); // prunes immediately and rewrites the file
    emit dataChanged();
}

QSqlDatabase Store::connection()
{
    if (t_connName.hasLocalData()) {
        QSqlDatabase db = QSqlDatabase::database(t_connName.localData(), false);
        if (db.isOpen())
            return db;
    }
    const QString name = QStringLiteral("am-%1")
                             .arg(quintptr(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name))
        QSqlDatabase::removeDatabase(name);
    t_connName.setLocalData(name);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(Paths::cacheDbPath());
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    if (!db.open())
        return db;
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    return db;
}

void Store::createSchema()
{
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta("
                          "  k TEXT PRIMARY KEY, v TEXT)"));
    // schema migration: a stale cache is disposable — drop and rebuild
    int version = 0;
    {
        QSqlQuery v(db);
        v.exec(QStringLiteral("SELECT v FROM meta WHERE k = 'schema_version'"));
        if (v.next())
            version = v.value(0).toInt();
    }
    if (version != kSchemaVersion) {
        q.exec(QStringLiteral("DROP TABLE IF EXISTS responses"));
        q.exec(QStringLiteral("DROP TABLE IF EXISTS sessions"));
        q.exec(QStringLiteral("DROP TABLE IF EXISTS files"));
        q.exec(QStringLiteral("DROP TABLE IF EXISTS tools"));
    }
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS files("
                          "  path TEXT PRIMARY KEY,"
                          "  agent TEXT NOT NULL,"
                          "  mtime INTEGER, size INTEGER)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS sessions("
                          "  agent TEXT NOT NULL, session_id TEXT NOT NULL,"
                          "  path TEXT NOT NULL,"
                          "  title TEXT, cwd TEXT, model TEXT, originator TEXT,"
                          "  started INTEGER, updated INTEGER,"
                          "  responses INTEGER, tool_calls INTEGER, errors INTEGER,"
                          "  in_tok INTEGER, cached_tok INTEGER,"
                          "  cache_write_tok INTEGER, out_tok INTEGER,"
                          "  reason_tok INTEGER, context_window INTEGER,"
                          "  PRIMARY KEY(agent, session_id))"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sessions_updated"
                          "  ON sessions(updated DESC)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS tools("
                          "  agent TEXT NOT NULL, session_id TEXT NOT NULL,"
                          "  name TEXT NOT NULL, calls INTEGER NOT NULL,"
                          "  first_ms INTEGER, last_ms INTEGER)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tools_name"
                          "  ON tools(agent, name)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS responses("
                          "  agent TEXT NOT NULL, session_id TEXT NOT NULL,"
                          "  turn_id TEXT, response_id TEXT,"
                          "  time_ms INTEGER NOT NULL,"
                          "  dur_ok INTEGER, dur_approx INTEGER, dur_ms REAL,"
                          "  model TEXT, effort TEXT, source TEXT,"
                          "  in_tok INTEGER, cached_tok INTEGER,"
                          "  cache_write_tok INTEGER, out_tok INTEGER,"
                          "  reason_tok INTEGER,"
                          "  tps_ok INTEGER, tps REAL, error TEXT)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_resp_time"
                          "  ON responses(time_ms)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_resp_session"
                          "  ON responses(agent, session_id)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_resp_model"
                          "  ON responses(agent, model)"));
    q.exec(QStringLiteral("INSERT OR REPLACE INTO meta VALUES('schema_version', '%1')")
               .arg(kSchemaVersion));
}

bool Store::openCache(QString* errOut)
{
    if (!Paths::ensureCacheDir()) {
        if (errOut)
            *errOut = QStringLiteral("cannot create cache dir");
        return false;
    }
    QSqlDatabase db = connection();
    if (!db.isOpen()) {
        if (errOut)
            *errOut = db.lastError().text();
        return false;
    }
    createSchema();
    loadArchive();
    return true;
}

void Store::startPolling(int intervalMs)
{
    m_timer.setInterval(intervalMs);
    if (!m_timer.isActive())
        m_timer.start();
}

void Store::stopPolling()
{
    m_timer.stop();
}

AgentAdapter* Store::adapter(const QString& id) const
{
    for (AgentAdapter* a : m_adapters)
        if (a->id() == id)
            return a;
    return nullptr;
}

QVector<types::AgentStatus> Store::agentStatuses() const
{
    // served from the scan() cache; the GUI thread must never walk dirs.
    // Guarded copy: scan() rebuilds this vector on a pool thread every 2s.
    {
        QMutexLocker lock(&m_statusMutex);
        if (!m_statusCache.isEmpty())
            return m_statusCache;
    }
    QVector<types::AgentStatus> out;
    for (AgentAdapter* a : m_adapters)
        out.push_back(a->status()); // pre-first-scan fallback only
    return out;
}

qint64 Store::lastScanMs() const
{
    QMutexLocker lock(&m_statusMutex);
    return m_lastScanMs;
}

// ───────────────────────── scan ─────────────────────────

Store::ScanResult Store::scan()
{
    ScanResult res;
    QElapsedTimer timer;
    timer.start();
    if (!openCache(&res.err)) {
        return res;
    }
    QSqlDatabase db = connection();
    if (!db.isOpen()) {
        res.err = db.lastError().text();
        return res;
    }

    QVector<types::AgentStatus> statusNext; // swapped into m_statusCache at the end
    for (AgentAdapter* ad : m_adapters) {
        const QString agent = ad->id();
        QString note;
        const QVector<SessionFile> listed = ad->listSessionFiles(&note);
        res.totalFiles += listed.size();
        if (listed.isEmpty() && !note.isEmpty())
            res.err = note; // last adapter note wins; purely informational

        // 状态直接由本轮已枚举的文件构造 —— status() 会为每个 agent 再走
        // 一遍完整文件树，2s 轮询等于把目录遍历做两遍
        {
            types::AgentStatus st;
            st.id = agent;
            st.name = ad->displayName();
            st.color = ad->color();
            QString dir;
            st.found = ad->dataPresent(&dir);
            st.dataDir = dir;
            st.sessions = st.found ? listed.size() : 0;
            st.note = !st.found
                ? QStringLiteral("未检测到本地数据目录")
                : (note.isEmpty() ? QStringLiteral("数据正常（只读观测）") : note);
            for (const SessionFile& f : listed)
                st.lastActivityMs = qMax(st.lastActivityMs, f.mtimeMs);
            statusNext.push_back(st);
        }

        // known file states for this agent
        QHash<QString, QPair<qint64, qint64>> known; // path → (mtime, size)
        {
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "SELECT path, mtime, size FROM files WHERE agent = ?"));
            q.addBindValue(agent);
            q.exec();
            while (q.next())
                known.insert(q.value(0).toString(),
                             { q.value(1).toLongLong(), q.value(2).toLongLong() });
        }

        QSet<QString> listedPaths;
        for (const SessionFile& f : listed) {
            listedPaths.insert(f.path);
            const auto it = known.constFind(f.path);
            if (it != known.constEnd() && it->first == f.mtimeMs
                && it->second == f.size)
                continue; // unchanged

            types::SessionInfo meta;
            QVector<types::UsageRecord> records;
            QHash<QString, ToolStat> tools;
            if (!ad->parseFile(f, meta, records, &tools))
                continue; // unreadable — skip (kept out of cache)

            QSqlQuery q(db);
            q.exec(QStringLiteral("BEGIN IMMEDIATE TRANSACTION"));
            q.prepare(QStringLiteral("DELETE FROM responses WHERE agent = ? AND session_id = ?"));
            q.addBindValue(agent);
            q.addBindValue(meta.sessionId);
            q.exec();
            q.prepare(QStringLiteral("DELETE FROM sessions WHERE agent = ? AND session_id = ?"));
            q.addBindValue(agent);
            q.addBindValue(meta.sessionId);
            q.exec();
            q.prepare(QStringLiteral("DELETE FROM tools WHERE agent = ? AND session_id = ?"));
            q.addBindValue(agent);
            q.addBindValue(meta.sessionId);
            q.exec();

            q.prepare(QStringLiteral(
                "INSERT INTO sessions(agent, session_id, path, title, cwd, model,"
                " originator, started, updated, responses, tool_calls, errors,"
                " in_tok, cached_tok, cache_write_tok, out_tok, reason_tok,"
                " context_window)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
            q.addBindValue(agent);
            q.addBindValue(meta.sessionId);
            q.addBindValue(meta.filePath);
            q.addBindValue(meta.title);
            q.addBindValue(meta.cwd);
            q.addBindValue(meta.model);
            q.addBindValue(meta.originator);
            q.addBindValue(meta.startedMs);
            q.addBindValue(meta.updatedMs);
            q.addBindValue(meta.responses);
            q.addBindValue(meta.toolCalls);
            q.addBindValue(meta.errors);
            q.addBindValue(meta.inputTokens);
            q.addBindValue(meta.cachedTokens);
            q.addBindValue(meta.cacheWriteTokens);
            q.addBindValue(meta.outputTokens);
            q.addBindValue(meta.reasoningTokens);
            q.addBindValue(meta.contextWindow);
            q.exec();

            q.prepare(QStringLiteral(
                "INSERT INTO responses(agent, session_id, turn_id, response_id,"
                " time_ms, dur_ok, dur_approx, dur_ms, model, effort, source,"
                " in_tok, cached_tok, cache_write_tok, out_tok, reason_tok,"
                " tps_ok, tps, error)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
            for (const types::UsageRecord& r : records) {
                q.addBindValue(agent);
                q.addBindValue(r.sessionId);
                q.addBindValue(r.turnId);
                q.addBindValue(r.responseId);
                q.addBindValue(r.timeMs);
                q.addBindValue(r.durOk ? 1 : 0);
                q.addBindValue(r.durApprox ? 1 : 0);
                q.addBindValue(r.durationMs);
                q.addBindValue(r.model);
                q.addBindValue(r.effort);
                q.addBindValue(r.source);
                q.addBindValue(r.inputTokens);
                q.addBindValue(r.cachedTokens);
                q.addBindValue(r.cacheWriteTokens);
                q.addBindValue(r.outputTokens);
                q.addBindValue(r.reasoningTokens);
                q.addBindValue(r.tpsOk ? 1 : 0);
                q.addBindValue(r.tps);
                q.addBindValue(r.error);
                q.exec();
            }

            if (!tools.isEmpty()) {
                q.prepare(QStringLiteral(
                    "INSERT INTO tools(agent, session_id, name, calls,"
                    " first_ms, last_ms) VALUES(?,?,?,?,?,?)"));
                for (auto it = tools.constBegin(); it != tools.constEnd(); ++it) {
                    q.addBindValue(agent);
                    q.addBindValue(meta.sessionId);
                    q.addBindValue(it.key());
                    q.addBindValue(it.value().calls);
                    q.addBindValue(it.value().firstMs);
                    q.addBindValue(it.value().lastMs);
                    q.exec();
                }
            }

            q.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO files(path, agent, mtime, size)"
                " VALUES(?,?,?,?)"));
            q.addBindValue(f.path);
            q.addBindValue(agent);
            q.addBindValue(f.mtimeMs);
            q.addBindValue(f.size);
            q.exec();
            q.exec(QStringLiteral("COMMIT"));
            res.changedFiles++; // parsed + stored — now it counts
        }

        // files vanished since the last scan (cleaned up sessions).
        // Guard: an empty listing while the source itself is present and we
        // previously knew files means the source failed to enumerate (e.g.
        // the ZCode db locked) — never mass-delete on that.
        const bool trustListing =
            !(listed.isEmpty() && ad->dataPresent() && !known.isEmpty());
        if (!trustListing)
            continue;
        for (auto it = known.constBegin(); it != known.constEnd(); ++it) {
            if (listedPaths.contains(it.key()))
                continue;
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "SELECT session_id FROM sessions WHERE agent = ? AND path = ?"));
            q.addBindValue(agent);
            q.addBindValue(it.key());
            q.exec();
            while (q.next()) {
                const QString sid = q.value(0).toString();
                QSqlQuery d(db);
                d.prepare(QStringLiteral("DELETE FROM responses WHERE agent = ? AND session_id = ?"));
                d.addBindValue(agent);
                d.addBindValue(sid);
                d.exec();
                d.prepare(QStringLiteral("DELETE FROM sessions WHERE agent = ? AND session_id = ?"));
                d.addBindValue(agent);
                d.addBindValue(sid);
                d.exec();
                d.prepare(QStringLiteral("DELETE FROM tools WHERE agent = ? AND session_id = ?"));
                d.addBindValue(agent);
                d.addBindValue(sid);
                d.exec();
            }
            QSqlQuery d(db);
            d.prepare(QStringLiteral("DELETE FROM files WHERE path = ?"));
            d.addBindValue(it.key());
            d.exec();
            res.changedFiles++;
        }
    }

    // live-feed diff: records committed after the watermark. First scan only
    // sets the watermark (never replays history).
    if (!m_watermarkInit) {
        QSqlQuery q(db);
        q.exec(QStringLiteral("SELECT MAX(time_ms) FROM responses"));
        m_watermarkMs = q.next() ? q.value(0).toLongLong() : 0;
        m_watermarkInit = true;
    } else if (m_watermarkMs > 0) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT agent, session_id, turn_id, response_id, time_ms, dur_ok,"
            " dur_approx, dur_ms, model, effort, source, in_tok, cached_tok,"
            " cache_write_tok, out_tok, reason_tok, tps_ok, tps, error"
            " FROM responses WHERE time_ms > ? ORDER BY time_ms LIMIT 200"));
        q.addBindValue(m_watermarkMs);
        q.exec();
        while (q.next()) {
            types::UsageRecord r;
            r.agentId = q.value(0).toString();
            r.sessionId = q.value(1).toString();
            r.turnId = q.value(2).toString();
            r.responseId = q.value(3).toString();
            r.timeMs = q.value(4).toLongLong();
            r.durOk = q.value(5).toInt() != 0;
            r.durApprox = q.value(6).toInt() != 0;
            r.durationMs = q.value(7).toDouble();
            r.model = q.value(8).toString();
            r.effort = q.value(9).toString();
            r.source = q.value(10).toString();
            r.inputTokens = q.value(11).toLongLong();
            r.cachedTokens = q.value(12).toLongLong();
            r.cacheWriteTokens = q.value(13).toLongLong();
            r.outputTokens = q.value(14).toLongLong();
            r.reasoningTokens = q.value(15).toLongLong();
            r.tpsOk = q.value(16).toInt() != 0;
            r.tps = q.value(17).toDouble();
            r.error = q.value(18).toString();
            res.freshRecords.push_back(r);
            m_watermarkMs = qMax(m_watermarkMs, r.timeMs);
        }
    }

    // merge the scan into the self-owned binary archive (survives source
    // deletion) — merge-only, never removes recorded history. A no-op scan
    // must not re-run the whole GROUP BY + serialize + SHA256 every 2s;
    // an empty archive still merges so a deleted bin file rebuilds.
    const qint64 beforeMerge = timer.elapsed();
    if (res.changedFiles > 0 || archiveEntryCount() == 0)
        mergeArchiveFromCache();

    // reported scan cost includes the archive merge — the topbar number is
    // the true cost of one poll tick, not just the directory walk
    {
        QMutexLocker lock(&m_statusMutex);
        m_statusCache = statusNext;
        m_lastScanMs = timer.elapsed();
    }

    res.ok = true;
    return res;
}

void Store::pollTick()
{
    if (m_pollPaused.loadRelaxed())
        return; // 最小化中：不启动新扫描（正在跑的那次照常完成）
    if (!m_scanBusy.testAndSetOrdered(0, 1))
        return;
    Async::run<ScanResult>(
        this,
        []() { return Store::instance().scan(); },
        [this](const ScanResult& r) {
            m_scanBusy.storeRelaxed(0);
            emit scanFinished(r.ok, r.err, r.changedFiles, r.totalFiles);
            if (!r.freshRecords.isEmpty())
                emit newRecords(r.freshRecords);
            if (r.ok && r.changedFiles > 0)
                emit dataChanged();
        });
}

// ───────────────────────── queries ─────────────────────────

qint64 Store::windowSince(const QString& window)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (window == QLatin1String("today")) {
        const QDateTime d = QDateTime::currentDateTime();
        QDateTime midnight(d.date(), QTime(0, 0));
        return midnight.toMSecsSinceEpoch();
    }
    if (window == QLatin1String("24h"))
        return now - 24 * 3600 * 1000LL;
    if (window == QLatin1String("7d"))
        return now - 7 * 24 * 3600 * 1000LL;
    if (window == QLatin1String("30d"))
        return now - 30 * 24 * 3600 * 1000LL;
    return 0; // "all"
}

types::OverviewData Store::overview(const QString& window)
{
    types::OverviewData d;
    d.window = window;
    d.sinceMs = windowSince(window);
    QSqlDatabase db = connection();

    const auto runKpis = [&](qint64 since, types::Kpis& k) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT COUNT(*), COUNT(DISTINCT session_id),"
            " SUM(CASE WHEN error IS NOT NULL AND error != '' THEN 1 ELSE 0 END),"
            " SUM(in_tok), SUM(out_tok), SUM(reason_tok), SUM(cached_tok),"
            " SUM(cache_write_tok),"
            " AVG(CASE WHEN dur_ok=1 THEN dur_ms END),"
            " SUM(CASE WHEN tps_ok=1 THEN out_tok + reason_tok ELSE 0 END),"
            " SUM(CASE WHEN tps_ok=1 THEN dur_ms ELSE 0 END),"
            " SUM(CASE WHEN tps_ok=1 THEN 1 ELSE 0 END)"
            " FROM responses WHERE time_ms >= ?"));
        q.addBindValue(since);
        q.exec();
        if (q.next()) {
            k.requests = q.value(0).toLongLong();
            k.sessions = q.value(1).toLongLong();
            k.errors = q.value(2).toLongLong();
            k.inTok = q.value(3).toLongLong();
            k.outTok = q.value(4).toLongLong();
            k.reasonTok = q.value(5).toLongLong();
            k.cacheRead = q.value(6).toLongLong();
            k.cacheWrite = q.value(7).toLongLong();
            k.avgDurOk = !q.value(8).isNull();
            k.avgDurMs = q.value(8).toDouble();
            const double gen = q.value(9).toDouble();
            const double durMs = q.value(10).toDouble();
            k.tpsSamples = q.value(11).toLongLong();
            k.tpsOk = durMs > 0;
            k.weightedTps = k.tpsOk ? gen / (durMs / 1000.0) : 0;
        }
    };
    if (d.sinceMs > 0)
        runKpis(d.sinceMs, d.kpis);
    else
        runKpis(0, d.kpis);
    runKpis(windowSince(QStringLiteral("today")), d.todayKpis);

    // series (bucket size per window)
    qint64 bucket = 3600 * 1000LL;
    if (window == QLatin1String("7d"))
        bucket = 6 * 3600 * 1000LL;
    else if (window == QLatin1String("30d") || window == QLatin1String("all"))
        bucket = 24 * 3600 * 1000LL;
    {
        QSqlQuery q(db);
        // LOCAL-aligned buckets (same tz shift as the daily aggregates):
        // UTC-aligned 24h buckets would start at 08:00 local in UTC+8,
        // splitting every local day in half. bucket/offset are internal
        // constants (never user input) → inlined.
        const qint64 tz = qint64(QDateTime::currentDateTime().offsetFromUtc()) * 1000;
        const QString b = QString::number(bucket);
        const QString tzs = QString::number(tz);
        if (d.sinceMs > 0) {
            q.prepare(QStringLiteral(
                "SELECT ((time_ms + %1) / %2) * %2 - %1 AS b, COUNT(*), SUM(in_tok),"
                " SUM(out_tok), SUM(reason_tok) FROM responses"
                " WHERE time_ms >= ? GROUP BY b ORDER BY b").arg(tzs, b));
            q.addBindValue(d.sinceMs);
        } else {
            q.prepare(QStringLiteral(
                "SELECT ((time_ms + %1) / %2) * %2 - %1 AS b, COUNT(*), SUM(in_tok),"
                " SUM(out_tok), SUM(reason_tok) FROM responses"
                " GROUP BY b ORDER BY b").arg(tzs, b));
        }
        q.exec();
        while (q.next()) {
            types::SeriesPoint p;
            p.bucketMs = q.value(0).toLongLong();
            p.requests = q.value(1).toLongLong();
            p.input = q.value(2).toLongLong();
            p.output = q.value(3).toLongLong();
            p.reasoning = q.value(4).toLongLong();
            d.series.push_back(p);
        }
    }

    // by model (+effort) — top 12 by total tokens; runs twice (window +
    // fixed today for the left big card)
    const auto fetchByModel = [&](qint64 since, QVector<types::ModelBreakdown>& out) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT agent, model, effort, COUNT(*), SUM(in_tok), SUM(cached_tok),"
            " SUM(out_tok), SUM(reason_tok),"
            " AVG(CASE WHEN dur_ok=1 THEN dur_ms END),"
            " SUM(CASE WHEN tps_ok=1 THEN out_tok + reason_tok ELSE 0 END),"
            " SUM(CASE WHEN tps_ok=1 THEN dur_ms ELSE 0 END),"
            " MAX(time_ms), SUM(in_tok + out_tok + reason_tok) AS tot"
            " FROM responses WHERE time_ms >= ?"
            " GROUP BY agent, model, effort ORDER BY tot DESC LIMIT 12"));
        q.addBindValue(since > 0 ? since : 0);
        q.exec();
        while (q.next()) {
            types::ModelBreakdown m;
            m.agentId = q.value(0).toString();
            m.model = q.value(1).toString();
            m.effort = q.value(2).toString();
            m.requests = q.value(3).toLongLong();
            m.inTok = q.value(4).toLongLong();
            m.cachedTok = q.value(5).toLongLong();
            m.outTok = q.value(6).toLongLong();
            m.reasonTok = q.value(7).toLongLong();
            m.avgDurOk = !q.value(8).isNull();
            m.avgDurMs = q.value(8).toDouble();
            const double gen = q.value(9).toDouble();
            const double durMs = q.value(10).toDouble();
            m.tpsOk = durMs > 0;
            m.weightedTps = m.tpsOk ? gen / (durMs / 1000.0) : 0;
            m.lastMs = q.value(11).toLongLong();
            out.push_back(m);
        }
    };
    fetchByModel(d.sinceMs > 0 ? d.sinceMs : 0, d.byModel);
    fetchByModel(windowSince(QStringLiteral("today")), d.todayByModel);

    // by agent
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT agent, COUNT(*), SUM(in_tok), SUM(cached_tok), SUM(out_tok),"
            " SUM(reason_tok) FROM responses WHERE time_ms >= ? GROUP BY agent"));
        q.addBindValue(d.sinceMs > 0 ? d.sinceMs : 0);
        q.exec();
        while (q.next()) {
            types::AgentBreakdown a;
            a.agentId = q.value(0).toString();
            a.requests = q.value(1).toLongLong();
            a.inTok = q.value(2).toLongLong();
            a.cachedTok = q.value(3).toLongLong();
            a.outTok = q.value(4).toLongLong();
            a.reasonTok = q.value(5).toLongLong();
            d.byAgent.push_back(a);
        }
    }

    // by tool (top 30) — filtered by tool TIME, not session membership:
    // a long-lived session would otherwise count ALL its tool calls into
    // every window
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT t.agent, t.name, SUM(t.calls) AS calls,"
            " COUNT(DISTINCT t.session_id) FROM tools t WHERE t.last_ms >= ?"
            " GROUP BY t.agent, t.name ORDER BY calls DESC LIMIT 30"));
        q.addBindValue(d.sinceMs > 0 ? d.sinceMs : 0);
        q.exec();
        while (q.next()) {
            types::ToolBreakdown t;
            t.agentId = q.value(0).toString();
            t.toolName = q.value(1).toString();
            t.calls = q.value(2).toLongLong();
            t.sessions = q.value(3).toLongLong();
            d.byTool.push_back(t);
        }
    }

    // recent completed requests (speed table) — all families; the page
    // re-queries with a family filter from the combo
    d.recentSpeed = recentSpeed(d.sinceMs, QString());
    return d;
}

QVector<types::SpeedRow> Store::recentSpeed(qint64 sinceMs, const QString& family,
                                            int limit)
{
    QVector<types::SpeedRow> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    if (family.isEmpty()) {
        q.prepare(QStringLiteral(
            "SELECT time_ms, agent, model, session_id, out_tok, reason_tok,"
            " dur_ok, dur_ms, tps_ok, tps FROM responses"
            " WHERE tps_ok = 1 AND time_ms >= :since"
            " ORDER BY time_ms DESC LIMIT :lim"));
        q.bindValue(QStringLiteral(":since"), sinceMs > 0 ? sinceMs : 0);
        q.bindValue(QStringLiteral(":lim"), limit);
    } else {
        // family 大类过滤：lower(model) LIKE 'deepseek-%' 覆盖该大类下
        // 所有具体型号（deepseek-flash / DeepSeek-V4-Pro / …）；
        // 恰好等于大类名的裸模型（无 - 变体）走 OR 精确匹配
        q.prepare(QStringLiteral(
            "SELECT time_ms, agent, model, session_id, out_tok, reason_tok,"
            " dur_ok, dur_ms, tps_ok, tps FROM responses"
            " WHERE tps_ok = 1 AND time_ms >= :since"
            "   AND (lower(model) LIKE :famPat OR lower(model) = :fam)"
            " ORDER BY time_ms DESC LIMIT :lim"));
        q.bindValue(QStringLiteral(":since"), sinceMs > 0 ? sinceMs : 0);
        q.bindValue(QStringLiteral(":famPat"), family + QStringLiteral("-%"));
        q.bindValue(QStringLiteral(":fam"), family);
        q.bindValue(QStringLiteral(":lim"), limit);
    }
    q.exec();
    while (q.next()) {
        types::SpeedRow r;
        r.timeMs = q.value(0).toLongLong();
        r.agentId = q.value(1).toString();
        r.model = q.value(2).toString();
        r.sessionId = q.value(3).toString();
        r.output = q.value(4).toLongLong();
        r.reasoning = q.value(5).toLongLong();
        r.durOk = q.value(6).toInt() != 0;
        r.durationMs = q.value(7).toDouble();
        r.tpsOk = q.value(8).toInt() != 0;
        r.tps = q.value(9).toDouble();
        out.push_back(r);
    }
    return out;
}

QVector<types::SessionInfo> Store::sessionList(const QString& agent,
                                               const QString& search, int limit)
{
    QVector<types::SessionInfo> out;
    QSqlDatabase db = connection();
    QString where;
    if (!agent.isEmpty())
        where += QStringLiteral("agent = :agent");
    if (!search.isEmpty()) {
        if (!where.isEmpty())
            where += QStringLiteral(" AND ");
        where += QStringLiteral(
            "(title LIKE :s OR cwd LIKE :s OR model LIKE :s"
            " OR session_id LIKE :s)");
    }
    QSqlQuery q(db);
    QString sql = QStringLiteral(
        "SELECT agent, session_id, path, title, cwd, model, originator,"
        " started, updated, responses, tool_calls, errors, in_tok, cached_tok,"
        " cache_write_tok, out_tok, reason_tok, context_window FROM sessions");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where;
    sql += QStringLiteral(" ORDER BY updated DESC LIMIT :lim");
    q.prepare(sql);
    if (!agent.isEmpty())
        q.bindValue(QStringLiteral(":agent"), agent);
    if (!search.isEmpty())
        q.bindValue(QStringLiteral(":s"), QStringLiteral("%") + search + QStringLiteral("%"));
    q.bindValue(QStringLiteral(":lim"), limit);
    q.exec();
    while (q.next()) {
        types::SessionInfo s;
        s.agentId = q.value(0).toString();
        s.sessionId = q.value(1).toString();
        s.filePath = q.value(2).toString();
        s.title = q.value(3).toString();
        s.cwd = q.value(4).toString();
        s.model = q.value(5).toString();
        s.originator = q.value(6).toString();
        s.startedMs = q.value(7).toLongLong();
        s.updatedMs = q.value(8).toLongLong();
        s.responses = q.value(9).toLongLong();
        s.toolCalls = q.value(10).toLongLong();
        s.errors = q.value(11).toLongLong();
        s.inputTokens = q.value(12).toLongLong();
        s.cachedTokens = q.value(13).toLongLong();
        s.cacheWriteTokens = q.value(14).toLongLong();
        s.outputTokens = q.value(15).toLongLong();
        s.reasoningTokens = q.value(16).toLongLong();
        s.contextWindow = q.value(17).toLongLong();
        out.push_back(s);
    }
    return out;
}

types::SessionInfo Store::sessionInfo(const QString& agent, const QString& sessionId)
{
    types::SessionInfo s;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT path, title, cwd, model, originator, started, updated,"
        " responses, tool_calls, errors, in_tok, cached_tok, cache_write_tok,"
        " out_tok, reason_tok, context_window FROM sessions"
        " WHERE agent = ? AND session_id = ?"));
    q.addBindValue(agent);
    q.addBindValue(sessionId);
    q.exec();
    if (q.next()) {
        s.agentId = agent;
        s.sessionId = sessionId;
        s.filePath = q.value(0).toString();
        s.title = q.value(1).toString();
        s.cwd = q.value(2).toString();
        s.model = q.value(3).toString();
        s.originator = q.value(4).toString();
        s.startedMs = q.value(5).toLongLong();
        s.updatedMs = q.value(6).toLongLong();
        s.responses = q.value(7).toLongLong();
        s.toolCalls = q.value(8).toLongLong();
        s.errors = q.value(9).toLongLong();
        s.inputTokens = q.value(10).toLongLong();
        s.cachedTokens = q.value(11).toLongLong();
        s.cacheWriteTokens = q.value(12).toLongLong();
        s.outputTokens = q.value(13).toLongLong();
        s.reasoningTokens = q.value(14).toLongLong();
        s.contextWindow = q.value(15).toLongLong();
        s.title = s.title.isEmpty() ? sessionId.left(12) : s.title;
    }
    return s;
}

QVector<types::UsageRecord> Store::sessionRecords(const QString& agent,
                                                  const QString& sessionId)
{
    QVector<types::UsageRecord> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT turn_id, response_id, time_ms, dur_ok, dur_approx, dur_ms,"
        " model, effort, source, in_tok, cached_tok, cache_write_tok,"
        " out_tok, reason_tok, tps_ok, tps, error FROM responses"
        " WHERE agent = ? AND session_id = ? ORDER BY time_ms"));
    q.addBindValue(agent);
    q.addBindValue(sessionId);
    q.exec();
    while (q.next()) {
        types::UsageRecord r;
        r.agentId = agent;
        r.sessionId = sessionId;
        r.turnId = q.value(0).toString();
        r.responseId = q.value(1).toString();
        r.timeMs = q.value(2).toLongLong();
        r.durOk = q.value(3).toInt() != 0;
        r.durApprox = q.value(4).toInt() != 0;
        r.durationMs = q.value(5).toDouble();
        r.model = q.value(6).toString();
        r.effort = q.value(7).toString();
        r.source = q.value(8).toString();
        r.inputTokens = q.value(9).toLongLong();
        r.cachedTokens = q.value(10).toLongLong();
        r.cacheWriteTokens = q.value(11).toLongLong();
        r.outputTokens = q.value(12).toLongLong();
        r.reasoningTokens = q.value(13).toLongLong();
        r.tpsOk = q.value(14).toInt() != 0;
        r.tps = q.value(15).toDouble();
        r.error = q.value(16).toString();
        out.push_back(r);
    }
    return out;
}

bool Store::sessionMessages(const QString& agent, const QString& sessionId,
                            QVector<types::ChatMessage>& outMessages, QString* err)
{
    // conversation replay reads the SOURCE file on demand — find its path
    const types::SessionInfo info = sessionInfo(agent, sessionId);
    if (info.filePath.isEmpty()) {
        if (err)
            *err = QStringLiteral("会话不存在");
        return false;
    }
    AgentAdapter* ad = adapter(agent);
    if (!ad) {
        if (err)
            *err = QStringLiteral("未知 agent");
        return false;
    }
    SessionFile f;
    f.path = info.filePath;
    QFileInfo fi(info.filePath);
    f.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    f.size = fi.size();
    if (!ad->parseMessages(f, outMessages)) {
        if (err)
            *err = QStringLiteral("该 agent 暂不支持对话重放");
        return false;
    }
    return true;
}

QVector<types::ToolBreakdown> Store::toolBreakdown(qint64 sinceMs, int limit)
{
    QVector<types::ToolBreakdown> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    // all-named binds: mixing ? and :name misroutes values in Qt's sqlite driver
    q.prepare(QStringLiteral(
        "SELECT t.agent, t.name, SUM(t.calls) AS calls, COUNT(DISTINCT t.session_id)"
        " FROM tools t WHERE t.last_ms >= :since"
        " GROUP BY t.agent, t.name ORDER BY calls DESC LIMIT :lim"));
    q.bindValue(QStringLiteral(":since"), sinceMs > 0 ? sinceMs : 0);
    q.bindValue(QStringLiteral(":lim"), limit);
    q.exec();
    while (q.next()) {
        types::ToolBreakdown t;
        t.agentId = q.value(0).toString();
        t.toolName = q.value(1).toString();
        t.calls = q.value(2).toLongLong();
        t.sessions = q.value(3).toLongLong();
        out.push_back(t);
    }
    return out;
}

QVector<types::ModelBreakdown> Store::modelBreakdown(qint64 sinceMs)
{
    QVector<types::ModelBreakdown> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT agent, model, effort, COUNT(*), SUM(in_tok), SUM(cached_tok),"
        " SUM(out_tok), SUM(reason_tok),"
        " AVG(CASE WHEN dur_ok=1 THEN dur_ms END),"
        " SUM(CASE WHEN tps_ok=1 THEN out_tok + reason_tok ELSE 0 END),"
        " SUM(CASE WHEN tps_ok=1 THEN dur_ms ELSE 0 END),"
        " MAX(time_ms), SUM(in_tok + out_tok + reason_tok) AS tot"
        " FROM responses WHERE time_ms >= ?"
        " GROUP BY agent, model, effort ORDER BY tot DESC"));
    q.addBindValue(sinceMs > 0 ? sinceMs : 0);
    q.exec();
    while (q.next()) {
        types::ModelBreakdown m;
        m.agentId = q.value(0).toString();
        m.model = q.value(1).toString();
        m.effort = q.value(2).toString();
        m.requests = q.value(3).toLongLong();
        m.inTok = q.value(4).toLongLong();
        m.cachedTok = q.value(5).toLongLong();
        m.outTok = q.value(6).toLongLong();
        m.reasonTok = q.value(7).toLongLong();
        m.avgDurOk = !q.value(8).isNull();
        m.avgDurMs = q.value(8).toDouble();
        const double gen = q.value(9).toDouble();
        const double durMs = q.value(10).toDouble();
        m.tpsOk = durMs > 0;
        m.weightedTps = m.tpsOk ? gen / (durMs / 1000.0) : 0;
        m.lastMs = q.value(11).toLongLong();
        out.push_back(m);
    }
    return out;
}

QVector<types::AgentBreakdown> Store::agentBreakdown(qint64 sinceMs)
{
    QVector<types::AgentBreakdown> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT agent, COUNT(*), SUM(in_tok), SUM(cached_tok), SUM(out_tok),"
        " SUM(reason_tok) FROM responses WHERE time_ms >= ? GROUP BY agent"
        " ORDER BY SUM(in_tok + out_tok + reason_tok) DESC"));
    q.addBindValue(sinceMs > 0 ? sinceMs : 0);
    q.exec();
    while (q.next()) {
        types::AgentBreakdown a;
        a.agentId = q.value(0).toString();
        a.requests = q.value(1).toLongLong();
        a.inTok = q.value(2).toLongLong();
        a.cachedTok = q.value(3).toLongLong();
        a.outTok = q.value(4).toLongLong();
        a.reasonTok = q.value(5).toLongLong();
        out.push_back(a);
    }
    return out;
}

QVector<Store::ModelDayPoint> Store::modelDailySeries(qint64 sinceMs, int topModels)
{
    // rank models by generated tokens, keep the top N
    QVector<QPair<QString, qint64>> ranked;
    {
        QSqlDatabase db = connection();
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT model, SUM(out_tok + reason_tok) AS g FROM responses"
            " WHERE time_ms >= ? AND model != '' GROUP BY model ORDER BY g DESC"));
        q.addBindValue(sinceMs > 0 ? sinceMs : 0);
        q.exec();
        while (q.next() && ranked.size() < topModels)
            ranked.push_back({ q.value(0).toString(), q.value(1).toLongLong() });
    }
    if (ranked.isEmpty())
        return {};
    QSet<QString> keep;
    for (const auto& r : ranked)
        keep.insert(r.first);

    QVector<ModelDayPoint> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT (time_ms/86400000)*86400000 AS d, model, SUM(out_tok + reason_tok)"
        " FROM responses WHERE time_ms >= ? GROUP BY d, model ORDER BY d"));
    q.addBindValue(sinceMs > 0 ? sinceMs : 0);
    q.exec();
    while (q.next()) {
        ModelDayPoint p;
        p.dayMs = q.value(0).toLongLong();
        p.model = q.value(1).toString();
        p.genTok = q.value(2).toLongLong();
        if (keep.contains(p.model))
            out.push_back(p);
    }
    return out;
}

QString Store::archivePath()
{
    const QByteArray env = qgetenv("AGENT_MONITOR_ARCHIVE");
    if (!env.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(env));
    // next to the executable — the project's bin folder
    return QCoreApplication::applicationDirPath()
         + QStringLiteral("/usage-archive.bin");
}

void Store::loadArchive()
{
    QMutexLocker lock(&m_archiveMutex);
    loadArchiveLocked();
}

void Store::loadArchiveLocked()
{
    if (m_archiveLoaded)
        return;
    m_archiveLoaded = true;
    QFile f(archivePath());
    if (!f.open(QIODevice::ReadOnly))
        return; // first run — the archive builds up from the cache
    QDataStream in(&f);
    in.setByteOrder(QDataStream::LittleEndian);
    char magic[8];
    if (in.readRawData(magic, 8) != 8
        || qstrncmp(magic, kArchiveMagic, 8) != 0)
        return; // unknown file — ignored, rebuilt from the cache
    quint32 version = 0, count = 0;
    in >> version;
    if (version != kArchiveVersion)
        return;
    in >> count;
    for (quint32 i = 0; i < count && !in.atEnd(); ++i) {
        ArchiveEntry e;
        e.agent = getStr(in);
        e.sessionId = getStr(in);
        e.model = getStr(in);
        e.effort = getStr(in);
        in >> e.dayMs >> e.requests >> e.errors >> e.inTok >> e.outTok
           >> e.reasonTok >> e.cachedTok >> e.cacheWriteTok >> e.durSumMs
           >> e.tpsGenTok;
        m_archive.insert(archiveKey(e.agent, e.sessionId, e.model, e.effort,
                                    e.dayMs),
                         e);
    }
    // a half-written tail is ignored; the next merge rewrites the file
}

void Store::mergeArchiveFromCache()
{
    // hold the lock for the whole merge: UI threads read the archive (filters,
    // stats queries) concurrently — QHash read/write races are UB
    QMutexLocker lock(&m_archiveMutex);
    loadArchiveLocked();
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    const qint64 tz = qint64(QDateTime::currentDateTime().offsetFromUtc()) * 1000;
    const QString tzs = QString::number(tz);
    q.prepare(QStringLiteral(
        "SELECT agent, session_id, model, effort,"
        " (time_ms + %1) / 86400000 * 86400000 - %1 AS d,"
        " COUNT(*), SUM(CASE WHEN error IS NOT NULL AND error != ''"
        "   THEN 1 ELSE 0 END),"
        " SUM(in_tok), SUM(out_tok), SUM(reason_tok), SUM(cached_tok),"
        " SUM(cache_write_tok),"
        " SUM(CASE WHEN dur_ok=1 THEN dur_ms ELSE 0 END),"
        " SUM(CASE WHEN tps_ok=1 THEN out_tok + reason_tok ELSE 0 END)"
        " FROM responses GROUP BY agent, session_id, model, effort, d").arg(tzs));
    if (!q.exec())
        return;
    bool anyRow = false;
    while (q.next()) {
        ArchiveEntry e;
        e.agent = q.value(0).toString();
        e.sessionId = q.value(1).toString();
        e.model = q.value(2).toString();
        e.effort = q.value(3).toString();
        e.dayMs = q.value(4).toLongLong();
        e.requests = q.value(5).toLongLong();
        e.errors = q.value(6).toLongLong();
        e.inTok = q.value(7).toLongLong();
        e.outTok = q.value(8).toLongLong();
        e.reasonTok = q.value(9).toLongLong();
        e.cachedTok = q.value(10).toLongLong();
        e.cacheWriteTok = q.value(11).toLongLong();
        e.durSumMs = qint64(q.value(12).toDouble());
        e.tpsGenTok = q.value(13).toLongLong();
        m_archive.insert(archiveKey(e.agent, e.sessionId, e.model, e.effort,
                                    e.dayMs),
                         e);
        anyRow = true;
    }
    // CRITICAL: an empty/unreadable cache (just wiped, source briefly
    // locked, load failure) must NEVER overwrite the archive with nothing —
    // merge-only upserts, zero rows means keep whatever the archive holds
    if (!anyRow)
        return;

    // retention: drop entries older than the configured window (default
    // 2 years — keeps the bin file small without losing recent history)
    if (m_retentionDays > 0) {
        const qint64 cutoff =
            QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch()
            - m_retentionDays * 86400000LL;
        for (auto it = m_archive.begin(); it != m_archive.end();) {
            if (it.value().dayMs < cutoff)
                it = m_archive.erase(it);
            else
                ++it;
        }
    }

    // serialize + write only when the content changed (2s poll cadence)
    QByteArray blob;
    QDataStream out(&blob, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData(kArchiveMagic, 8);
    out << kArchiveVersion << quint32(m_archive.size());
    for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it) {
        const ArchiveEntry& e = it.value();
        putStr(out, e.agent);
        putStr(out, e.sessionId);
        putStr(out, e.model);
        putStr(out, e.effort);
        // every integer 8 bytes (qint64) — the reader reads them as qint64
        out << e.dayMs << qint64(e.requests) << qint64(e.errors) << e.inTok
            << e.outTok << e.reasonTok << e.cachedTok << e.cacheWriteTok
            << e.durSumMs << e.tpsGenTok;
    }
    const QByteArray hash = QCryptographicHash::hash(
        blob, QCryptographicHash::Sha256);
    if (hash == m_archiveHash)
        return; // nothing new
    m_archiveHash = hash;
    const QString path = archivePath();
    const QString tmp = path + QStringLiteral(".tmp");
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(blob);
    }
    QFile::remove(path);
    QFile::rename(tmp, path);
}

int Store::archiveEntryCount()
{
    QMutexLocker lock(&m_archiveMutex);
    return int(m_archive.size());
}

QStringList Store::archiveAgentIds()
{
    QMutexLocker lock(&m_archiveMutex);
    if (!m_archive.isEmpty()) {
        QSet<QString> ids;
        for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it)
            ids.insert(it.value().agent);
        QStringList out;
        for (const QString& s : ids)
            out << s;
        std::sort(out.begin(), out.end());
        return out;
    }
    QStringList out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT DISTINCT agent FROM responses"))) {
        while (q.next())
            out << q.value(0).toString();
    }
    std::sort(out.begin(), out.end());
    return out;
}

QVector<Store::DailyTotal> Store::dailyTotals(const QString& agent,
                                              const QString& family)
{
    const bool hasAgent = !agent.isEmpty();
    const bool hasFamily = !family.isEmpty();

    // archive first: history must survive source deletion
    // (lock scoped to the archive branch only — the SQL fallback below
    //  must not contend with the GUI thread's archiveEntryCount)
    {
        QMutexLocker lock(&m_archiveMutex);
        if (!m_archive.isEmpty()) {
            QHash<qint64, qint64> byDay;
            for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it) {
                const ArchiveEntry& e = it.value();
                if (hasAgent && e.agent != agent)
                    continue;
                if (hasFamily && modelFamily(e.model) != family)
                    continue;
                byDay.insert(e.dayMs, byDay.value(e.dayMs) + e.inTok + e.outTok
                                          + e.reasonTok);
            }
            QVector<DailyTotal> out;
            for (auto it = byDay.constBegin(); it != byDay.constEnd(); ++it)
                out.push_back({ it.key(), it.value() });
            std::sort(out.begin(), out.end(),
                      [](const DailyTotal& a, const DailyTotal& b) {
                          return a.dayMs < b.dayMs;
                      });
            return out;
        }
    }
    // 归档为空（首扫之前 / 归档刚被清空）：退回缓存库

    QVector<DailyTotal> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    // bucket by LOCAL midnight: shift by the current UTC offset, floor to
    // UTC days, shift back (tz comes from the environment — safe to inline)
    const qint64 tz = qint64(QDateTime::currentDateTime().offsetFromUtc()) * 1000;
    const QString tzs = QString::number(tz);
    QString where;
    if (hasAgent)
        where += QStringLiteral("agent = :agent");
    if (hasFamily) {
        if (!where.isEmpty())
            where += QStringLiteral(" AND ");
        where += QStringLiteral("(lower(model) LIKE :famPat OR lower(model) = :fam)");
    }
    QString sql = QStringLiteral(
        "SELECT (time_ms + %1) / 86400000 * 86400000 - %1 AS d,"
        " SUM(in_tok + out_tok + reason_tok) FROM responses").arg(tzs);
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where;
    sql += QStringLiteral(" GROUP BY d ORDER BY d");
    q.prepare(sql);
    if (hasAgent)
        q.bindValue(QStringLiteral(":agent"), agent);
    if (hasFamily) {
        q.bindValue(QStringLiteral(":famPat"), family + QStringLiteral("-%"));
        q.bindValue(QStringLiteral(":fam"), family);
    }
    q.exec();
    while (q.next()) {
        DailyTotal t;
        t.dayMs = q.value(0).toLongLong();
        t.tokens = q.value(1).toLongLong();
        out.push_back(t);
    }
    return out;
}

QVector<Store::AgentTotal> Store::agentTotals(const QString& agent,
                                              const QString& family)
{
    const bool hasAgent = !agent.isEmpty();
    const bool hasFamily = !family.isEmpty();

    // archive first（与 dailyTotals 同一套口径）
    {
        QMutexLocker lock(&m_archiveMutex);
        if (!m_archive.isEmpty()) {
            QHash<QString, AgentTotal> byAgent;
            for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it) {
                const ArchiveEntry& e = it.value();
                if (hasAgent && e.agent != agent)
                    continue;
                if (hasFamily && modelFamily(e.model) != family)
                    continue;
                AgentTotal& t = byAgent[e.agent];
                t.agent = e.agent;
                t.requests += e.requests;
                t.inTok += e.inTok;
                t.outTok += e.outTok;
                t.reasonTok += e.reasonTok;
            }
            QVector<AgentTotal> out;
            for (auto it = byAgent.constBegin(); it != byAgent.constEnd(); ++it)
                out.push_back(it.value());
            std::sort(out.begin(), out.end(),
                      [](const AgentTotal& a, const AgentTotal& b) {
                          return (a.inTok + a.outTok + a.reasonTok)
                               > (b.inTok + b.outTok + b.reasonTok);
                      });
            return out;
        }
    }
    // 归档为空：退回缓存库
    QVector<AgentTotal> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    QString where;
    if (hasAgent)
        where += QStringLiteral("agent = :agent");
    if (hasFamily) {
        if (!where.isEmpty())
            where += QStringLiteral(" AND ");
        where += QStringLiteral("(lower(model) LIKE :famPat OR lower(model) = :fam)");
    }
    QString sql = QStringLiteral(
        "SELECT agent, COUNT(*), SUM(in_tok), SUM(out_tok), SUM(reason_tok)"
        " FROM responses");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where;
    sql += QStringLiteral(
        " GROUP BY agent ORDER BY SUM(in_tok + out_tok + reason_tok) DESC");
    q.prepare(sql);
    if (hasAgent)
        q.bindValue(QStringLiteral(":agent"), agent);
    if (hasFamily) {
        q.bindValue(QStringLiteral(":famPat"), family + QStringLiteral("-%"));
        q.bindValue(QStringLiteral(":fam"), family);
    }
    q.exec();
    while (q.next()) {
        AgentTotal t;
        t.agent = q.value(0).toString();
        t.requests = q.value(1).toLongLong();
        t.inTok = q.value(2).toLongLong();
        t.outTok = q.value(3).toLongLong();
        t.reasonTok = q.value(4).toLongLong();
        out.push_back(t);
    }
    return out;
}

qint64 Store::longestSessionActiveMs(const QString& agent, const QString& family)
{
    QMutexLocker lock(&m_archiveMutex); // both paths touch m_archive    // ① agents with an official chat-duration metric (zcode: Σ completed
    //    turn durations) report it live;
    // ② file-based agents fall back to the archive's per-session stream sum
    //    (their own agent rows only — zcode's live number is authoritative)
    QSet<QString> liveAgents;
    qint64 best = -1;
    for (AgentAdapter* a : m_adapters) {
        // zcode's live query only counts its own rows → skip when another
        // agent filter is active
        const bool applies = agent.isEmpty() || agent == a->id();
        if (!applies)
            continue;
        const qint64 live = a->longestChatMs(family);
        if (live >= 0) {
            best = qMax(best, live);
            liveAgents.insert(a->id());
        }
    }
    if (!m_archive.isEmpty()) {
        QHash<QPair<QString, QString>, qint64> perSession;
        for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it) {
            const ArchiveEntry& e = it.value();
            if (liveAgents.contains(e.agent))
                continue; // this agent's live metric is authoritative
            if (!agent.isEmpty() && e.agent != agent)
                continue;
            if (!family.isEmpty() && modelFamily(e.model) != family)
                continue;
            perSession[qMakePair(e.agent, e.sessionId)] += e.durSumMs;
        }
        for (auto it = perSession.constBegin(); it != perSession.constEnd(); ++it)
            best = qMax(best, it.value());
    }
    return qMax<qint64>(best, 0);
}

QString Store::modelFamily(const QString& model)
{
    // "GLM-5.3-Flash" / "glm-5.3" → "glm"; "deepseek-v4-1-flash" → "deepseek"
    const QString lower = model.toLower().trimmed();
    const int dash = lower.indexOf(QLatin1Char('-'));
    return dash > 0 ? lower.left(dash) : lower;
}

QString Store::familyDisplay(const QString& family)
{
    static const QHash<QString, QString> pretty = {
        { "glm", "GLM" }, { "gpt", "GPT" }, { "grok", "Grok" },
        { "deepseek", "DeepSeek" }, { "claude", "Claude" },
        { "gemini", "Gemini" }, { "qwen", "Qwen" }, { "qwen3", "Qwen" },
        { "o3", "O3" }, { "o4", "O4" },
    };
    const QString lower = family.toLower();
    return pretty.value(lower, family);
}

QStringList Store::archiveModelFamilies()
{
    QMutexLocker lock(&m_archiveMutex);
    if (!m_archive.isEmpty()) {
        QSet<QString> fams;
        for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it) {
            const QString f = modelFamily(it.value().model);
            if (!f.isEmpty())
                fams.insert(f);
        }
        QStringList out;
        for (const QString& f : fams)
            out << f; // family KEY — the caller maps it to display text
        std::sort(out.begin(), out.end());
        return out;
    }
    // cache fallback
    QStringList out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    if (q.exec(QStringLiteral(
            "SELECT DISTINCT lower(substr(model, 1, instr(model || '-', '-') - 1))"
            " FROM responses WHERE model != ''"))) {
        while (q.next()) {
            const QString f = q.value(0).toString();
            if (!f.isEmpty())
                out << f; // KEY, not display
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

QVector<Store::ModelDayPoint> Store::dailyTokensByModel(qint64 sinceMs, int topModels,
                                                        const QString& agent,
                                                        const QString& family)
{
    const bool hasAgent = !agent.isEmpty();
    const bool hasFamily = !family.isEmpty();

    // archive first: per (model, local day) TOTAL tokens
    // (lock scoped to the archive branch — SQL fallback runs free)
    {
        QMutexLocker lock(&m_archiveMutex);
        if (!m_archive.isEmpty()) {
            QHash<QString, qint64> totals;
            QHash<QString, QHash<qint64, qint64>> perModelDay;
            for (auto it = m_archive.constBegin(); it != m_archive.constEnd(); ++it) {
                const ArchiveEntry& e = it.value();
                if (hasAgent && e.agent != agent)
                    continue;
                if (hasFamily && modelFamily(e.model) != family)
                    continue;
                const qint64 tok = e.inTok + e.outTok + e.reasonTok;
                totals.insert(e.model, totals.value(e.model) + tok);
                perModelDay[e.model].insert(e.dayMs,
                                            perModelDay[e.model].value(e.dayMs) + tok);
            }
            QVector<QPair<QString, qint64>> ranked;
            for (auto it = totals.constBegin(); it != totals.constEnd(); ++it)
                ranked.push_back({ it.key(), it.value() });
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            QSet<QString> keep;
            for (int i = 0; i < ranked.size() && i < topModels; ++i)
                keep.insert(ranked[i].first);

            QVector<ModelDayPoint> out;
            for (auto it = perModelDay.constBegin(); it != perModelDay.constEnd(); ++it) {
                if (!keep.contains(it.key()))
                    continue;
                for (auto d = it.value().constBegin(); d != it.value().constEnd(); ++d) {
                    if (sinceMs > 0 && d.key() < sinceMs)
                        continue;
                    out.push_back({ d.key(), it.key(), d.value() });
                }
            }
            std::sort(out.begin(), out.end(),
                      [](const ModelDayPoint& a, const ModelDayPoint& b) {
                          return a.dayMs < b.dayMs;
                      });
            return out;
        }
    }
    // 归档为空：退回缓存库（按窗口总量取 Top N）

    // rank models by TOTAL tokens, keep the top N
    QVector<QPair<QString, qint64>> ranked;
    {
        QSqlDatabase db = connection();
        QSqlQuery q(db);
        QString where = QStringLiteral("model != ''");
        if (hasAgent)
            where += QStringLiteral(" AND agent = :agent");
        if (hasFamily)
            where += QStringLiteral(" AND (lower(model) LIKE :famPat OR lower(model) = :fam)");
        q.prepare(QStringLiteral(
            "SELECT model, SUM(in_tok + out_tok + reason_tok) AS t FROM responses"
            " WHERE %1 GROUP BY model ORDER BY t DESC").arg(where));
        if (hasAgent)
            q.bindValue(QStringLiteral(":agent"), agent);
        if (hasFamily) {
            q.bindValue(QStringLiteral(":famPat"), family + QStringLiteral("-%"));
            q.bindValue(QStringLiteral(":fam"), family);
        }
        q.exec();
        while (q.next() && ranked.size() < topModels)
            ranked.push_back({ q.value(0).toString(), q.value(1).toLongLong() });
    }
    if (ranked.isEmpty())
        return {};
    QSet<QString> keep;
    for (const auto& r : ranked)
        keep.insert(r.first);

    QVector<ModelDayPoint> out;
    QSqlDatabase db = connection();
    QSqlQuery q(db);
    const qint64 tz = qint64(QDateTime::currentDateTime().offsetFromUtc()) * 1000;
    const QString tzs = QString::number(tz);
    QString where = QStringLiteral("time_ms >= :since");
    if (hasAgent)
        where += QStringLiteral(" AND agent = :agent");
    if (hasFamily)
        where += QStringLiteral(" AND (lower(model) LIKE :famPat OR lower(model) = :fam)");
    q.prepare(QStringLiteral(
        "SELECT (time_ms + %1) / 86400000 * 86400000 - %1 AS d, model,"
        " SUM(in_tok + out_tok + reason_tok)"
        " FROM responses WHERE %2 GROUP BY d, model ORDER BY d").arg(tzs, where));
    q.bindValue(QStringLiteral(":since"), sinceMs > 0 ? sinceMs : 0);
    if (hasAgent)
        q.bindValue(QStringLiteral(":agent"), agent);
    if (hasFamily) {
        q.bindValue(QStringLiteral(":famPat"), family + QStringLiteral("-%"));
        q.bindValue(QStringLiteral(":fam"), family);
    }
    q.exec();
    while (q.next()) {
        ModelDayPoint p;
        p.dayMs = q.value(0).toLongLong();
        p.model = q.value(1).toString();
        p.genTok = q.value(2).toLongLong();
        if (keep.contains(p.model))
            out.push_back(p);
    }
    return out;
}

QVector<types::UsageRecord> Store::rawRows(const QString& agent, const QString& model,
                                           int limit)
{
    QVector<types::UsageRecord> out;
    QSqlDatabase db = connection();
    QString where;
    if (!agent.isEmpty())
        where += QStringLiteral("agent = :agent");
    if (!model.isEmpty()) {
        if (!where.isEmpty())
            where += QStringLiteral(" AND ");
        where += QStringLiteral("model LIKE :model");
    }
    QSqlQuery q(db);
    QString sql = QStringLiteral(
        "SELECT agent, session_id, turn_id, response_id, time_ms, dur_ok,"
        " dur_approx, dur_ms, model, effort, source, in_tok, cached_tok,"
        " cache_write_tok, out_tok, reason_tok, tps_ok, tps, error FROM responses");
    if (!where.isEmpty())
        sql += QStringLiteral(" WHERE ") + where;
    sql += QStringLiteral(" ORDER BY time_ms DESC LIMIT :lim");
    q.prepare(sql);
    if (!agent.isEmpty())
        q.bindValue(QStringLiteral(":agent"), agent);
    if (!model.isEmpty())
        q.bindValue(QStringLiteral(":model"), QStringLiteral("%") + model + QStringLiteral("%"));
    q.bindValue(QStringLiteral(":lim"), limit);
    q.exec();
    while (q.next()) {
        types::UsageRecord r;
        r.agentId = q.value(0).toString();
        r.sessionId = q.value(1).toString();
        r.turnId = q.value(2).toString();
        r.responseId = q.value(3).toString();
        r.timeMs = q.value(4).toLongLong();
        r.durOk = q.value(5).toInt() != 0;
        r.durApprox = q.value(6).toInt() != 0;
        r.durationMs = q.value(7).toDouble();
        r.model = q.value(8).toString();
        r.effort = q.value(9).toString();
        r.source = q.value(10).toString();
        r.inputTokens = q.value(11).toLongLong();
        r.cachedTokens = q.value(12).toLongLong();
        r.cacheWriteTokens = q.value(13).toLongLong();
        r.outputTokens = q.value(14).toLongLong();
        r.reasoningTokens = q.value(15).toLongLong();
        r.tpsOk = q.value(16).toInt() != 0;
        r.tps = q.value(17).toDouble();
        r.error = q.value(18).toString();
        out.push_back(r);
    }
    return out;
}
