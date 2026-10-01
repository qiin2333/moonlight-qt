#include "waylandstreamwindow.h"
#include "SDL_compat.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QRasterWindow>
#include <QScopedValueRollback>
#include <QTimer>
#include <QtGui/qguiapplication_platform.h>
#include <qpa/qplatformwindow_p.h>
#include <wayland-client.h>

namespace {
constexpr int Border = 5;
constexpr int TitleHeight = 32;
constexpr int ButtonWidth = 40;

class FrameStrip final : public QRasterWindow
{
public:
    FrameStrip(QWindow* window, Qt::Edge edge)
        : QRasterWindow(window), m_Window(window), m_Edge(edge)
    {
        setFlags(Qt::SubWindow | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        setObjectName(edge == Qt::TopEdge ? QStringLiteral("wayland-stream-titlebar")
                                          : QStringLiteral("wayland-stream-border"));
        connect(window, &QWindow::windowTitleChanged, this, [this]() { update(); });
        connect(window, &QWindow::activeChanged, this, [this]() { update(); });
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const QPalette palette = QGuiApplication::palette();
        painter.fillRect(QRect(QPoint(), size()), palette.window());
        if (m_Edge != Qt::TopEdge) {
            return;
        }
        painter.setPen(palette.windowText().color());
        const QRect titleRect(Border + 10, Border,
                              qMax(0, width() - 2 * Border - 3 * ButtonWidth - 20), TitleHeight);
        painter.drawText(
            titleRect, Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(m_Window->title(), Qt::ElideRight, titleRect.width()));
        for (int button = 0; button < 3; ++button) {
            const QRect rect = buttonRect(button);
            if (button == m_HoveredButton) {
                painter.fillRect(rect, button == 2 ? QColor(190, 45, 45) : palette.mid().color());
            }
            const QPoint center = rect.center();
            if (button == 0) {
                painter.drawLine(center + QPoint(-5, 3), center + QPoint(5, 3));
            } else if (button == 1) {
                painter.drawRect(QRect(center - QPoint(5, 5), QSize(10, 10)));
                if (m_Window->windowStates().testFlag(Qt::WindowMaximized)) {
                    painter.drawLine(center + QPoint(-2, -3), center + QPoint(3, -3));
                }
            } else {
                painter.drawLine(center + QPoint(-5, -5), center + QPoint(5, 5));
                painter.drawLine(center + QPoint(-5, 5), center + QPoint(5, -5));
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const QPoint point = event->position().toPoint();
        m_PressedButton = buttonAt(point);
        if (m_PressedButton < 0) {
            const Qt::Edges edges = resizeEdges(point);
            if (edges && !m_Window->windowStates().testFlag(Qt::WindowMaximized)) {
                m_Window->startSystemResize(edges);
            } else if (m_Edge == Qt::TopEdge) {
                m_Window->startSystemMove();
            }
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const int button = m_PressedButton;
        m_PressedButton = -1;
        if (button >= 0 && button == buttonAt(event->position().toPoint())) {
            if (button == 0) {
                m_Window->showMinimized();
            } else if (button == 1) {
                toggleMaximized();
            } else {
                m_Window->close();
            }
        }
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_Edge == Qt::TopEdge &&
            buttonAt(event->position().toPoint()) < 0) {
            toggleMaximized();
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const QPoint point = event->position().toPoint();
        const int button = buttonAt(point);
        if (button != m_HoveredButton) {
            m_HoveredButton = button;
            update();
        }
        const Qt::Edges edges = resizeEdges(point);
        Qt::CursorShape cursor = Qt::ArrowCursor;
        if (!m_Window->windowStates().testFlag(Qt::WindowMaximized)) {
            if (edges == (Qt::TopEdge | Qt::LeftEdge) ||
                edges == (Qt::BottomEdge | Qt::RightEdge)) {
                cursor = Qt::SizeFDiagCursor;
            } else if (edges == (Qt::TopEdge | Qt::RightEdge) ||
                       edges == (Qt::BottomEdge | Qt::LeftEdge)) {
                cursor = Qt::SizeBDiagCursor;
            } else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) {
                cursor = Qt::SizeHorCursor;
            } else if (edges) {
                cursor = Qt::SizeVerCursor;
            }
        }
        setCursor(cursor);
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::Leave) {
            m_HoveredButton = -1;
            update();
        }
        return QRasterWindow::event(event);
    }

private:
    QRect buttonRect(int button) const
    {
        return QRect(width() - Border - (3 - button) * ButtonWidth, Border, ButtonWidth,
                     TitleHeight);
    }

    int buttonAt(const QPoint& point) const
    {
        if (m_Edge == Qt::TopEdge) {
            for (int button = 0; button < 3; ++button) {
                if (buttonRect(button).contains(point)) {
                    return button;
                }
            }
        }
        return -1;
    }

    Qt::Edges resizeEdges(const QPoint& point) const
    {
        if (m_Edge == Qt::LeftEdge || m_Edge == Qt::RightEdge) {
            return m_Edge;
        }
        Qt::Edges edges;
        if (m_Edge == Qt::BottomEdge || point.y() < Border) {
            edges |= m_Edge;
        }
        if (point.x() < Border) {
            edges |= Qt::LeftEdge;
        } else if (point.x() >= width() - Border) {
            edges |= Qt::RightEdge;
        }
        return edges;
    }

    void toggleMaximized()
    {
        if (m_Window->windowStates().testFlag(Qt::WindowMaximized)) {
            m_Window->showNormal();
        } else {
            m_Window->showMaximized();
        }
    }

    QWindow* const m_Window;
    const Qt::Edge m_Edge;
    int m_HoveredButton = -1;
    int m_PressedButton = -1;
};
}

WaylandStreamWindow::WaylandStreamWindow(std::mutex& surfaceMutex) : m_SurfaceMutex(surfaceMutex)
{
    setFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(QSize(3 * ButtonWidth, TitleHeight));
    // xdg-shell does not report minimized state. Qt clears WindowMinimized
    // after sending the request, so retain it until the compositor activates
    // this window again. Observe the signal to cover every minimize entry
    // point, including the titlebar, shortcut, and initial window state.
    connect(this, &QWindow::windowStateChanged, this, [this](Qt::WindowState state) {
        if (state == Qt::WindowMinimized) {
            m_Minimized = true;
        }
    });
    connect(this, &QWindow::activeChanged, this, [this]() {
        if (isActive()) {
            m_Minimized = false;
        }
    });
    m_Frame = { new FrameStrip(this, Qt::TopEdge), new FrameStrip(this, Qt::LeftEdge),
                new FrameStrip(this, Qt::RightEdge), new FrameStrip(this, Qt::BottomEdge) };
    qGuiApp->installEventFilter(this);
}

WaylandStreamWindow::~WaylandStreamWindow()
{
    qGuiApp->removeEventFilter(this);
}

bool WaylandStreamWindow::isMinimized() const
{
    return m_Minimized;
}

bool WaylandStreamWindow::initializeFrame()
{
    create();
    if (!nativeInterface<QNativeInterface::Private::QWaylandWindow>()) {
        return false;
    }
    const QSize clientSize = size();
    updateFrame();
    // setCustomMargins preserves the outer size. At creation the caller
    // supplied a video client size, which must not shrink to fit the frame.
    resize(clientSize);
    return true;
}

void WaylandStreamWindow::updateFrame()
{
    auto* native = nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (m_UpdatingFrame || native == nullptr) {
        return;
    }
    QScopedValueRollback<bool> updating(m_UpdatingFrame, true);
    const bool framed = !windowStates().testFlag(Qt::WindowFullScreen);
    const QMargins margins =
        framed ? QMargins(-Border, -TitleHeight - Border, -Border, -Border) : QMargins();
    if (m_FrameMargins != margins) {
        m_FrameMargins = margins;
        native->setCustomMargins(margins);
    }
    m_Frame[0]->setGeometry(-Border, -TitleHeight - Border, width() + 2 * Border,
                            TitleHeight + Border);
    m_Frame[1]->setGeometry(-Border, 0, Border, height());
    m_Frame[2]->setGeometry(width(), 0, Border, height());
    m_Frame[3]->setGeometry(-Border, height(), width() + 2 * Border, Border);
    for (auto* strip : m_Frame) {
        strip->setVisible(framed && isVisible());
        strip->update();
    }
    scheduleSubsurfaceCommit();
}

bool WaylandStreamWindow::event(QEvent* event)
{
    const bool handled = QWindow::event(event);
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowStateChange:
        updateFrame();
        break;
    default:
        break;
    }
    return handled;
}

bool WaylandStreamWindow::eventFilter(QObject* watched, QEvent* event)
{
    auto* child = qobject_cast<QWindow*>(watched);
    if (child != nullptr && child->parent() == this) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Expose:
        case QEvent::UpdateRequest:
            // Run after Qt has processed the event and flushed the child.
            // Desynchronized subsurface positions still need a parent commit.
            scheduleSubsurfaceCommit();
            break;
        default:
            break;
        }
    }
    return false;
}

void WaylandStreamWindow::scheduleSubsurfaceCommit()
{
    if (!m_CommitPending) {
        m_CommitPending = true;
        QTimer::singleShot(0, this, [this]() { commitSubsurfaces(); });
    }
}

void WaylandStreamWindow::commitSubsurfaces()
{
    // Do not interleave a commit with the renderer's buffer attach/present.
    // Never block the Qt event pump waiting for a renderer that needs it.
    std::unique_lock<std::mutex> lock(m_SurfaceMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        QTimer::singleShot(8, this, [this]() { commitSubsurfaces(); });
        return;
    }
    m_CommitPending = false;
    auto* native = nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (native != nullptr && native->surface() != nullptr && isExposed()) {
        wl_surface_commit(native->surface());
        auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        wl_display_flush(wayland->display());
    }
}

void WaylandStreamWindow::closeEvent(QCloseEvent* event)
{
    SDL_Event quitEvent = {};
    quitEvent.type = SDL_QUIT;
    if (SDL_PushEvent(&quitEvent) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to queue quit for Wayland stream window: %s", SDL_GetError());
    }
    // SDL and its decoder must release the borrowed surface before Qt does.
    event->ignore();
}
