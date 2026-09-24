// Linux sysfs 设备枚举 → 设备表解析的纯函数测试。
// 用运行时生成的夹具目录模拟 /sys/bus/usb/devices。driver symlink 只在
// Unix 上创建（Windows 建链需要特权），绑定断言相应分平台。
#include "../../app/backend/usbforwardingbackend.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVariantMap>

// wm.cpp 拖入 SDL/X11 依赖，这里用桩替代（同 usb_forwarding_backend_list）。
#include "../../app/utils.h"
bool WMUtils::isRunningWayland()
{
    return false;
}
bool WMUtils::isGpuSlow()
{
    return false;
}

static void writeAttr(const QDir &base, const QString &rel, const QByteArray &content)
{
    base.mkpath(QFileInfo(base.filePath(rel)).path());
    QFile file(base.filePath(rel));
    file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    file.write(content);
}

static QVariantMap findDevice(const QVariantList &devices, const QString &busId)
{
    for (const QVariant &entry : devices) {
        const QVariantMap device = entry.toMap();
        if (device.value("busId").toString() == busId) {
            return device;
        }
    }
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    QTemporaryDir fixture;
    if (!fixture.isValid()) {
        qCritical() << "no temp dir";
        return 2;
    }
    const QDir root(fixture.path());

    // 完整设备：经 hub 的点分 busid、有序列号，绑定 usbip-host（仅 Unix 建链）。
    writeAttr(root, "1-1/idVendor", "076B\n");
    writeAttr(root, "1-1/idProduct", "6666\n");
    writeAttr(root, "1-1/bDeviceClass", "00\n");
    writeAttr(root, "1-1/product", "Spike USB Stick\n");
    writeAttr(root, "1-1/manufacturer", "Spike Labs\n");
    writeAttr(root, "1-1/serial", "spike0001\n");
#ifdef Q_OS_UNIX
    QFile::link(QStringLiteral("/sys/bus/usb/drivers/usbip-host"),
                root.filePath(QStringLiteral("1-1/driver")));
#endif

    // 最小设备：只有 VID/PID，无驱动绑定。
    writeAttr(root, "1-2/idVendor", "054c\n");
    writeAttr(root, "1-2/idProduct", "0ce6\n");
    writeAttr(root, "1-2/bDeviceClass", "00\n");

    // 换装设备：同 VID/PID、不同序列号，已绑定（仅 Unix 建链）——
    // 模拟「共享后同端口插上另一台设备被 usbip-host 认领」。
    writeAttr(root, "3-1/idVendor", "054c\n");
    writeAttr(root, "3-1/idProduct", "0ce6\n");
    writeAttr(root, "3-1/bDeviceClass", "00\n");
    writeAttr(root, "3-1/serial", "changed-serial\n");
#ifdef Q_OS_UNIX
    QFile::link(QStringLiteral("/sys/bus/usb/drivers/usbip-host"),
                root.filePath(QStringLiteral("3-1/driver")));
#endif

    // hub（bDeviceClass 09）→ 跳过。
    writeAttr(root, "2-1.2/idVendor", "1d6b\n");
    writeAttr(root, "2-1.2/idProduct", "0104\n");
    writeAttr(root, "2-1.2/bDeviceClass", "09\n");

    // 接口目录与根 hub → 跳过。
    writeAttr(root, "1-1:1.0/bInterfaceClass", "08\n");
    writeAttr(root, "usb1/idVendor", "1d6b\n");
    writeAttr(root, "usb1/idProduct", "0002\n");

    QString error;
    const QVariantList devices = UsbForwardingBackend::parseSysfsDevices(root.path(), &error);

    int failures = 0;
    if (!error.isEmpty()) {
        qCritical() << "unexpected error:" << error;
        ++failures;
    }
    if (devices.size() != 3) {
        qCritical() << "expected 3 devices, got" << devices.size();
        ++failures;
    }

    const QVariantMap full = findDevice(devices, "1-1");
    if (full.isEmpty() || full.value("description").toString() != "Spike USB Stick" ||
        full.value("vidPid").toString() != "076b:6666" ||
        full.value("instanceId").toString() != "spike0001" || !full.value("isConnected").toBool() ||
        !full.value("isSupported").toBool() || full.value("isAttached").toBool() ||
        full.value("isForced").toBool() || !full.value("persistedGuid").toString().isEmpty() ||
        full.contains("isOccupied")) {
        qCritical() << "1-1 fields mismatch:" << full;
        ++failures;
    }
#ifdef Q_OS_UNIX
    if (!full.value("isBound").toBool()) {
        qCritical() << "1-1 should be bound (driver symlink)";
        ++failures;
    }
#endif

    const QVariantMap minimal = findDevice(devices, "1-2");
    if (minimal.isEmpty() || minimal.value("description").toString() != "054c:0ce6" ||
        minimal.value("vidPid").toString() != "054c:0ce6" || minimal.value("isBound").toBool()) {
        qCritical() << "1-2 fields mismatch:" << minimal;
        ++failures;
    }

    // 不存在的目录 → error 非空 + 空表。
    QString missingError;
    const QVariantList missing =
        UsbForwardingBackend::parseSysfsDevices(root.filePath("does-not-exist"), &missingError);
    if (missingError.isEmpty() || !missing.isEmpty()) {
        qCritical() << "missing dir should error:" << missingError << missing.size();
        ++failures;
    }

    // 身份替换标记：绑定快照与活体身份不符 → isReplaced。
    // Windows 建不了 driver symlink，3-1 在拷贝里显式置 isBound，
    // 保证标记逻辑的平台无关性断言在 Windows CI 同样成立。
    QVariantList replaced = devices;
    for (QVariant &entry : replaced) {
        QVariantMap device = entry.toMap();
        if (device.value(QStringLiteral("busId")).toString() == QStringLiteral("3-1")) {
            device.insert(QStringLiteral("isBound"), true);
            entry = device;
            break;
        }
    }
    QMap<QString, QString> bindings;
    bindings.insert(QStringLiteral("1-1"), QStringLiteral("076b:6666:spike0001"));
    bindings.insert(QStringLiteral("3-1"), QStringLiteral("054c:0ce6:original-serial"));
    UsbForwardingBackend::markReplacedDevices(replaced, bindings);
    const QVariantMap sameIdentity = findDevice(replaced, "1-1");
    if (sameIdentity.value("isReplaced").toBool()) {
        qCritical() << "1-1 identity matches snapshot, should not be replaced";
        ++failures;
    }
    const QVariantMap swappedIdentity = findDevice(replaced, "3-1");
    if (!swappedIdentity.value("isReplaced").toBool()) {
        qCritical() << "3-1 identity differs from snapshot, should be replaced";
        ++failures;
    }

    // 无快照的已共享设备：无从判断，不标。
    QVariantList onlyOne;
    onlyOne.append(findDevice(devices, "1-1"));
    QMap<QString, QString> noSnapshot;
    UsbForwardingBackend::markReplacedDevices(onlyOne, noSnapshot);
    if (findDevice(onlyOne, "1-1").contains("isReplaced")) {
        qCritical() << "no snapshot should not mark replaced";
        ++failures;
    }

    // 身份构造：空白剥离与 helper 对齐；空序列号退化为 vidPid 级
    // （同型号无序列号孪生设备不做区分，见 deviceIdentity 注释）。
    if (UsbForwardingBackend::deviceIdentity(QStringLiteral("054C:0CE6"),
                                             QStringLiteral(" A B \n")) !=
        QStringLiteral("054c:0ce6:AB")) {
        qCritical() << "deviceIdentity should strip whitespace";
        ++failures;
    }
    if (UsbForwardingBackend::deviceIdentity(QStringLiteral("054c:0ce6"), QString()) !=
        QStringLiteral("054c:0ce6:")) {
        qCritical() << "empty serial should degrade to vidPid identity";
        ++failures;
    }

    if (failures != 0) {
        qCritical() << failures << "failure(s)";
        return 1;
    }
    qDebug() << "usb_forwarding_backend_sysfs: all checks passed";
    return 0;
}
