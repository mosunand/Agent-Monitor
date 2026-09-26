// Paths.cpp — see Paths.h.

#include "Paths.h"

#include <QDir>
#include <QStandardPaths>
#include <QtGlobal>

namespace {

QString underHome(const QString& tail)
{
    return QDir::homePath() + QLatin1Char('/') + tail;
}

QString envOr(const char* name, const QString& fallback)
{
    const QByteArray v = qgetenv(name);
    if (!v.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(v));
    return fallback;
}

} // namespace

namespace Paths {

QString codexHome()
{
    return envOr("CODEX_HOME", underHome(QStringLiteral(".codex")));
}

QString codexSessionsDir()
{
    return codexHome() + QStringLiteral("/sessions");
}

QString codexArchiveDir()
{
    return codexHome() + QStringLiteral("/archived_sessions");
}

QString claudeHome()
{
    return envOr("CLAUDE_CONFIG_DIR", underHome(QStringLiteral(".claude")));
}

QString claudeProjectsDir()
{
    return claudeHome() + QStringLiteral("/projects");
}

QString zcodeDbPath()
{
    return envOr("ZCODE_DB", underHome(QStringLiteral(".zcode/cli/db/db.sqlite")));
}

QString cacheDbPath()
{
    // AppLocalDataLocation already ends with <org>/<app>
    // (…/AppData/Local/zhipucode/agent-monitor) — just append the file name
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/cache.sqlite");
}

bool ensureCacheDir()
{
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    return QDir().mkpath(base);
}

} // namespace Paths
