#pragma once

#include <QObject>
#include <QString>

class UsbForwardingEnvironment : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString usbipdVersion READ usbipdVersion NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
    enum State
    {
        Checking,
        NotInstalled,
        ServiceStopped,
        Ready,
    };
    Q_ENUM(State)

    static UsbForwardingEnvironment* get();

    Q_INVOKABLE void refresh();

    State state() const { return m_State; }
    QString usbipdVersion() const { return m_Version; }
    bool checking() const { return m_Checking; }

signals:
    void stateChanged();
    void checkingChanged();

private:
    explicit UsbForwardingEnvironment(QObject *parent = nullptr);

    void startVersionProbe(const QString &usbipdExe);
    void startServiceProbe();
    void finish(State state);

    State m_State = Checking;
    QString m_Version;
    bool m_Checking = false;
    QString m_UsbipdExe;
};
