// Frost.cpp — see Frost.h. Windows-only DWM implementation.

#include "Frost.h"

#include <QtGlobal>

#ifdef Q_OS_WIN

#include <QWidget>
#include <QWindow>

#include <dwmapi.h>

namespace {

constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmSystemBackdropType = 38;
constexpr DWORD kDwmsbtNone = 0;
constexpr DWORD kDwmsbtTransientWindow = 2;

constexpr DWORD kWcaAccentPolicy = 19;
constexpr DWORD kAccentEnableAcrylicBlurBehind = 4;

#pragma pack(push, 1)
struct AccentPolicy {
    DWORD state;
    DWORD flags;
    DWORD gradientColor; // AABBGGRR
    DWORD animationId;
};
struct WindowCompositionAttribData {
    PVOID attribute;
    PVOID data;
    SIZE_T sizeOfData;
};
#pragma pack(pop)

} // namespace

namespace Frost {

bool apply(QWidget* window, bool on, bool darkTheme)
{
    if (!window)
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return false;

    // 深色主题的标题栏
    BOOL dark = darkTheme ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));

    bool anyOk = false;

    // ① SWCA 亚克力：色调可控的强毛玻璃（深色主题用深色 tint）
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SwcaFn = BOOL (WINAPI*)(HWND, DWORD, void*);
        auto swca = reinterpret_cast<SwcaFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute")));
        if (swca) {
            AccentPolicy ap;
            ZeroMemory(&ap, sizeof(ap));
            if (on) {
                ap.state = kAccentEnableAcrylicBlurBehind;
                // AABBGGRR — 更透的玻璃底（深色 55% tint，浅色 85%）
                ap.gradientColor = darkTheme ? 0x8C120D0Bu : 0xD8FAF8F6u;
            }
            WindowCompositionAttribData data;
            data.attribute = reinterpret_cast<PVOID>(kWcaAccentPolicy);
            data.data = &ap;
            data.sizeOfData = sizeof(ap);
            if (swca(hwnd, kWcaAccentPolicy, &data))
                anyOk = true;
        }
    }

    // ② 回退：Win11 系统亚克力背景
    DWORD backdrop = on ? kDwmsbtTransientWindow : kDwmsbtNone;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType,
                                             &backdrop, sizeof(backdrop));
    const bool backdropOk = SUCCEEDED(hr);
    if (backdropOk && on)
        anyOk = true;

    if (on) {
        MARGINS margins{-1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
    qWarning("[frost] on=%d swca=%d backdrop=%d hr=0x%08lX", on ? 1 : 0,
             anyOk ? 1 : 0, backdropOk ? 1 : 0, (unsigned long)hr);
    return anyOk;
}

} // namespace Frost

#else // !Q_OS_WIN

namespace Frost {

bool apply(QWidget* window, bool on, bool darkTheme)
{
    Q_UNUSED(window);
    Q_UNUSED(on);
    Q_UNUSED(darkTheme);
    return false;
}

} // namespace Frost

#endif
