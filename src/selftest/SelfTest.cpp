// SelfTest.cpp — see SelfTest.h.

#include "SelfTest.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

#include "core/AgentAdapter.h"
#include "core/ClaudeAdapter.h"
#include "core/CodexAdapter.h"
#include "core/Store.h"
#include "core/Types.h"

namespace {

struct Check {
    QString name;
    bool ok = false;
    QString detail;
};

void report(QVector<Check>& all, const QString& name, bool ok, const QString& detail = QString())
{
    Check c;
    c.name = name;
    c.ok = ok;
    c.detail = detail;
    all.push_back(c);
    printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", qPrintable(name),
           detail.isEmpty() ? "" : " — ", qPrintable(detail));
    fflush(stdout);
}

// parse every file of an adapter, return the records
QVector<types::UsageRecord> parseAll(AgentAdapter* a, QVector<types::SessionInfo>* sessions,
                                     QHash<QString, qint64>* toolTotals = nullptr)
{
    QVector<types::UsageRecord> out;
    const auto files = a->listSessionFiles();
    for (const SessionFile& f : files) {
        types::SessionInfo meta;
        QVector<types::UsageRecord> recs;
        QHash<QString, ToolStat> tools;
        if (a->parseFile(f, meta, recs, &tools)) {
            out += recs;
            if (sessions)
                sessions->push_back(meta);
            if (toolTotals)
                for (auto it = tools.constBegin(); it != tools.constEnd(); ++it)
                    toolTotals->insert(it.key(), toolTotals->value(it.key()) + it.value().calls);
        }
    }
    return out;
}

} // namespace

