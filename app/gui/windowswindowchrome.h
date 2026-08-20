#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QWindow>

class WindowsWindowChrome : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(QQuickItem* titleBar READ titleBar WRITE setTitleBar NOTIFY titleBarChanged)

public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using NativeEventResult = qintptr;
#else
    using NativeEventResult = long;
#endif

    explicit WindowsWindowChrome(QObject* parent = nullptr);
    ~WindowsWindowChrome() override;

    QWindow* window() const;
    void setWindow(QWindow* window);

    QQuickItem* titleBar() const;
    void setTitleBar(QQuickItem* titleBar);

    Q_INVOKABLE void activate();

    bool nativeEventFilter(const QByteArray& eventType, void* message, NativeEventResult* result) override;

signals:
    void windowChanged();
    void titleBarChanged();

private:
    QPointer<QWindow> m_Window;
    QPointer<QQuickItem> m_TitleBar;
    WId m_WindowId = 0;
    bool m_Installed = false;
};
