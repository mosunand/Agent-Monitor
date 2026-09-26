#pragma once
// Types.h — unified data model across all monitored agents.
// Every agent adapter normalizes its own session files into these structs:
//   one UsageRecord  = one completed model response (the unit of TPS/tokens)
//   one SessionInfo  = one local session file (codex rollout / claude jsonl)
// Time values are epoch milliseconds throughout; ISO-8601 "Z" timestamps in
// the source files are converted on parse.

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QVariant>
#include <QVector>

namespace types {

// ───────────────────────── usage (per model response) ─────────────────────────

struct UsageRecord {
    QString agentId;        // "codex" | "claude" | …
    QString sessionId;
    QString turnId;         // best-effort turn grouping ("" when unknown)
    QString responseId;     // provider response/message id ("" when unknown)
    qint64 timeMs = 0;      // completion timestamp (UTC ms)
    bool durOk = false;     // streaming duration derivable
    bool durApprox = false; // duration includes tool gaps (codex approx)
    double durationMs = 0;
    QString model;
    QString effort;         // reasoning effort/variant when known
    QString source;         // "main" | "sidechain" | …
    qint64 inputTokens = 0;
    qint64 cachedTokens = 0;      // cache read
    qint64 cacheWriteTokens = 0;
    qint64 outputTokens = 0;
    qint64 reasoningTokens = 0;
    QString error;          // non-empty → failed/aborted call (zero tokens)
    bool tpsOk = false;
    double tps = 0;         // (output+reasoning)/durationSeconds
    qint64 totalTokens() const { return inputTokens + outputTokens + reasoningTokens; }
};

// one chat message for the conversation replay (parsed on demand — never
// stored in the cache db; the source file is re-read when the tab opens)
struct ChatMessage {
    QString role;      // "user" | "assistant" | "reasoning" | "tool" | "tool_output"
    QString model;     // assistant messages
    QString toolName;  // tool calls
    QString text;      // content / tool arguments / tool output (truncated)
    qint64 timeMs = 0;
    bool isError = false;
};

// ───────────────────────── sessions ─────────────────────────

struct SessionInfo {
    QString agentId, sessionId, filePath;
    QString title, cwd, model, originator;
    qint64 startedMs = 0, updatedMs = 0;
    qint64 responses = 0, toolCalls = 0, errors = 0;
    qint64 inputTokens = 0, cachedTokens = 0, cacheWriteTokens = 0;
    qint64 outputTokens = 0, reasoningTokens = 0;
    qint64 contextWindow = 0;
    qint64 totalTokens() const { return inputTokens + outputTokens + reasoningTokens; }
};

// ───────────────────────── agents (adapters) ─────────────────────────

struct AgentStatus {
    QString id, name;
    QColor color;
    bool found = false;       // data directory present
    QString dataDir, note;
    int sessions = 0;
    qint64 lastActivityMs = 0;
};

// ───────────────────────── overview aggregates ─────────────────────────

struct Kpis {
    qint64 requests = 0, sessions = 0, errors = 0;
    qint64 inTok = 0, outTok = 0, reasonTok = 0, cacheRead = 0, cacheWrite = 0;
    bool avgDurOk = false;
    double avgDurMs = 0;
    bool tpsOk = false;
    double weightedTps = 0;   // Σ(out+reason where tps_ok) / Σ(dur where tps_ok)
    qint64 tpsSamples = 0;
};

struct SeriesPoint {
    qint64 bucketMs = 0;
    qint64 requests = 0, input = 0, output = 0, reasoning = 0;
};

struct ModelBreakdown {
    QString agentId, model, effort;
    qint64 requests = 0, inTok = 0, cachedTok = 0, outTok = 0, reasonTok = 0;
    bool avgDurOk = false;
    double avgDurMs = 0;
    bool tpsOk = false;
    double weightedTps = 0;
    qint64 lastMs = 0;
    qint64 totalTokens() const { return inTok + outTok + reasonTok; }
};

struct AgentBreakdown {
    QString agentId;
    qint64 requests = 0, inTok = 0, cachedTok = 0, outTok = 0, reasonTok = 0;
    qint64 totalTokens() const { return inTok + outTok + reasonTok; }
};

struct ToolBreakdown {
    QString agentId, toolName;
    qint64 calls = 0, sessions = 0;
};

struct SpeedRow {
    qint64 timeMs = 0;
    QString agentId, model, sessionId;
    qint64 output = 0, reasoning = 0;
    bool durOk = false;
    double durationMs = 0;
    bool tpsOk = false;
    double tps = 0;
};

struct OverviewData {
    QString window; // "today" | "24h" | "7d" | "30d" | "all"
    qint64 sinceMs = 0;
    Kpis kpis;
    Kpis todayKpis; // fixed today window — feeds the left big card
    QVector<ModelBreakdown> todayByModel; // per-model lines for the today card
    QVector<SeriesPoint> series;
    QVector<ModelBreakdown> byModel;
    QVector<AgentBreakdown> byAgent;
    QVector<ToolBreakdown> byTool;
    QVector<SpeedRow> recentSpeed;
};

} // namespace types
