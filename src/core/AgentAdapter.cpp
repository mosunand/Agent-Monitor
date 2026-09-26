// AgentAdapter.cpp — shared status() + the built-in adapter registry.

#include "AgentAdapter.h"

#include <QDateTime>

#include "ClaudeAdapter.h"
#include "CodexAdapter.h"
#include "GeminiLikeAdapter.h"
#include "OpenCodeAdapter.h"
#include "PiAdapter.h"
#include "ZCodeAdapter.h"

types::AgentStatus AgentAdapter::status() const
{
    types::AgentStatus st;
    st.id = id();
    st.name = displayName();
    st.color = color();
    QString dir;
    st.found = dataPresent(&dir);
    st.dataDir = dir;
    if (st.found) {
        QString note;
        const QVector<SessionFile> files = listSessionFiles(&note);
        st.sessions = files.size();
        st.note = note.isEmpty() ? QStringLiteral("数据正常（只读观测）") : note;
        for (const SessionFile& f : files)
            st.lastActivityMs = qMax(st.lastActivityMs, f.mtimeMs);
    } else {
        st.note = QStringLiteral("未检测到本地数据目录");
    }
    return st;
}

namespace AgentRegistry {

QVector<AgentAdapter*> createAdapters()
{
    // display order; extend here when adding a new agent
    return {
        new ZCodeAdapter(),
        new CodexAdapter(),
        new ClaudeAdapter(),
        new GeminiLikeAdapter(QStringLiteral("gemini"),
                              QStringLiteral("Gemini CLI"),
                              QColor(0x42, 0x85, 0xf4), QStringLiteral(".gemini"),
                              nullptr),
        new GeminiLikeAdapter(QStringLiteral("qwen"),
                              QStringLiteral("Qwen Code (千问)"),
                              QColor(0x7c, 0x3a, 0xed), QStringLiteral(".qwen"),
                              "QWEN_HOME"),
        new OpenCodeAdapter(),
        new PiAdapter(),
    };
}

} // namespace AgentRegistry