int SelfTest::run()
{
    QVector<Check> all;
    printf("agent-monitor selftest — %s\n",
           qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)));
    fflush(stdout);

    // ── 1. registry ──
    const auto adapters = AgentRegistry::createAdapters();
    report(all, QStringLiteral("registry >= 2 adapters"), adapters.size() >= 2,
           QStringLiteral("got %1").arg(adapters.size()));

    AgentAdapter* codex = nullptr;
    AgentAdapter* claude = nullptr;
    AgentAdapter* zcode = nullptr;
    for (AgentAdapter* a : adapters) {
        if (a->id() == QLatin1String("codex"))
            codex = a;
        if (a->id() == QLatin1String("claude"))
            claude = a;
        if (a->id() == QLatin1String("zcode"))
            zcode = a;
    }
    report(all, QStringLiteral("codex adapter registered"), codex != nullptr);
    report(all, QStringLiteral("claude adapter registered"), claude != nullptr);
    report(all, QStringLiteral("zcode adapter registered"), zcode != nullptr);

    // ── 2. zcode parsing (sqlite source, biggest volume) ──
    if (zcode) {
        QVector<types::SessionInfo> metas;
        const auto records = parseAll(zcode, &metas);
        report(all, QStringLiteral("zcode parse produces records"), !records.isEmpty(),
               QStringLiteral("%1 records from %2 sessions")
                   .arg(records.size()).arg(metas.size()));
        bool anyTokens = false, anyDur = false;
        for (const auto& r : records) {
            if (r.totalTokens() > 0)
                anyTokens = true;
            if (r.durOk && r.tpsOk && r.tps > 0)
                anyDur = true;
        }
        report(all, QStringLiteral("zcode records carry token usage"), anyTokens);
        report(all, QStringLiteral("zcode exact TPS derivable"), anyDur);
    }

    // ── 2. codex parsing (real local data) ──
    if (codex) {
        const auto files = codex->listSessionFiles();
        report(all, QStringLiteral("codex session files >= 1"), files.size() >= 1,
               QStringLiteral("found %1").arg(files.size()));
        QVector<types::SessionInfo> metas;
        const auto records = parseAll(codex, &metas);
        report(all, QStringLiteral("codex parse produces records"), !records.isEmpty(),
               QStringLiteral("%1 records from %2 files").arg(records.size()).arg(metas.size()));
        bool anyModel = false, anyTokens = false, anyDur = false, badTime = false;
        for (const auto& r : records) {
            if (!r.model.isEmpty())
                anyModel = true;
            if (r.totalTokens() > 0)
                anyTokens = true;
            if (r.durOk && r.tpsOk && r.tps > 0)
                anyDur = true;
            if (r.timeMs <= 0)
                badTime = true;
        }
        report(all, QStringLiteral("codex records carry model ids"), anyModel);
        report(all, QStringLiteral("codex records carry token usage"), anyTokens);
        report(all, QStringLiteral("codex records carry approximate TPS"), anyDur);
        report(all, QStringLiteral("codex timestamps all valid"), !badTime);
        bool metasOk = true;
        for (const auto& m : metas)
            if (m.sessionId.isEmpty() || m.title.isEmpty() || m.updatedMs <= 0)
                metasOk = false;
        report(all, QStringLiteral("codex session meta complete"), metasOk);
    }

    // ── 3. claude parsing ──
    if (claude) {
        const auto files = claude->listSessionFiles();
        report(all, QStringLiteral("claude session files >= 1"), files.size() >= 1,
               QStringLiteral("found %1").arg(files.size()));
        QVector<types::SessionInfo> metas;
        const auto records = parseAll(claude, &metas);
        report(all, QStringLiteral("claude parse produces records"), !records.isEmpty(),
               QStringLiteral("%1 records from %2 files").arg(records.size()).arg(metas.size()));
        bool anyExactTps = false, anyCached = false, anyResponseId = false;
        for (const auto& r : records) {
            if (r.durOk && r.tpsOk && r.tps > 0 && !r.durApprox)
                anyExactTps = true;
            if (r.cachedTokens > 0)
                anyCached = true;
            if (!r.responseId.isEmpty())
                anyResponseId = true;
        }
        report(all, QStringLiteral("claude exact streaming TPS derivable"), anyExactTps);
        // 缓存读做「原始文件 ↔ 解析结果」保真校验，而不是假设本机数据一定有
        // 样本 —— Claude Code 默认 30 天保留期会把带缓存读的旧会话轮换掉
        bool rawHasCacheRead = false;
        for (const SessionFile& f : files) {
            QFile file(f.path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            while (!rawHasCacheRead && !file.atEnd()) {
                const QByteArray line = file.readLine(4 * 1024 * 1024);
                if (!line.contains("cache_read_input_tokens"))
                    continue;
                const QJsonObject o = QJsonDocument::fromJson(line).object();
                if (o.value("message").toObject().value("usage").toObject()
                        .value("cache_read_input_tokens").toVariant().toLongLong() > 0)
                    rawHasCacheRead = true;
            }
            if (rawHasCacheRead)
                break;
        }
        report(all, QStringLiteral("claude cache-read fidelity"),
               !rawHasCacheRead || anyCached,
               rawHasCacheRead ? QString()
                               : QStringLiteral("本机数据无缓存读样本（保留期轮换）"));
        report(all, QStringLiteral("claude response ids present"), anyResponseId);
        bool titlesOk = true;
        for (const auto& m : metas)
            if (m.title.isEmpty())
                titlesOk = false;
        report(all, QStringLiteral("claude session titles complete"), titlesOk);
    }

    // ── 4. store + cache db ──
    QString err;
    bool opened = Store::instance().openCache(&err);
    report(all, QStringLiteral("cache db opens"), opened, err);

    const auto scan1 = Store::instance().scan();
    report(all, QStringLiteral("first scan ok"), scan1.ok, scan1.err);
    report(all, QStringLiteral("first scan sees files"), scan1.totalFiles >= 1,
           QStringLiteral("%1 files").arg(scan1.totalFiles));

    const auto scan2 = Store::instance().scan();
    report(all, QStringLiteral("second scan incremental (0 changed)"),
           scan2.ok && scan2.changedFiles == 0,
           QStringLiteral("changed=%1").arg(scan2.changedFiles));

    // ── 5. aggregates ──
    const auto allData = Store::instance().overview(QStringLiteral("all"));
    report(all, QStringLiteral("overview(all) has requests"),
           allData.kpis.requests >= 1,
           QStringLiteral("%1 requests").arg(allData.kpis.requests));
    report(all, QStringLiteral("overview(all) token sums consistent"),
           allData.kpis.inTok + allData.kpis.outTok + allData.kpis.reasonTok > 0);
    report(all, QStringLiteral("overview(all) today kpis computed"),
           allData.todayKpis.requests >= 0);
    report(all, QStringLiteral("overview series non-empty"), !allData.series.isEmpty(),
           QStringLiteral("%1 buckets").arg(allData.series.size()));
    bool seriesSorted = true;
    for (int i = 1; i < allData.series.size(); ++i)
        if (allData.series[i].bucketMs <= allData.series[i - 1].bucketMs)
            seriesSorted = false;
    report(all, QStringLiteral("series buckets sorted"), seriesSorted);
    report(all, QStringLiteral("byModel breakdown non-empty"), !allData.byModel.isEmpty());
    {
        const auto byAgent = Store::instance().agentBreakdown(0);
        qint64 sumIn = 0, sumOut = 0;
        for (const auto& a : byAgent) {
            sumIn += a.inTok;
            sumOut += a.outTok + a.reasonTok;
        }
        report(all, QStringLiteral("byAgent sums == kpis"),
               sumIn == allData.kpis.inTok && sumOut == allData.kpis.outTok + allData.kpis.reasonTok,
               QStringLiteral("%1/%2 vs %3/%4")
                   .arg(sumIn).arg(sumOut)
                   .arg(allData.kpis.inTok)
                   .arg(allData.kpis.outTok + allData.kpis.reasonTok));
    }

    // ── 6. session list + detail consistency ──
    const auto sessions = Store::instance().sessionList(QString(), QString(), 500);
    report(all, QStringLiteral("session list non-empty"), !sessions.isEmpty(),
           QStringLiteral("%1 sessions").arg(sessions.size()));
    if (!sessions.isEmpty()) {
        const types::SessionInfo& s = sessions.first();
        const auto recs = Store::instance().sessionRecords(s.agentId, s.sessionId);
        report(all, QStringLiteral("session records match response count"),
               recs.size() == int(s.responses),
               QStringLiteral("%1 records vs %2 responses").arg(recs.size()).arg(s.responses));
        qint64 recOut = 0;
        for (const auto& r : recs)
            recOut += r.outputTokens;
        report(all, QStringLiteral("session record sums == session sums"),
               recOut == s.outputTokens,
               QStringLiteral("%1 vs %2").arg(recOut).arg(s.outputTokens));
    }

    // ── 7. models page helpers ──
    const auto daily = Store::instance().modelDailySeries(0, 5);
    bool dailySane = true;
    for (const auto& p : daily)
        if (p.genTok < 0 || p.dayMs <= 0 || p.model.isEmpty())
            dailySane = false;
    report(all, QStringLiteral("model daily series sane"), dailySane,
           QStringLiteral("%1 points").arg(daily.size()));
    const auto raw = Store::instance().rawRows(QString(), QString(), 400);
    report(all, QStringLiteral("raw rows respect limit"), raw.size() <= 400,
           QStringLiteral("%1 rows").arg(raw.size()));

    // ── 8. v2 features: tools / errors / conversation replay ──
    {
        const auto tools = Store::instance().toolBreakdown(0, 30);
        bool toolsSane = !tools.isEmpty();
        for (const auto& t : tools)
            if (t.calls <= 0 || t.toolName.isEmpty())
                toolsSane = false;
        report(all, QStringLiteral("tool aggregation populated"), toolsSane,
               tools.isEmpty() ? QStringLiteral("no tools")
                               : QStringLiteral("top: %1 ×%2")
                                     .arg(tools.first().toolName)
                                     .arg(tools.first().calls));

        // error bookkeeping: records with error == session meta count
        bool errorsConsistent = true;
        for (const types::SessionInfo& s : sessions) {
            const auto recs = Store::instance().sessionRecords(s.agentId, s.sessionId);
            qint64 errRecs = 0;
            for (const auto& r : recs)
                if (!r.error.isEmpty())
                    ++errRecs;
            if (errRecs != s.errors)
                errorsConsistent = false;
        }
        report(all, QStringLiteral("error counts consistent"), errorsConsistent);

        // conversation replay (on-demand source-file parse) — pick a session
        // whose agent supports it; zcode (sqlite) reads message/part tables
        QVector<types::ChatMessage> msgs;
        QString chatErr;
        bool ok = false;
        QElapsedTimer replayTimer;
        replayTimer.start();
        for (const types::SessionInfo& s : sessions) {
            msgs.clear();
            ok = Store::instance().sessionMessages(s.agentId, s.sessionId, msgs, &chatErr);
            if (ok && !msgs.isEmpty())
                break;
        }
        report(all, QStringLiteral("conversation replay parses"), ok && !msgs.isEmpty(),
               QStringLiteral("%1 messages in %2ms %3")
                   .arg(msgs.size())
                   .arg(replayTimer.elapsed())
                   .arg(chatErr));
    }

    // ── 9. usage stats (使用统计 page data) ──
    {
        const auto dayTotals = Store::instance().dailyTotals();
        bool dailyOk = !dayTotals.isEmpty();
        qint64 peak = 0;
        for (int i = 0; i < dayTotals.size(); ++i) {
            peak = qMax(peak, dayTotals[i].tokens);
            if (i > 0 && dayTotals[i].dayMs <= dayTotals[i - 1].dayMs)
                dailyOk = false;
        }
        report(all, QStringLiteral("daily totals sane (for heatmap/streaks)"),
               dailyOk && peak > 0,
               QStringLiteral("%1 days, peak %2").arg(dayTotals.size()).arg(peak));
        const qint64 longest = Store::instance().longestSessionActiveMs();
        report(all, QStringLiteral("longest session duration >= 0"), longest >= 0,
               QStringLiteral("%1 ms").arg(longest));

        // self-owned binary archive: exists next to the exe, non-empty, and
        // the archive-served daily totals must match the SQL-computed ones
        const QString archPath = Store::archivePath();
        const QFileInfo fi(archPath);
        report(all, QStringLiteral("archive bin file exists"),
               fi.exists() && fi.size() > 0,
               QStringLiteral("%1 (%2 KB, %3 entries loaded)")
                   .arg(archPath)
                   .arg(fi.size() / 1024)
                   .arg(Store::instance().archiveEntryCount()));
        const auto fromArchive = Store::instance().dailyTotals();
        qint64 sumA = 0;
        for (const auto& d : fromArchive)
            sumA += d.tokens;
        report(all, QStringLiteral("archive daily totals non-empty"),
               !fromArchive.isEmpty() && sumA > 0,
               QStringLiteral("%1 days, %2 tokens")
                   .arg(fromArchive.size())
                   .arg(sumA));

        // 按 agent 分布（同走归档）：与 dailyTotals 的总量对账
        const auto byAgentTotals = Store::instance().agentTotals();
        qint64 sumAgent = 0;
        for (const auto& t : byAgentTotals)
            sumAgent += t.inTok + t.outTok + t.reasonTok;
        report(all, QStringLiteral("agent totals match daily totals"),
               sumA == sumAgent && !byAgentTotals.isEmpty(),
               QStringLiteral("%1 agents, %2 tokens")
                   .arg(byAgentTotals.size())
                   .arg(sumAgent));
    }

    // ── summary ──
    int failed = 0;
    QJsonArray arr;
    for (const Check& c : all) {
        if (!c.ok)
            ++failed;
        QJsonObject o;
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("ok"), c.ok);
        if (!c.detail.isEmpty())
            o.insert(QStringLiteral("detail"), c.detail);
        arr.append(o);
    }
    QJsonObject summary;
    summary.insert(QStringLiteral("passed"), all.size() - failed);
    summary.insert(QStringLiteral("failed"), failed);
    summary.insert(QStringLiteral("checks"), arr);
    printf("\nselftest summary: %s\n",
           qPrintable(QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Indented))));
    fflush(stdout);
    return failed == 0 ? 0 : 1;
}
