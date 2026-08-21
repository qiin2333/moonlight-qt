#include "windowplacement.h"

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QWindow>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

namespace {
constexpr auto GeometryKey = "mainwindow/geometry";
constexpr auto WindowsOuterGeometryKey = "mainwindow/windowsOuterGeometry";
constexpr auto WindowsScreenNameKey = "mainwindow/windowsScreen";
constexpr auto ScreenNameKey = "mainwindow/screen";
constexpr int OversizedWindowInset = 32;

int constrainedLength(int requested, int available, int oversizedInset = OversizedWindowInset)
{
    if (available <= 0) {
        return qMax(1, requested);
    }

    if (requested >= available) {
        return qMax(1, available > oversizedInset
                           ? available - oversizedInset
                           : available);
    }

    return qMax(1, requested);
}

QRect constrainedGeometry(const QRect& geometry,
                          const QRect& availableGeometry,
                          int oversizedInset = OversizedWindowInset)
{
    if (!availableGeometry.isValid()) {
        return geometry;
    }

    QRect result = geometry;
    result.setWidth(constrainedLength(result.width(), availableGeometry.width(), oversizedInset));
    result.setHeight(constrainedLength(result.height(), availableGeometry.height(), oversizedInset));

    const int maximumX = availableGeometry.right() - result.width() + 1;
    const int maximumY = availableGeometry.bottom() - result.height() + 1;
    result.moveLeft(qBound(availableGeometry.left(), result.left(), maximumX));
    result.moveTop(qBound(availableGeometry.top(), result.top(), maximumY));
    return result;
}

QScreen* screenForName(const QString& name)
{
    const auto screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (screen->name() == name) {
            return screen;
        }
    }

    return nullptr;
}

#ifdef Q_OS_WIN32
struct NativeMonitorGeometry
{
    HMONITOR handle = nullptr;
    QRect monitor;
    QRect workArea;
    QString name;

    bool isValid() const
    {
        return handle && monitor.isValid() && workArea.isValid();
    }
};

QRect fromNativeRect(const RECT& rect)
{
    return QRect(rect.left,
                 rect.top,
                 rect.right - rect.left,
                 rect.bottom - rect.top);
}

QString rectText(const QRect& rect)
{
    return QStringLiteral("[%1,%2 %3x%4]")
            .arg(rect.x())
            .arg(rect.y())
            .arg(rect.width())
            .arg(rect.height());
}

bool populateNativeMonitorGeometry(HMONITOR monitor, NativeMonitorGeometry& geometry)
{
    if (!monitor) {
        return false;
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&monitorInfo))) {
        return false;
    }

    geometry.handle = monitor;
    geometry.monitor = fromNativeRect(monitorInfo.rcMonitor);
    geometry.workArea = fromNativeRect(monitorInfo.rcWork);
    geometry.name = QString::fromWCharArray(monitorInfo.szDevice);
    return geometry.isValid();
}

struct MonitorSearchContext
{
    QString name;
    NativeMonitorGeometry geometry;
};

BOOL CALLBACK findMonitorByName(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto* context = reinterpret_cast<MonitorSearchContext*>(data);
    NativeMonitorGeometry candidate;
    if (populateNativeMonitorGeometry(monitor, candidate) &&
            candidate.name.compare(context->name, Qt::CaseInsensitive) == 0) {
        context->geometry = candidate;
        return FALSE;
    }
    return TRUE;
}

bool nativeMonitorForName(const QString& name, NativeMonitorGeometry& geometry)
{
    if (name.isEmpty()) {
        return false;
    }

    MonitorSearchContext context { name, {} };
    EnumDisplayMonitors(nullptr,
                        nullptr,
                        findMonitorByName,
                        reinterpret_cast<LPARAM>(&context));
    if (context.geometry.isValid()) {
        geometry = context.geometry;
        return true;
    }
    return false;
}

