#pragma once
// CrashHandler.h — unhandled-exception capture on Windows: writes a minidump
// + a textual crash line next to the exe, so "it just crashes" comes with a
// stack. (Ported from zcode-monitor.)

#include <QString>

namespace CrashHandler {
// installs the filter; returns false when unsupported (non-Windows)
bool install();
// where the last crash artifacts were written (empty when none)
QString lastCrashFile();
} // namespace CrashHandler
