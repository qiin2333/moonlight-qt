#include "windowswindowchrome.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <windowsx.h>
#endif

WindowsWindowChrome::WindowsWindowChrome(QObject* parent)
    : QObject(parent)
{
}

WindowsWindowChrome::~WindowsWindowChrome()
{
    if (m_Installed && QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
}

QWindow* WindowsWindowChrome::window() const
{
    return m_Window;
}

void WindowsWindowChrome::setWindow(QWindow* window)
{
    if (m_Window == window) {
        return;
    }

    m_Window = window;
    m_WindowId = 0;
    emit windowChanged();
}

QQuickItem* WindowsWindowChrome::titleBar() const
{
    return m_TitleBar;
}

void WindowsWindowChrome::setTitleBar(QQuickItem* titleBar)
{
    if (m_TitleBar == titleBar) {
        return;
    }

    m_TitleBar = titleBar;
    emit titleBarChanged();
}

void WindowsWindowChrome::activate()
{
#ifdef Q_OS_WIN32
    if (!m_Window) {
        return;
    }

    m_WindowId = m_Window->winId();
    const HWND nativeWindow = reinterpret_cast<HWND>(m_WindowId);
    const LONG_PTR style = GetWindowLongPtrW(nativeWindow, GWL_STYLE);
    SetWindowLongPtrW(nativeWindow, GWL_STYLE,
                      style | WS_THICKFRAME | WS_MINIMIZEBOX |
                              WS_MAXIMIZEBOX | WS_SYSMENU);
    if (!m_Installed && QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        m_Installed = true;
    }
#endif
}

bool WindowsWindowChrome::nativeEventFilter(const QByteArray& eventType,
                                            void* message,
                                            qintptr* result)
{
#ifdef Q_OS_WIN32
    if (eventType != "windows_generic_MSG" || !m_Window || !m_TitleBar || !m_WindowId) {
        return false;
    }

    auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage->hwnd != reinterpret_cast<HWND>(m_WindowId)) {
        return false;
    }

    if (nativeMessage->message == WM_GETMINMAXINFO) {
        const HMONITOR monitor = MonitorFromWindow(nativeMessage->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
            auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(nativeMessage->lParam);
            minMaxInfo->ptMaxPosition.x = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
            minMaxInfo->ptMaxPosition.y = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;
            minMaxInfo->ptMaxSize.x = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
            minMaxInfo->ptMaxSize.y = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
            *result = 0;
            return true;
        }
    }

    if (nativeMessage->message == WM_NCLBUTTONDBLCLK && nativeMessage->wParam == HTCAPTION) {
        if (m_Window->visibility() == QWindow::Maximized) {
            m_Window->showNormal();
        }
        else {
            m_Window->showMaximized();
        }
        *result = 0;
        return true;
    }

    if (nativeMessage->message != WM_NCHITTEST ||
            m_Window->visibility() == QWindow::FullScreen ||
            !m_TitleBar->isVisible()) {
        return false;
    }

    const POINT screenPoint = {
        GET_X_LPARAM(nativeMessage->lParam),
        GET_Y_LPARAM(nativeMessage->lParam)
    };

    if (m_Window->visibility() == QWindow::Windowed && !IsZoomed(nativeMessage->hwnd)) {
        RECT windowRect;
        if (GetWindowRect(nativeMessage->hwnd, &windowRect)) {
            const UINT dpi = GetDpiForWindow(nativeMessage->hwnd);
            const int horizontalBorder = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                                         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int verticalBorder = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                                       GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

            const bool onLeft = screenPoint.x < windowRect.left + horizontalBorder;
            const bool onRight = screenPoint.x >= windowRect.right - horizontalBorder;
            const bool onTop = screenPoint.y < windowRect.top + verticalBorder;
            const bool onBottom = screenPoint.y >= windowRect.bottom - verticalBorder;

            if (onTop && onLeft) {
                *result = HTTOPLEFT;
                return true;
            }
            if (onTop && onRight) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (onBottom && onLeft) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (onBottom && onRight) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (onLeft) {
                *result = HTLEFT;
                return true;
            }
            if (onRight) {
                *result = HTRIGHT;
                return true;
            }
            if (onTop) {
                *result = HTTOP;
                return true;
            }
            if (onBottom) {
                *result = HTBOTTOM;
                return true;
            }
        }
    }

    POINT clientPoint = screenPoint;
    if (!ScreenToClient(nativeMessage->hwnd, &clientPoint)) {
        return false;
    }

    const qreal scale = m_Window->devicePixelRatio();
    const QPointF scenePoint(clientPoint.x / scale, clientPoint.y / scale);
    const QRectF titleBarRect = m_TitleBar->mapRectToScene(
            QRectF(0, 0, m_TitleBar->width(), m_TitleBar->height()));
    if (titleBarRect.contains(scenePoint)) {
        *result = HTCAPTION;
        return true;
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif

    return false;
}
