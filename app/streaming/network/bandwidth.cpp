#include "bandwidth.h"
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

BandwidthCalculator *BandwidthCalculator::s_instance = nullptr;

BandwidthCalculator *BandwidthCalculator::instance()
{
    if (s_instance == nullptr)
    {
        s_instance = new BandwidthCalculator();
    }
    return s_instance;
}

class SystemNetworkStats
{
public:
    static bool getNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent);

private:
#ifdef Q_OS_WIN
    static bool getWindowsNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent);
#elif defined(Q_OS_LINUX)
    static bool getLinuxNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent);
#elif defined(Q_OS_DARWIN)
    static bool getMacOSNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent);
#endif
};

BandwidthCalculator::BandwidthCalculator() : m_bytesReceived(0),
                                             m_lastBytesReceived(0),
                                             m_currentBandwidthKbps(0),
                                             m_running(false)
{
    connect(&m_updateTimer, &QTimer::timeout,
            this, &BandwidthCalculator::updateBandwidth);
}

BandwidthCalculator::~BandwidthCalculator()
{
    stop();
}

void BandwidthCalculator::addBytes(qint64 bytes)
{
    if (m_running)
    {
        m_bytesReceived += bytes;
    }
}

int BandwidthCalculator::getCurrentBandwidthKbps()
{
    return m_currentBandwidthKbps.load();
}

void BandwidthCalculator::start()
{
    if (!m_running)
    {
        m_bytesReceived = 0;
        m_lastBytesReceived = 0;
        m_currentBandwidthKbps = 0;
        m_elapsedTimer.start();
        m_updateTimer.start(1000); // 每秒更新一次
        m_running = true;
    }
}

void BandwidthCalculator::stop()
{
    if (m_running)
    {
        m_updateTimer.stop();
        m_running = false;
    }
}

void BandwidthCalculator::updateBandwidth()
{
    qint64 currentBytes = 0;
    qint64 sentBytes = 0;

    // 从系统获取实际网络使用情况
    if (SystemNetworkStats::getNetworkUsage(currentBytes, sentBytes))
    {
        m_bytesReceived = currentBytes;
    }
    else
    {
        // 如果获取失败，则使用手动计算的值
        currentBytes = m_bytesReceived.load();
    }

    qint64 bytesTransferred = currentBytes - m_lastBytesReceived;

    // 计算带宽（bits per second）
    qint64 elapsedMs = m_elapsedTimer.restart();
    if (elapsedMs > 0)
    {
        // 转换为Kbps (bits per second / 1000)
        m_currentBandwidthKbps = static_cast<int>((bytesTransferred * 8.0 * 1000) / elapsedMs / 1000);
    }

    m_lastBytesReceived = currentBytes;
}

// 系统网络统计实现
bool SystemNetworkStats::getNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent)
{
#ifdef Q_OS_WIN
    return getWindowsNetworkUsage(bytesReceived, bytesSent);
#elif defined(Q_OS_LINUX)
    return getLinuxNetworkUsage(bytesReceived, bytesSent);
#elif defined(Q_OS_DARWIN)
    return getMacOSNetworkUsage(bytesReceived, bytesSent);
#else
    return false;
#endif
}

#ifdef Q_OS_WIN
bool SystemNetworkStats::getWindowsNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent)
{
    // 初始化计数器
    bytesReceived = 0;
    bytesSent = 0;

    // 需要包含头文件：
    // #include <winsock2.h>
    // #include <ws2tcpip.h>
    // #include <iphlpapi.h>
    // 并链接 iphlpapi.lib

    PMIB_IF_TABLE2 pIfTable = NULL;
    ULONG error = GetIfTable2(&pIfTable);

    if (error != NO_ERROR)
    {
        return false;
    }

    // 累加所有网络接口的流量
    for (ULONG i = 0; i < pIfTable->NumEntries; i++)
    {
        MIB_IF_ROW2 row = pIfTable->Table[i];

        // 跳过未连接或虚拟接口
        if (row.OperStatus != IfOperStatusUp ||
            row.MediaType == NdisMediumLoopback ||
            row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }

        bytesReceived += row.InOctets;
        bytesSent += row.OutOctets;
    }

    // 释放表资源
    FreeMibTable(pIfTable);

    return true;
}
#elif defined(Q_OS_LINUX)
bool SystemNetworkStats::getLinuxNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent)
{
    // Linux 实现：读取 /proc/net/dev 文件
    QFile file("/proc/net/dev");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    bytesReceived = 0;
    bytesSent = 0;

    QTextStream in(&file);
    QString line = in.readLine(); // 跳过前两行标题
    line = in.readLine();

    while (!in.atEnd())
    {
        line = in.readLine();
        QStringList parts = line.trimmed().split(QRegExp("\\s+"));
        if (parts.size() >= 10)
        {
            // 每个网络接口行格式：Interface: rx_bytes ... tx_bytes ...
            QString interface = parts[0].remove(":");
            if (interface != "lo")
            { // 忽略本地回环接口
                bytesReceived += parts[1].toLongLong();
                bytesSent += parts[9].toLongLong();
            }
        }
    }

    file.close();
    return true;
}
#elif defined(Q_OS_DARWIN)
bool SystemNetworkStats::getMacOSNetworkUsage(qint64 &bytesReceived, qint64 &bytesSent)
{
    // macOS 实现：使用 getifaddrs 或 IOKit
    // TODO: 实现具体代码
    return false;
}
#endif

// 适配Limelight接口的函数实现
bool LiGetBandwidthStatistics(int *bandwidthKbps, float *packetLossRate)
{
    if (bandwidthKbps != nullptr)
    {
        *bandwidthKbps = BandwidthCalculator::instance()->getCurrentBandwidthKbps();
    }

    // 我们目前没有实现丢包率计算
    if (packetLossRate != nullptr)
    {
        *packetLossRate = 0.0f;
    }

    return true;
}