#pragma once
// Paths.h — resolution of every agent's local data directory + our cache DB.
// Environment overrides (same variable names the tools themselves honor):
//   CODEX_HOME          → Codex data root          (default ~/.codex)
//   CLAUDE_CONFIG_DIR   → Claude Code config root  (default ~/.claude)

#include <QString>

namespace Paths {

// Codex (OpenAI)
QString codexHome();          // CODEX_HOME or ~/.codex
QString codexSessionsDir();   // ~/.codex/sessions
QString codexArchiveDir();    // ~/.codex/archived_sessions

// Claude Code (Anthropic)
QString claudeHome();         // CLAUDE_CONFIG_DIR or ~/.claude
QString claudeProjectsDir();  // ~/.claude/projects

// ZCode
QString zcodeDbPath();        // ZCODE_DB or ~/.zcode/cli/db/db.sqlite

// our own SQLite cache (parsed rollouts; the app is writable here only)
QString cacheDbPath();        // <AppLocalData>/agent-monitor/cache.sqlite
bool ensureCacheDir();

} // namespace Paths
