#pragma once
// Store.h — the central data service.
//
//  · owns every AgentAdapter
//  · incremental scanner: cheap mtime/size probe of each adapter's session
//    files; only new/changed files are re-parsed into the SQLite cache
//    (responses + sessions + files tables, WAL, per-thread connections)
//  · all overview/session/model queries run as SQL over the cache
//  · a poll timer re-scans every 2s (off-thread) and pushes genuinely new
//    UsageRecords to the live feed
//
// Every method may be called from pool threads (pages use Async::run);
// queries use a per-thread connection, writes are serialized by a mutex.

#include <QAtomicInt>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSqlDatabase>
#include <QTimer>
#include <QVector>

#include "AgentAdapter.h"
#include "Types.h"

class Store : public QObject {
    Q_OBJECT
public:
    static Store& instance();

    bool openCache(QString* errOut = nullptr); // idempotent; false on db error

    void startPolling(int intervalMs = 2000);
    void stopPolling();

    // ── adapters ──
    QVector<AgentAdapter*> adapters() const { return m_adapters; }
    AgentAdapter* adapter(const QString& id) const;
    // directory walks are done during scan() only; the UI reads the cache
    QVector<types::AgentStatus> agentStatuses() const;

    // ── scan (off-thread) ──
    struct ScanResult {
        bool ok = false;
        QString err;
        int totalFiles = 0;
        int changedFiles = 0;
        QVector<types::UsageRecord> freshRecords; // after watermark, feed only
    };
    ScanResult scan(); // incremental; parse + cache update + fresh-record diff

    // ── queries (SQL over the cache, off-thread) ──
    static qint64 windowSince(const QString& window); // today|24h|7d|30d|all → epoch ms (0=all)
    types::OverviewData overview(const QString& window);
    QVector<types::SessionInfo> sessionList(const QString& agent, const QString& search,
                                            int limit = 500);
    types::SessionInfo sessionInfo(const QString& agent, const QString& sessionId);
    QVector<types::UsageRecord> sessionRecords(const QString& agent, const QString& sessionId);
    // conversation replay — parsed on demand from the source file (lazy,
    // off-thread; heavy content never lives in the cache db)
    bool sessionMessages(const QString& agent, const QString& sessionId,
                         QVector<types::ChatMessage>& outMessages, QString* err = nullptr);
    QVector<types::ModelBreakdown> modelBreakdown(qint64 sinceMs);
    QVector<types::AgentBreakdown> agentBreakdown(qint64 sinceMs);
    QVector<types::ToolBreakdown> toolBreakdown(qint64 sinceMs, int limit = 30);
    // per-model daily generated-token series (top N models by total)
    struct ModelDayPoint { qint64 dayMs; QString model; qint64 genTok; };
    QVector<ModelDayPoint> modelDailySeries(qint64 sinceMs, int topModels);
    // per-model daily TOTAL tokens (in+out+reason) — 使用统计 page
    QVector<ModelDayPoint> dailyTokensByModel(qint64 sinceMs, int topModels,
                                              const QString& agent = QString(),
                                              const QString& family = QString());
    // per-local-day total tokens (heatmap / streaks / peak); dayMs aligned
    // to LOCAL midnight. Served from the binary archive when present —
    // history survives even after the source agents' data is deleted.
    // agent/family filters (empty = all) apply to every query here.
    struct DailyTotal { qint64 dayMs; qint64 tokens; };
    QVector<DailyTotal> dailyTotals(const QString& agent = QString(),
                                    const QString& family = QString());
    // per-agent lifetime totals（使用统计页「按 agent 分布」，同样归档优先）
    struct AgentTotal {
        QString agent;
        qint64 requests = 0, inTok = 0, outTok = 0, reasonTok = 0;
    };
    QVector<AgentTotal> agentTotals(const QString& agent = QString(),
                                    const QString& family = QString());
    // Σ stream duration of the most active session (最长聊天时长)
    qint64 longestSessionActiveMs(const QString& agent = QString(),
                                  const QString& family = QString());

    // model family normalization: "GLM-5.3-Flash"/"glm-5.3" → "glm"
    static QString modelFamily(const QString& model);
    static QString familyDisplay(const QString& family); // "glm" → "GLM"
    QStringList archiveModelFamilies();                  // display names
    QStringList archiveAgentIds();                       // agents with recorded data

    // ── binary archive (self-owned history, <exe dir>/usage-archive.bin) ──
    // Aggregated per (agent, session, model, effort, local day); merged from
    // the cache after every scan, NEVER deleted — deleting an agent's source
    // data (or our cache) cannot erase recorded history.
    struct ArchiveEntry {
        QString agent, sessionId, model, effort;
        qint64 dayMs = 0;       // local midnight
        qint64 requests = 0, errors = 0;
        qint64 inTok = 0, outTok = 0, reasonTok = 0;
        qint64 cachedTok = 0, cacheWriteTok = 0;
        qint64 durSumMs = 0;    // Σ streaming duration (dur_ok rows)
        qint64 tpsGenTok = 0;   // Σ (out+reason) of tps_ok rows
    };
    static QString archivePath();
    int archiveEntryCount(); // locked size for UI threads
    // archive retention (days); entries older than this are pruned at merge
    qint64 retentionDays() const { return m_retentionDays; }
    void setRetentionDays(qint64 days); // persists + prunes immediately
    QVector<types::UsageRecord> rawRows(const QString& agent, const QString& model,
                                        int limit = 400);
    // 最近可测流式响应（速度表），可按模型大类过滤（空=全部）
    QVector<types::SpeedRow> recentSpeed(qint64 sinceMs, const QString& family,
                                         int limit = 60);
    qint64 lastScanMs() const;   // duration of last scan (locked read)
    // 最小化时暂停 2s 轮询（恢复后下一拍自动续上）
    void setPollPaused(bool paused) { m_pollPaused.storeRelaxed(paused ? 1 : 0); }

signals:
    void scanFinished(bool ok, const QString& err, int changedFiles, int totalFiles);
    void newRecords(const QVector<types::UsageRecord>& records);
    void dataChanged(); // pages reload (new/changed files parsed)

private slots:
    void pollTick();

private:
    explicit Store();
    QSqlDatabase connection(); // per-thread, opened lazily
    void createSchema();
    void loadArchive();        // takes the lock (idempotent)
    void loadArchiveLocked();  // caller holds m_archiveMutex
    void mergeArchiveFromCache(); // upsert (agent,session,model,day) aggregates
                                  // + atomic rewrite of the bin file

    QTimer m_timer;
    QVector<AgentAdapter*> m_adapters;
    // SQL 写只发生在 scan() 内，由 m_scanBusy 串行（一次只有一趟扫描）
    QAtomicInt m_scanBusy;
    QAtomicInt m_pollPaused;
    bool m_watermarkInit = false;
    qint64 m_watermarkMs = 0;
    // refreshed by scan() (pool thread) — agentStatuses() reads it from BOTH
    // the GUI thread (SourcesPage) and pool threads (health poll): must be
    // guarded, an unguarded QVector copy during a scan is a data race
    mutable QMutex m_statusMutex;
    QVector<types::AgentStatus> m_statusCache;
    qint64 m_lastScanMs = 0;
    // binary archive (self-owned history)
    QHash<QString, ArchiveEntry> m_archive;
    mutable QMutex m_archiveMutex; // GUI readers vs pool-thread merge
    bool m_archiveLoaded = false;
    QByteArray m_archiveHash; // skip disk write when unchanged
    qint64 m_retentionDays = 730; // keep 2 years by default
};
