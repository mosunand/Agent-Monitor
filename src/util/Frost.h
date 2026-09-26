#pragma once
// Frost.h — Windows 11 acrylic/frosted-glass backdrop for the main window.
// (Ported from zcode-monitor.)

class QWidget;

namespace Frost {

// apply (on=true) or remove (on=false) the acrylic backdrop.
// darkTheme selects the glass tint (dark/light). Returns true when accepted.
bool apply(QWidget* window, bool on, bool darkTheme);

} // namespace Frost