bool nativeMonitorForScreen(QScreen* screen, NativeMonitorGeometry& geometry)
{
    if (!screen) {
        return false;
    }

    if (nativeMonitorForName(screen->name(), geometry)) {
        return true;
    }

    const QPoint center = screen->geometry().center();
    const POINT nativeCenter { center.x(), center.y() };
    return populateNativeMonitorGeometry(
            MonitorFromPoint(nativeCenter, MONITOR_DEFAULTTONEAREST), geometry);
}

bool nativeMonitorForWindow(HWND window, NativeMonitorGeometry& geometry)
{
    return window && populateNativeMonitorGeometry(
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), geometry);
}

qreal nativeScaleForScreen(const NativeMonitorGeometry& monitor, QScreen* screen)
{
    if (screen && screen->geometry().width() > 0 && monitor.monitor.width() > 0) {
        return static_cast<qreal>(monitor.monitor.width()) / screen->geometry().width();
    }
    if (screen && screen->devicePixelRatio() > 0) {
        return screen->devicePixelRatio();
    }
    return 1.0;
}

QRect relativeLogicalToNative(const QRect& logicalGeometry,
                              const NativeMonitorGeometry& monitor,
                              qreal scale)
{
    return QRect(monitor.monitor.left() + qRound(logicalGeometry.left() * scale),
                 monitor.monitor.top() + qRound(logicalGeometry.top() * scale),
                 qMax(1, qRound(logicalGeometry.width() * scale)),
                 qMax(1, qRound(logicalGeometry.height() * scale)));
}

QRect nativeToRelativeLogical(const QRect& nativeGeometry,
                              const NativeMonitorGeometry& monitor,
                              qreal scale)
{
    if (scale <= 0) {
        scale = 1.0;
    }

    return QRect(qRound((nativeGeometry.left() - monitor.monitor.left()) / scale),
                 qRound((nativeGeometry.top() - monitor.monitor.top()) / scale),
                 qMax(1, qRound(nativeGeometry.width() / scale)),
                 qMax(1, qRound(nativeGeometry.height() / scale)));
}
#endif
}

WindowPlacement::WindowPlacement(QObject* parent)
    : QObject(parent)
{
    m_SaveTimer.setInterval(300);
    m_SaveTimer.setSingleShot(true);
    connect(&m_SaveTimer, &QTimer::timeout, this, &WindowPlacement::saveNow);
}

QWindow* WindowPlacement::window() const
{
    return m_Window;
}

void WindowPlacement::setWindow(QWindow* window)
{
    if (m_Window == window) {
        return;
    }

    m_SaveTimer.stop();
    if (m_Window) {
        disconnect(m_Window, nullptr, this, nullptr);
    }

    m_Window = window;
    if (m_Window) {
        connect(m_Window, &QWindow::xChanged, this, &WindowPlacement::scheduleSave);
        connect(m_Window, &QWindow::yChanged, this, &WindowPlacement::scheduleSave);
        connect(m_Window, &QWindow::widthChanged, this, &WindowPlacement::scheduleSave);
        connect(m_Window, &QWindow::heightChanged, this, &WindowPlacement::scheduleSave);
        connect(m_Window, &QWindow::screenChanged, this, &WindowPlacement::scheduleSave);
        connect(m_Window, &QWindow::visibilityChanged, this, [this](QWindow::Visibility visibility) {
            if (visibility == QWindow::Windowed) {
                scheduleSave();
            }
            else {
                m_SaveTimer.stop();
            }
        });
    }

    emit windowChanged();
}

bool WindowPlacement::isEnabled() const
{
    return m_Enabled;
}

void WindowPlacement::setEnabled(bool enabled)
{
    if (m_Enabled == enabled) {
        return;
    }

    m_Enabled = enabled;
    if (m_Enabled) {
        scheduleSave();
    }
    else {
        m_SaveTimer.stop();
    }

    emit enabledChanged();
}

