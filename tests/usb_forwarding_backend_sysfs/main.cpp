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
    if (!base.mkpath(QFileInfo(base.filePath(rel)).path())) {
        qFatal("could not create fixture directory");
    }
    QFile file(base.filePath(rel));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(content) != content.size()) {
        qFatal("could not write fixture attribute");
    }
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

    // A regular device beneath a hub remains shareable. Fall back to the
    // manufacturer when the product name is unavailable.
    writeAttr(root, "2-1.3/idVendor", "ABCD\n");
    writeAttr(root, "2-1.3/idProduct", "0001\n");
    writeAttr(root, "2-1.3/bDeviceClass", "00\n");
    writeAttr(root, "2-1.3/manufacturer", "Nested Devices\n");

    // An incomplete sysfs entry is not a USB device and must be skipped.
    writeAttr(root, "4-1/idVendor", "1234\n");

    // A non-topology name must not be passed to the privileged helper even
    // when the entry otherwise has valid device attributes.
    writeAttr(root, "1-3;touch/idVendor", "1234\n");
    writeAttr(root, "1-3;touch/idProduct", "5678\n");
    writeAttr(root, "1-3;touch/bDeviceClass", "00\n");

#ifdef Q_OS_UNIX
    // A device imported through vhci_hcd cannot be exported again because
    // the kernel rejects USB/IP loops.
    writeAttr(root, "platform-vhci/5-1/idVendor", "1234\n");
    writeAttr(root, "platform-vhci/5-1/idProduct", "5678\n");
    writeAttr(root, "platform-vhci/5-1/bDeviceClass", "00\n");
    if (!QFile::link(root.filePath(QStringLiteral("platform-vhci/5-1")),
                     root.filePath(QStringLiteral("5-1")))) {
        qFatal("could not create vhci fixture link");
    }
#endif

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

    const QVariantMap nested = findDevice(devices, "2-1.3");
    if (nested.isEmpty() || nested.value("description").toString() != "Nested Devices" ||
        nested.value("vidPid").toString() != "abcd:0001" || !nested.value("isSupported").toBool()) {
        qCritical() << "nested topology device mismatch:" << nested;
        ++failures;
    }

    const QVariantMap unsafeBusId = findDevice(devices, "1-3;touch");
    if (unsafeBusId.isEmpty() || unsafeBusId.value("isSupported").toBool()) {
        qCritical() << "non-topology busid must not be shareable:" << unsafeBusId;
        ++failures;
    }
    if (!findDevice(devices, "2-1.2").isEmpty() || !findDevice(devices, "1-1:1.0").isEmpty() ||
        !findDevice(devices, "usb1").isEmpty() || !findDevice(devices, "4-1").isEmpty() ||
        !findDevice(devices, "5-1").isEmpty()) {
        qCritical() << "hub, interface, root, incomplete, or vhci devices must be filtered";
        ++failures;
    }

#ifdef Q_OS_LINUX
    // bind() must enforce the privilege boundary itself rather than relying
    // on QML to hide devices that cannot be shared.
    UsbForwardingBackend *backend = UsbForwardingBackend::get();
    int operationCount = 0;
    bool operationSucceeded = true;
    const QMetaObject::Connection operationConnection =
        QObject::connect(backend, &UsbForwardingBackend::operationFinished,
                         [&operationCount, &operationSucceeded](bool success, const QString &) {
                             ++operationCount;
                             operationSucceeded = success;
                         });
    backend->bind(QStringLiteral("1-3;touch"));
    if (operationCount != 1 || operationSucceeded) {
        qCritical() << "invalid busid must be rejected before elevation";
        ++failures;
    }
    backend->bind(QStringLiteral("9-9"));
    if (operationCount != 2 || operationSucceeded) {
        qCritical() << "stale busid must be rejected before elevation";
        ++failures;
    }
    QObject::disconnect(operationConnection);
#endif

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
    const QMap<QString, QString> bindings =
        UsbForwardingBackend::parseBindIdentities("1-1\t076B:6666\t spike 0001 \n"
                                                  "1-2\t9999:0000\tdifferent-device\n"
                                                  "2-1.3\tABCD:0001\t\n"
                                                  "3-1\t054c:0ce6\toriginal-serial\n"
                                                  "usb1\t1d6b:0002\troot-hub\n"
                                                  "1-1:1.0\t076b:6666\tinterface\n"
                                                  "../escape\t1234:5678\tunsafe\n"
                                                  "1-3;touch\t1234:5678\tunsafe\n"
                                                  "malformed\n");
    if (!bindings.contains(QStringLiteral("1-1")) || !bindings.contains(QStringLiteral("1-2")) ||
        !bindings.contains(QStringLiteral("2-1.3")) || !bindings.contains(QStringLiteral("3-1")) ||
        bindings.contains(QStringLiteral("usb1")) || bindings.contains(QStringLiteral("1-1:1.0")) ||
        bindings.contains(QStringLiteral("../escape")) ||
        bindings.contains(QStringLiteral("1-3;touch"))) {
        qCritical() << "binding parser accepted malformed or unsafe rows:" << bindings;
        ++failures;
    }
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
    const QVariantMap unboundMismatch = findDevice(replaced, "1-2");
    if (unboundMismatch.contains("isReplaced")) {
        qCritical() << "unbound device must not be marked replaced:" << unboundMismatch;
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
