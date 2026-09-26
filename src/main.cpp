// main.cpp — entry point.
//   --selftest           headless data-layer verification (exit code 0/1)
//   --screenshot [dir]   walk every page/tab, save PNGs, quit (default dir: .)

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>

#include "selftest/SelfTest.h"
#include "util/CrashHandler.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char* argv[])
{
    const QStringList args = [argv, argc]() {
        QStringList l;
        for (int i = 1; i < argc; ++i)
            l << QString::fromLocal8Bit(argv[i]);
        return l;
    }();

    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--selftest")) {
#ifdef Q_OS_WIN
            // WIN32 subsystem has no console of its own; attach when launched
            // from a shell so the report is visible.
            const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
            if (!h || GetFileType(h) == FILE_TYPE_UNKNOWN) {
                if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                    FILE* unused = nullptr;
                    freopen_s(&unused, "CONOUT$", "w", stdout);
                    freopen_s(&unused, "CONOUT$", "w", stderr);
                }
            }
#endif
            QCoreApplication app(argc, argv);
            // keep AppLocalDataLocation (the cache db path) identical to GUI mode
            QCoreApplication::setOrganizationName(QStringLiteral("zhipucode"));
            QCoreApplication::setApplicationName(QStringLiteral("agent-monitor"));
            const int code = SelfTest::run();
            fflush(stdout);
            fflush(stderr);
            return code;
        }
    }

    // --screenshot [dir]
    QString screenshotDir;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--screenshot")) {
            screenshotDir = QStringLiteral(".");
            if (i + 1 < args.size() && !args.at(i + 1).startsWith(QLatin1Char('-')))
                screenshotDir = args.at(i + 1);
            break;
        }
    }

    QApplication app(argc, argv);
    // Qt routes qInfo/qWarning to OutputDebugString on Windows GUI apps —
    // install a stderr handler so diagnostics are visible when redirected.
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx,
                              const QString& msg) {
        const char* level = "info";
        switch (type) {
        case QtDebugMsg: level = "debug"; break;
        case QtInfoMsg: level = "info"; break;
        case QtWarningMsg: level = "warning"; break;
        case QtCriticalMsg: level = "critical"; break;
        case QtFatalMsg: level = "fatal"; break;
        }
        QByteArray line = QByteArray("[qt:") + level + "] " + msg.toUtf8() + '\n';
        if (ctx.file)
            line += QByteArray("  (") + ctx.file + ':' + QByteArray::number(ctx.line) + ")\n";
        fwrite(line.constData(), 1, line.size(), stderr);
        fflush(stderr);
    });
    CrashHandler::install();
    extern int runGui(QApplication& app, const QString& screenshotDir);
    return runGui(app, screenshotDir);
}