void WindowPlacement::restore()
{
    if (!m_Window) {
        return;
    }

    QSettings settings;
    const QRect savedGeometry = settings.value(GeometryKey).toRect();
#ifdef Q_OS_WIN32
    const QRect savedWindowsGeometry = settings.value(WindowsOuterGeometryKey).toRect();
    const bool hasSavedWindowsGeometry = m_Enabled && savedWindowsGeometry.isValid();
#else
    const bool hasSavedGeometry = m_Enabled && savedGeometry.isValid();
#endif

#ifdef Q_OS_WIN32
    const bool hasLegacyGeometry = m_Enabled && !hasSavedWindowsGeometry && savedGeometry.isValid();
    QScreen* screen = hasSavedWindowsGeometry
            ? screenForName(settings.value(ScreenNameKey).toString())
            : (hasLegacyGeometry
                       ? findSavedScreen(settings.value(ScreenNameKey).toString(), savedGeometry)
                       : m_Window->screen());
#else
    QScreen* screen = hasSavedGeometry
            ? findSavedScreen(settings.value(ScreenNameKey).toString(), savedGeometry)
            : m_Window->screen();
#endif
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }

    m_Restoring = true;
#ifdef Q_OS_WIN32
    const HWND nativeWindow = reinterpret_cast<HWND>(m_Window->winId());
    NativeMonitorGeometry nativeMonitor;
    const QString savedNativeScreenName = settings.value(WindowsScreenNameKey).toString();
    if (!nativeMonitorForName(savedNativeScreenName, nativeMonitor) &&
            !nativeMonitorForScreen(screen, nativeMonitor)) {
        nativeMonitorForWindow(nativeWindow, nativeMonitor);
    }

    RECT currentWindowRect = {};
    const bool hasCurrentWindowRect = nativeWindow && GetWindowRect(nativeWindow, &currentWindowRect);
    if (nativeWindow && nativeMonitor.isValid() && hasCurrentWindowRect) {
        const qreal scale = nativeScaleForScreen(nativeMonitor, screen);
        QRect requestedNativeGeometry = fromNativeRect(currentWindowRect);
        QRect logicalGeometry;

        if (hasSavedWindowsGeometry) {
            logicalGeometry = savedWindowsGeometry;
            requestedNativeGeometry = relativeLogicalToNative(logicalGeometry, nativeMonitor, scale);
        }
        else if (hasLegacyGeometry) {
            logicalGeometry = savedGeometry.translated(-screen->geometry().topLeft());
            requestedNativeGeometry = relativeLogicalToNative(logicalGeometry, nativeMonitor, scale);
        }

        const int nativeInset = qMax(1, qRound(OversizedWindowInset * scale));
        const QRect constrainedNativeGeometry = constrainedGeometry(
                requestedNativeGeometry, nativeMonitor.workArea, nativeInset);

        if (!SetWindowPos(nativeWindow,
                          nullptr,
                          constrainedNativeGeometry.x(),
                          constrainedNativeGeometry.y(),
                          constrainedNativeGeometry.width(),
                          constrainedNativeGeometry.height(),
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER)) {
            qWarning() << "Failed to restore native window geometry:" << GetLastError();
        }

        RECT restoredWindowRect = {};
        GetWindowRect(nativeWindow, &restoredWindowRect);
        qInfo().noquote()
                << QStringLiteral("Window placement restore screen=%1 saved=%2 requestedNative=%3 "
                                  "constrainedNative=%4 actualNative=%5 scale=%6")
                           .arg(nativeMonitor.name,
                                logicalGeometry.isValid() ? rectText(logicalGeometry)
                                                          : QStringLiteral("default"),
                                rectText(requestedNativeGeometry),
                                rectText(constrainedNativeGeometry),
                                rectText(fromNativeRect(restoredWindowRect)))
                           .arg(scale);
    }
    else {
        const QRect availableGeometry = screen->availableGeometry();
        if (hasLegacyGeometry) {
            m_Window->setScreen(screen);
            m_Window->setGeometry(constrainGeometry(savedGeometry, availableGeometry));
        }
        else {
            const QSize requestedSize = m_Window->size();
            m_Window->resize(constrainedLength(requestedSize.width(), availableGeometry.width()),
                             constrainedLength(requestedSize.height(), availableGeometry.height()));
        }
    }
