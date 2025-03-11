#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>

class BandwidthCalculator : public QObject
{
    Q_OBJECT

public:
    static BandwidthCalculator* instance();
    
    // 更新传输的字节数
    void addBytes(qint64 bytes);
    
    // 获取当前带宽（Kbps）
    int getCurrentBandwidthKbps();
    
    // 开始/停止计算
    void start();
    void stop();

private slots:
    void updateBandwidth();

private:
    BandwidthCalculator();
    ~BandwidthCalculator();
    
    static BandwidthCalculator* s_instance;
    
    QTimer m_updateTimer;
    QElapsedTimer m_elapsedTimer;
    
    std::atomic<qint64> m_bytesReceived;
    std::atomic<qint64> m_lastBytesReceived;
    std::atomic<int> m_currentBandwidthKbps;
    
    bool m_running;
};
