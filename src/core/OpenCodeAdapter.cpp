// OpenCodeAdapter.cpp — see OpenCodeAdapter.h.

#include "OpenCodeAdapter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QThreadStorage>
#include <QVariant>

namespace {

QColor agentColor()
{
    return QColor(0x14, 0xb8, 0xa6); // opencode teal
}

constexpr int kBusyTimeoutMs = 5000;

// candidate db locations, first existing wins (env override honored)
QString dbPath()
{
    const QByteArray env = qgetenv("OPENCODE_DB");
    if (!env.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(env));
    const QString appdata = QString::fromLocal8Bit(qgetenv("APPDATA"));
    const QString home = QDir::homePath();
    const QStringList candidates = {
        appdata + QStringLiteral("/opencode/opencode.db"),
        home + QStringLiteral("/.local/share/opencode/opencode.db"),
        home + QStringLiteral("/.local/share/opencode/opencode.db3"),
    };
    for (const QString& c : candidates)
        if (QFile::exists(c))
            return c;
    return candidates.first();
}

QThreadStorage<QString> t_srcConn;

QSqlDatabase sourceConnection(QString* errOut)
{
    if (t_srcConn.hasLocalData()) {
        QSqlDatabase db = QSqlDatabase::database(t_srcConn.localData(), false);
        if (db.isOpen())
            return db;
    }
    const QString name = QStringLiteral("opencode-src-%1")
                             .arg(quintptr(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name))
        QSqlDatabase::removeDatabase(name);
    t_srcConn.setLocalData(name);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(dbPath());
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

QColor OpenCodeAdapter::color() const { return agentColor(); }

bool OpenCodeAdapter::dataPresent(QString* dir) const
{
    const QString d = dbPath();
    if (dir)
        *dir = d;
    return QFile::exists(d);
}

QVector<SessionFile> OpenCodeAdapter::listSessionFiles(QString* note) const
{
    QVector<SessionFile> out;
    QString err;
    QSqlDatabase db = sourceConnection(&err);
    if (!db.isOpen()) {
        if (note)
            *note = QStringLiteral("数据库打不开：") + err.left(80);
        return out;
    }
    // table name varies across schema generations
    QSqlQuery q(db);
    QString sessionTable;
    for (const char* t : { "session", "session_v2" }) {
        QSqlQuery m(db);
        m.prepare(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' AND name=?"));
        m.addBindValue(QString::fromLatin1(t));
        m.exec();
        if (m.next()) {
            sessionTable = QString::fromLatin1(t);
            break;
        }
    }
    if (sessionTable.isEmpty()) {
        if (note)
            *note = QStringLiteral("库中未发现 session 表");
        return out;
    }
    if (!q.exec(QStringLiteral("SELECT id, time_updated FROM %1")
                   .arg(sessionTable))) {
        if (note)
            *note = QStringLiteral("查询失败：") + q.lastError().text().left(80);
        return out;
    }
    const QString path = dbPath();
    while (q.next()) {
        SessionFile f;
        f.path = path + QLatin1Char('|') + q.value(0).toString();
        f.mtimeMs = q.value(1).toLongLong();
        f.size = 0;
        out.push_back(f);
    }
    if (note)
        *note = out.isEmpty() ? QStringLiteral("数据库中还没有会话") : QString();
    return out;
}

QString OpenCodeAdapter::sessionIdFromPseudoPath(const QString& path)
{
    const int bar = path.lastIndexOf(QLatin1Char('|'));
    return bar >= 0 ? path.mid(bar + 1) : path;
}

bool OpenCodeAdapter::parseFile(const SessionFile& f, types::SessionInfo& meta,
                                QVector<types::UsageRecord>& records,
                                QHash<QString, ToolStat>* outTools) const
{
    Q_UNUSED(outTools); // tool names live in part blobs — not aggregated yet
    meta = types::SessionInfo{};
    meta.agentId = id();
    meta.filePath = f.path;
    meta.sessionId = sessionIdFromPseudoPath(f.path);
    meta.originator = QStringLiteral("opencode");

    QString err;
    QSqlDatabase db = sourceConnection(&err);
    if (!db.isOpen()) {
        qWarning("opencode: source db unavailable: %s", qPrintable(err));
        return false;
    }

    // session meta (title/directory columns exist in most generations)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT * FROM session WHERE id = ?"));
        q.addBindValue(meta.sessionId);
        q.exec();
        if (q.next()) {
            const QSqlRecord rec = q.record();
            const int t = rec.indexOf(QStringLiteral("title"));
            const int d = rec.indexOf(QStringLiteral("directory"));
            const int u = rec.indexOf(QStringLiteral("time_updated"));
            const int c = rec.indexOf(QStringLiteral("time_created"));
            if (t >= 0)
                meta.title = q.value(t).toString();
            if (d >= 0)
                meta.cwd = q.value(d).toString();
            if (c >= 0)
                meta.startedMs = q.value(c).toLongLong();
            if (u >= 0)
                meta.updatedMs = q.value(u).toLongLong();
        } else {
            return false; // session vanished between list and parse
        }
    }

    // message rows: token data inside the `data` JSON blob
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT data FROM message WHERE session_id = ? ORDER BY time_created"));
    q.addBindValue(meta.sessionId);
    q.exec();
    while (q.next()) {
        const QJsonObject o = QJsonDocument::fromJson(
                                  q.value(0).toString().toUtf8()).object();
        if (o.value(QStringLiteral("role")).toString() != QLatin1String("assistant"))
            continue;
        const QJsonObject tokens = o.value(QStringLiteral("tokens")).toObject();
        if (tokens.isEmpty())
            continue;
        const QJsonObject time = o.value(QStringLiteral("time")).toObject();
        const QJsonObject model = o.value(QStringLiteral("model")).toObject();

        types::UsageRecord r;
        r.agentId = id();
        r.sessionId = meta.sessionId;
        r.responseId = o.value(QStringLiteral("id")).toString();
        r.model = model.value(QStringLiteral("id")).toString(
            o.value(QStringLiteral("modelID")).toString());
        r.effort = model.value(QStringLiteral("variant")).toString();
        r.source = QStringLiteral("main");
        r.inputTokens = tokens.value(QStringLiteral("input")).toVariant().toLongLong();
        r.outputTokens = tokens.value(QStringLiteral("output")).toVariant().toLongLong();
        r.reasoningTokens = tokens.value(QStringLiteral("reasoning")).toVariant().toLongLong();
        const QJsonObject cache = tokens.value(QStringLiteral("cache")).toObject();
        r.cachedTokens = cache.value(QStringLiteral("read")).toVariant().toLongLong();
        r.cacheWriteTokens = cache.value(QStringLiteral("write")).toVariant().toLongLong();
        const qint64 created = time.value(QStringLiteral("created")).toVariant().toLongLong();
        const qint64 completed = time.value(QStringLiteral("completed")).toVariant().toLongLong();
        r.timeMs = completed > 0 ? completed : created;
        if (completed > created && completed - created >= 100) {
            r.durOk = true;
            r.durationMs = double(completed - created);
            if (r.outputTokens + r.reasoningTokens > 0) {
                r.tpsOk = true;
                r.tps = double(r.outputTokens + r.reasoningTokens)
                        / (r.durationMs / 1000.0);
            }
        }
        if (r.totalTokens() <= 0 && r.error.isEmpty())
            continue;
        records.push_back(r);
    }

    meta.updatedMs = qMax(meta.startedMs, meta.updatedMs);
    if (!meta.cwd.isEmpty() && meta.title.isEmpty())
        meta.title = QFileInfo(meta.cwd).fileName();
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
    if (meta.title.isEmpty())
        meta.title = meta.sessionId.left(12);
    return true;
}