#else
    if (hasSavedGeometry) {
        m_Window->setScreen(screen);
        m_Window->setGeometry(constrainGeometry(savedGeometry, screen->availableGeometry()));
    }
    else {
        const QRect availableGeometry = screen->availableGeometry();
        const QSize requestedSize = m_Window->size();
        m_Window->resize(constrainedLength(requestedSize.width(), availableGeometry.width()),
                         constrainedLength(requestedSize.height(), availableGeometry.height()));
    }
#endif
    m_Restoring = false;
}

void WindowPlacement::flush()
{
    m_SaveTimer.stop();
    saveNow();
}

QScreen* WindowPlacement::findSavedScreen(const QString& name, const QRect& geometry)
{
    if (QScreen* screen = screenForName(name)) {
        return screen;
    }

    if (QScreen* screen = QGuiApplication::screenAt(geometry.center())) {
        return screen;
    }

    return QGuiApplication::primaryScreen();
}

QRect WindowPlacement::constrainGeometry(const QRect& geometry, const QRect& availableGeometry)
{
    return constrainedGeometry(geometry, availableGeometry);
}

void WindowPlacement::scheduleSave()
{
    if (m_Enabled && !m_Restoring && m_Window &&
            m_Window->visibility() == QWindow::Windowed) {
        m_SaveTimer.start();
    }
}

void WindowPlacement::saveNow()
{
    if (!m_Enabled || m_Restoring || !m_Window ||
            m_Window->visibility() != QWindow::Windowed ||
            !m_Window->geometry().isValid()) {
        return;
    }

    QSettings settings;
#ifdef Q_OS_WIN32
    const HWND nativeWindow = reinterpret_cast<HWND>(m_Window->winId());
    if (!nativeWindow || IsIconic(nativeWindow) || IsZoomed(nativeWindow)) {
        return;
    }

    RECT nativeWindowRect = {};
    NativeMonitorGeometry nativeMonitor;
    if (!GetWindowRect(nativeWindow, &nativeWindowRect) ||
            !nativeMonitorForWindow(nativeWindow, nativeMonitor)) {
        return;
    }

    QScreen* screen = m_Window->screen();
    if (!screen) {
        screen = QGuiApplication::screenAt(m_Window->geometry().center());
    }
    const qreal scale = nativeScaleForScreen(nativeMonitor, screen);
    const QRect nativeGeometry = fromNativeRect(nativeWindowRect);
    const QRect logicalGeometry = nativeToRelativeLogical(
            nativeGeometry, nativeMonitor, scale);

    settings.setValue(WindowsOuterGeometryKey, logicalGeometry);
    settings.setValue(WindowsScreenNameKey, nativeMonitor.name);
    settings.setValue(ScreenNameKey, screen ? screen->name() : nativeMonitor.name);
    qInfo().noquote()
            << QStringLiteral("Window placement save screen=%1 native=%2 relativeLogical=%3 scale=%4")
                       .arg(nativeMonitor.name,
                            rectText(nativeGeometry),
                            rectText(logicalGeometry))
                       .arg(scale);
#else
    QScreen* screen = QGuiApplication::screenAt(m_Window->geometry().center());
    if (!screen) {
        screen = m_Window->screen();
    }

    settings.setValue(GeometryKey, m_Window->geometry());
    if (screen) {
        settings.setValue(ScreenNameKey, screen->name());
    }
#endif
    settings.sync();
}
