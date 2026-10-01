#pragma once

#include <QWindow>
#include <array>
#include <mutex>

class QRasterWindow;

// A single Qt-owned video/keyboard surface, with a frame in child surfaces.
// Keeping the video on the toplevel is essential: subsurfaces cannot receive
// the keyboard focus SDL needs for relative mouse input.
class WaylandStreamWindow final : public QWindow
{
public:
    explicit WaylandStreamWindow(std::mutex& surfaceMutex);
    ~WaylandStreamWindow() override;
    bool initializeFrame();
    bool isMinimized() const;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void updateFrame();
    void scheduleSubsurfaceCommit();
    void commitSubsurfaces();

    std::mutex& m_SurfaceMutex;
    std::array<QRasterWindow*, 4> m_Frame{};
    QMargins m_FrameMargins;
    bool m_UpdatingFrame = false;
    bool m_CommitPending = false;
    bool m_Minimized = false;
};
