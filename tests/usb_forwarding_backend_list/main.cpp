// moonlight-usbd `list --json` 输出 → QVariantMap 设备表的纯函数解析测试。
// 不 spawn helper，Windows/Linux 开发机可跑（helper 本身只在 macOS 构建）。
#include "../../app/backend/usbforwardingbackend.h"
#include <QCoreApplication>
#include <QDebug>
#include <QVariantMap>

// wm.cpp 拖入 SDL/X11 依赖，这里用桩替代（非 Wayland 平台真实现同样返回 false）。
#include "../../app/utils.h"
bool WMUtils::isRunningWayland() { return false; }
bool WMUtils::isGpuSlow() { return false; }

static QVariantMap firstDevice(const char* json)
{
    QString error;
    const QVariantList devices = UsbForwardingBackend::parseHelperDevices(json, &error);
    if (error.isEmpty() && devices.size() == 1) {
        return devices.first().toMap();
    }
    return {};
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);

    // 完整设备：经 hub 的点分 busid、可占用 → supported。
    {
        const QVariantMap device = firstDevice(
            "[{\"busId\":\"1-2.3\",\"vid\":1351,\"pid\":3302,\"vidPid\":\"054C:0CE6\","
            "\"serial\":\"0102030\",\"manufacturer\":\"Sony Interactive Entertainment\","
            "\"product\":\"DualSense Wireless Controller\",\"claimable\":true}]");
        if (device.isEmpty()
                || device.value("busId").toString() != "1-2.3"
                || device.value("description").toString() != "DualSense Wireless Controller"
                || device.value("instanceId").toString() != "0102030"
                || device.value("vidPid").toString() != "054c:0ce6"
                || device.value("isBound").toBool() // 解析层恒 false，由 refresh 按偏好叠加
                || !device.value("isConnected").toBool()
                || device.value("isAttached").toBool()
                || !device.value("isSupported").toBool()
                || device.value("isOccupied").toBool()
                || device.value("isForced").toBool()
                || !device.value("persistedGuid").toString().isEmpty()) {
            qWarning() << "full device parse mismatch:" << device;
            return 1;
        }
    }

    // 被系统占用（claimable=false）：显示「In use by macOS」，不可共享。
    {
        const QVariantMap device = firstDevice(
            "[{\"busId\":\"2-1\",\"vid\":1,\"pid\":2,\"vidPid\":\"0001:0002\","
            "\"serial\":\"\",\"manufacturer\":\"\",\"product\":\"USB Keyboard\","
            "\"claimable\":false}]");
        if (device.isEmpty()
                || device.value("isSupported").toBool()
                || !device.value("isOccupied").toBool()) {
            qWarning() << "occupied device parse mismatch:" << device;
            return 2;
        }
    }

    // 非法 busid（哪怕 claimable）不可共享。
    {
        const QVariantMap device = firstDevice(
            "[{\"busId\":\"IncompatibleHub\",\"vid\":1,\"pid\":2,\"vidPid\":\"0001:0002\","
            "\"serial\":\"\",\"manufacturer\":\"\",\"product\":\"X\",\"claimable\":true}]");
        if (device.isEmpty() || device.value("isSupported").toBool()) {
            qWarning() << "bad busid should be unsupported:" << device;
            return 3;
        }
    }

    // 描述回退链：product 空 → manufacturer；再空 → vidPid。
    {
        const QVariantMap device = firstDevice(
            "[{\"busId\":\"3-1\",\"vid\":1027,\"pid\":24577,\"vidPid\":\"0403:6001\","
            "\"serial\":\"A\",\"manufacturer\":\"FTDI\",\"product\":\"\",\"claimable\":true}]");
        if (device.value("description").toString() != "FTDI") {
            qWarning() << "description fallback to manufacturer failed:" << device;
            return 4;
        }
        const QVariantMap bare = firstDevice(
            "[{\"busId\":\"3-2\",\"vid\":1027,\"pid\":24577,\"vidPid\":\"0403:6001\","
            "\"serial\":\"\",\"manufacturer\":\"\",\"product\":\"\",\"claimable\":true}]");
        if (bare.value("description").toString() != "0403:6001") {
            qWarning() << "description fallback to vidPid failed:" << bare;
            return 5;
        }
    }

    // 空数组：空表、无错误。
    {
        QString error = QStringLiteral("stale");
        const QVariantList devices = UsbForwardingBackend::parseHelperDevices("[]", &error);
        if (!devices.isEmpty() || !error.isEmpty()) return 6;
    }

    // 坏输入：非数组 / 非 JSON / 截断。
    for (const char* invalid : {"{}", "not json", "[{\"busId\":", "42"}) {
        QString error;
        const QVariantList devices = UsbForwardingBackend::parseHelperDevices(invalid, &error);
        if (!devices.isEmpty() || error.isEmpty()) {
            qWarning() << "malformed input should set error:" << invalid;
            return 7;
        }
    }

    qInfo() << "PASS helper list parsing: dotted busid, occupied, fallbacks, empty, malformed";
    return 0;
}
