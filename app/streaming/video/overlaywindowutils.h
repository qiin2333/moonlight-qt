#pragma once

#include <QWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace OverlayWindowUtils {

inline Qt::WindowFlags nonActivatingToolFlags(Qt::WindowFlags extraFlags = Qt::WindowFlags())
{
    return Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
           Qt::WindowDoesNotAcceptFocus | extraFlags;
}

inline void applyNativeNoActivate(QWindow* window)
{
    if (!window) {
        return;
    }

#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_NOACTIVATE) == 0 || (exStyle & WS_EX_TOOLWINDOW) == 0) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    }

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    Q_UNUSED(window);
#endif
}

inline void showWithoutActivating(QWindow* window)
{
    if (!window) {
        return;
    }

    // Apply native style before show() so Windows won't activate the overlay
    // while Qt creates the platform window, then apply again in case Qt rewrote
    // the style during creation.
    applyNativeNoActivate(window);
    window->show();
    applyNativeNoActivate(window);

#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
#else
    window->raise();
#endif
}

} // namespace OverlayWindowUtils