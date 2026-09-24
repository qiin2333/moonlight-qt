#include "usbforwardingbackend.h"
#include "usbforwardingenvironment.h"

#include "usbforwardinglocalserver.h"

#ifdef Q_OS_DARWIN
// 仅 macOS 的绑定偏好持久化用到；不无条件引入，让 tests/ 无需 Qt Qml。
#include "settings/streamingpreferences.h"
#endif

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QTemporaryDir>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

#ifndef Q_OS_DARWIN
// 从 Windows 实例 ID（USB\VID_054C&PID_0CE6\...）解析出 "054c:0ce6"。
QString vidPidFromInstanceId(const QString &instanceId)
{
    static const QRegularExpression vidRe(QStringLiteral("VID_([0-9A-Fa-f]{4})"));
    static const QRegularExpression pidRe(QStringLiteral("PID_([0-9A-Fa-f]{4})"));
    const QRegularExpressionMatch vidMatch = vidRe.match(instanceId);
    const QRegularExpressionMatch pidMatch = pidRe.match(instanceId);
    if (!vidMatch.hasMatch() || !pidMatch.hasMatch()) {
        return QString();
    }
    return vidMatch.captured(1).toLower() + QLatin1Char(':') + pidMatch.captured(1).toLower();
}

// "1-2" 这类真实 busid 才可绑定；"IncompatibleHub" 与空串都不可。
bool isRealBusId(const QString &busId)
{
    static const QRegularExpression realRe(QStringLiteral("^[1-9][0-9]*-[1-9][0-9]*$"));
    return realRe.match(busId).hasMatch();
}
#endif

// macOS helper 的 busid 是 libusb 拓扑路径："1-2"，经 hub 时 "1-2.3"。
// 必须与 usbipdcpp 的 find_by_busid 生成算法一致（bus + "-" + 端口链 "." 连接）。
// Linux sysfs 的 busid 同为拓扑路径（"1-1"、"2-1.2"），共用同一校验。
bool isTopologyBusId(const QString &busId)
{
    static const QRegularExpression topologyRe(QStringLiteral("^[1-9][0-9]*-[0-9]+(\\.[0-9]+)*$"));
    return topologyRe.match(busId).hasMatch();
}

#ifdef Q_OS_LINUX
// 提权面收口成一个固定 helper：pkexec 只执行这两个固定路径，busid 在
// helper 里再做一次字符集校验（数字、点、横线），不拼任何用户输入进
// shell。首次共享时由 app 用一次通用 pkexec 安装 helper 本体，helper 的
// install 动作随后写 policy + udev 规则（拔出自动清理钩子），之后 bind/
// unbind 都走 app 自己的 auth_admin_keep action（一次授权管几分钟）。
constexpr auto kLinuxHelperPath = "/usr/lib/moonlight-qt/moonlight-usb-helper";
constexpr auto kLinuxPolicyPath =
    "/usr/share/polkit-1/actions/org.moonlight.qt.usbforwarding.policy";
constexpr auto kLinuxUdevRulePath = "/usr/lib/udev/rules.d/90-moonlight-usb-forwarding.rules";
constexpr auto kLinuxBindingsPath = "/var/lib/moonlight-qt/bindings";

constexpr auto kLinuxHelperScript = R"HELPER(#!/bin/sh
# Moonlight USB forwarding privileged helper (Linux usbip-host backend).
# Installed and invoked only via pkexec; validates its own arguments.
# NOTE: keep this script ASCII-only. MSVC rejects some non-ASCII punctuation
# (em dash, CJK ideographs) inside raw string literals (error C3872), so the
# prose lives in the C++ comment above instead.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
set -u
ACTION=${1:-}
BUSID=${2:-}
STATE_DIR=/var/lib/moonlight-qt
STATE_FILE=$STATE_DIR/bindings
POLICY_FILE=/usr/share/polkit-1/actions/org.moonlight.qt.usbforwarding.policy
UDEV_RULE_FILE=/usr/lib/udev/rules.d/90-moonlight-usb-forwarding.rules

if [ "$ACTION" = "install" ]; then
    # Both files are written by the helper itself so the elevated surface
    # stays a single fixed executable; contents are compiled into it.
    # Every step is checked: a half-installed state (file present but rule
    # never activated) is worse than a failed install the user can retry.
    write_policy() {
        cat > "$POLICY_FILE" <<'POLICY'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC
 "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
  <action id="org.moonlight.qt.usbforwarding.run">
    <description>Run the Moonlight USB forwarding helper</description>
    <message>Authentication is required to share or release a USB device with Moonlight</message>
    <defaults>
      <allow_any>no</allow_any>
      <allow_inactive>no</allow_inactive>
      <allow_active>auth_admin_keep</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">/usr/lib/moonlight-qt/moonlight-usb-helper</annotate>
  </action>
</policyconfig>
POLICY
    }
    write_rule() {
        cat > "$UDEV_RULE_FILE" <<'RULE'
# Moonlight USB forwarding: on unplug of a shared device, clear the
# lingering usbip-host match_busid entry for that port. The kernel only
# allows match_busid del while the device is present, so after an unplug
# the entry can only be dropped via a module reload; modprobe -r refuses
# safely while another shared device is still bound.
ACTION=="remove", SUBSYSTEM=="usb", RUN+="/usr/lib/moonlight-qt/moonlight-usb-helper auto-release $kernel"
RULE
    }
    if ! write_policy || ! chmod 644 "$POLICY_FILE" ||
       ! write_rule || ! chmod 644 "$UDEV_RULE_FILE"; then
        echo "config write failed" >&2
        exit 7
    fi
    # A failed reload leaves the rule on disk but invisible to udev events;
    # surface it so the caller can ask for a retry instead of silently
    # running without unplug cleanup.
    udevadm control --reload 2>/dev/null || { echo "udev reload failed" >&2; exit 7; }
    echo "installed"
    exit 0
fi

case "$ACTION" in bind|unbind|auto-release) ;; *) echo "unsupported action" >&2; exit 2;; esac
case "$BUSID" in ''|*[!0-9.-]*) echo "invalid busid" >&2; exit 2;; esac
command -v usbip >/dev/null 2>&1 || { echo "usbip tool not found" >&2; exit 3; }
if [ ! -d /sys/module/usbip_host ]; then
    modprobe usbip_host 2>/dev/null || { echo "cannot load usbip_host module" >&2; exit 4; }
fi
# identity() snapshots are vidPid + serial: stable across replug. usbip's
# match_busid matches ports, not devices, so a different device plugged into
# a shared port would be silently claimed by usbip-host. The snapshot lets
# the client detect that substitution and refuse to forward the wrong device.
# Serial-less devices degrade to a vidPid-only identity: cross-model swaps
# are still caught, same-model twins are indistinguishable (there is no
# replug-stable per-device identity for such devices on Linux).
read_identity_attrs() {
    VID=$(tr -d ' \t\n\r' < "/sys/bus/usb/devices/$BUSID/idVendor" 2>/dev/null | tr 'A-F' 'a-f')
    PID=$(tr -d ' \t\n\r' < "/sys/bus/usb/devices/$BUSID/idProduct" 2>/dev/null | tr 'A-F' 'a-f')
    SERIAL=$(tr -d ' \t\n\r' < "/sys/bus/usb/devices/$BUSID/serial" 2>/dev/null)
    [ -n "$VID" ] && [ -n "$PID" ]
}
forget() {
    mkdir -p "$STATE_DIR"
    [ -f "$STATE_FILE" ] || return 0
    awk -F'\t' -v b="$1" '$1 != b' "$STATE_FILE" > "$STATE_FILE.new" &&
        mv "$STATE_FILE.new" "$STATE_FILE"
}
# Sweep snapshot entries whose busid is gone from sysfs. After a
# successful module reload every lingering match is already cleared, so
# these entries no longer guard anything.
sweep_stale() {
    [ -f "$STATE_FILE" ] || return 0
    while IFS="$(printf '\t')" read -r b vidpid serial; do
        [ -n "$b" ] || continue
        [ -d "/sys/bus/usb/devices/$b" ] || forget "$b"
    done < "$STATE_FILE"
}
record() {
    read_identity_attrs || return 1
    forget "$BUSID"
    # Append failure (read-only /var/lib, full disk) must surface: a bound
    # device without a snapshot silently disables replacement detection.
    printf '%s\t%s:%s\t%s\n' "$BUSID" "$VID" "$PID" "$SERIAL" >> "$STATE_FILE" || return 1
    chmod 644 "$STATE_FILE" 2>/dev/null || true
}
if [ "$ACTION" = "bind" ] && ! pgrep -x usbipd >/dev/null 2>&1; then
    usbipd -D >/dev/null 2>&1
    sleep 0.5
fi
if [ "$ACTION" = "bind" ]; then
    # TOCTOU guard: $3 is the identity the user saw when requesting sharing.
    # Re-read sysfs as root and refuse to bind anything else at this port.
    EXPECTED=${3:-}
    if [ -z "$EXPECTED" ] || [ "${#EXPECTED}" -gt 512 ]; then
        echo "missing expected identity" >&2; exit 2
    fi
    case "$EXPECTED" in *[![:print:]]*) echo "invalid identity" >&2; exit 2;; esac
    if ! read_identity_attrs || [ "$VID:$PID:$SERIAL" != "$EXPECTED" ]; then
        echo "device_changed" >&2; exit 5
    fi
    OUT=$(usbip bind -b "$BUSID" 2>&1)
    RC=$?
    echo "$OUT"
    case "$OUT" in *"already bound"*) RC=0;; esac
    if [ $RC -eq 0 ]; then
        if ! record; then
            # Keep the bind (sharing itself is fine and matches what
            # usbipd-win offers), but tell the caller that replacement
            # detection is unavailable for this port.
            echo "snapshot_failed" >&2
            exit 6
        fi
    fi
    exit $RC
fi
if [ "$ACTION" = "auto-release" ]; then
    # udev remove hook: the shared device was unplugged. The usbip-host
    # match_busid entry for this port lingers (the kernel only allows del
    # while the device is present) and would seize a different device
    # replugged into the same port. A module reload clears it; modprobe -r
    # refuses safely while another shared device is still bound. Keep the
    # snapshot when the reload fails; without it replacement detection
    # would skip this port while the lingering match is still live; the
    # sweep retries on later unplug events once unloading becomes possible.
    awk -F'\t' -v b="$BUSID" '$1 == b { found = 1 } END { exit !found }' \
        "$STATE_FILE" 2>/dev/null || exit 0
    if modprobe -r usbip_host 2>/dev/null && modprobe usbip_host 2>/dev/null; then
        forget "$BUSID"
        sweep_stale
    fi
    exit 0
fi
OUT=$(usbip unbind -b "$BUSID" 2>&1)
RC=$?
echo "$OUT"
if [ $RC -ne 0 ]; then
    # Unbind failed: keep the snapshot so replacement detection still guards
    # whatever is still bound at this port.
    exit $RC
fi
echo "$BUSID" > /sys/bus/usb/drivers_probe 2>/dev/null || true
forget "$BUSID"
)HELPER";

#endif // Q_OS_LINUX

} // namespace

UsbForwardingBackend::UsbForwardingBackend(QObject *parent) : QObject(parent) {}

UsbForwardingBackend *UsbForwardingBackend::get()
{
    static UsbForwardingBackend backend;
    return &backend;
}

QVariantList UsbForwardingBackend::parseHelperDevices(const QByteArray &helperJson, QString *error)
{
    if (error) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(helperJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error) {
            *error = tr("Could not parse the USB device list.");
        }
        return {};
    }

    QVariantList devices;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject o = value.toObject();
        const QString busId = o.value(QLatin1String("busId")).toString();
        const QString vidPid = o.value(QLatin1String("vidPid")).toString().toLower();
        const QString product = o.value(QLatin1String("product")).toString();
        const QString manufacturer = o.value(QLatin1String("manufacturer")).toString();
        const bool claimable = o.value(QLatin1String("claimable")).toBool();

        QString description = !product.isEmpty() ? product : manufacturer;
        if (description.isEmpty()) {
            description = vidPid;
        }

        const bool occupied = !claimable;

        QVariantMap device;
        device.insert(QStringLiteral("busId"), busId);
        device.insert(QStringLiteral("description"), description);
        device.insert(QStringLiteral("instanceId"), o.value(QLatin1String("serial")).toString());
        device.insert(QStringLiteral("vidPid"), vidPid);
        // isBound 恒 false，由 refreshFromHelper() 按用户偏好叠加。
        device.insert(QStringLiteral("isBound"), false);
        // 出现在枚举输出里即已连接。
        device.insert(QStringLiteral("isConnected"), true);
        device.insert(QStringLiteral("isAttached"), false);
        device.insert(QStringLiteral("isSupported"), isTopologyBusId(busId) && claimable);
        device.insert(QStringLiteral("isForced"), false);
        device.insert(QStringLiteral("persistedGuid"), QString());
        device.insert(QStringLiteral("isOccupied"), occupied);
        devices.append(device);
    }

    return devices;
}

void UsbForwardingBackend::setBusy(bool busy)
{
    if (m_Busy == busy) {
        return;
    }
    m_Busy = busy;
    emit busyChanged();
}

void UsbForwardingBackend::setError(const QString &error)
{
    if (m_Error == error) {
        return;
    }
    m_Error = error;
    emit errorChanged();
}

void UsbForwardingBackend::refresh()
{
    if (m_Busy) {
        return;
    }
#ifdef Q_OS_DARWIN
    refreshFromHelper();
#elif defined(Q_OS_LINUX)
    // sysfs 读取是纯文件访问，同步完成即可，无需 busy 状态机。
    if (QStandardPaths::findExecutable(QStringLiteral("usbip")).isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("The usbip tool is not installed. Install the USB/IP package of your "
                    "distribution (usbip-utils or linux-tools)."));
        return;
    }
    setError(QString());
    QString parseError;
    QVariantList devices = parseSysfsDevices(QStringLiteral("/sys/bus/usb/devices"), &parseError);
    if (!parseError.isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(parseError);
        return;
    }
    // 绑定快照比对：共享后端口上被换成别的设备时标记 isReplaced，
    // 悬浮菜单不再转发它，绑定对话框给出重共享提示。
    QFile bindingsFile(QString::fromLatin1(kLinuxBindingsPath));
    const QMap<QString, QString> bindings = parseBindIdentities(
        bindingsFile.open(QIODevice::ReadOnly) ? bindingsFile.readAll() : QByteArray());
    markReplacedDevices(devices, bindings);
    m_Devices = devices;
    emit devicesChanged();
#else
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
    if (exe.isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("usbipd-win is not installed."));
        return;
    }
    setBusy(true);
    setError(QString());

    QProcess *probe = new QProcess(this);
    /* FailedToStart emits errorOccurred but never finished; without this the
     * busy flag would stick and the device list would go stale. */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart) {
                    return;
                }
                probe->deleteLater();
                setBusy(false);
                m_Devices.clear();
                emit devicesChanged();
                setError(tr("usbipd could not be started. Requires usbipd-win 2.2.0+."));
            });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        setBusy(false);

        if (exitCode != 0) {
            const QString detail =
                QString::fromLocal8Bit(probe->readAllStandardError()).simplified();
            m_Devices.clear();
            emit devicesChanged();
            setError(
                detail.isEmpty()
                    ? tr("usbipd state failed (exit %1). Requires usbipd-win 2.2.0+.").arg(exitCode)
                    : detail);
            return;
        }

        const QByteArray output = probe->readAllStandardOutput();
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(output, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            m_Devices.clear();
            emit devicesChanged();
            setError(tr("Could not parse usbipd state output."));
            return;
        }

        QVariantList devices;
        const QJsonArray array = doc.object().value(QLatin1String("Devices")).toArray();
        for (const QJsonValue &value : array) {
            const QJsonObject o = value.toObject();
            const QString busId = o.value(QLatin1String("BusId")).toString();
            const QString clientIp = o.value(QLatin1String("ClientIPAddress")).toString();
            const QString description = o.value(QLatin1String("Description")).toString();
            const QString instanceId = o.value(QLatin1String("InstanceId")).toString();
            const bool isForced = o.value(QLatin1String("IsForced")).toBool();
            const QString persistedGuid = o.value(QLatin1String("PersistedGuid")).toString();

            const bool isBound = !persistedGuid.isEmpty();
            const bool isConnected = !busId.isEmpty();
            const bool isAttached = !clientIp.isEmpty();
            const bool supported = isConnected && isRealBusId(busId);

            QVariantMap device;
            device.insert(QStringLiteral("busId"), busId);
            device.insert(QStringLiteral("description"), description);
            device.insert(QStringLiteral("instanceId"), instanceId);
            device.insert(QStringLiteral("vidPid"), vidPidFromInstanceId(instanceId));
            device.insert(QStringLiteral("isBound"), isBound);
            device.insert(QStringLiteral("isConnected"), isConnected);
            device.insert(QStringLiteral("isAttached"), isAttached);
            device.insert(QStringLiteral("isSupported"), supported);
            device.insert(QStringLiteral("isForced"), isForced);
            device.insert(QStringLiteral("persistedGuid"), persistedGuid);
            devices.append(device);
        }

        m_Devices = devices;
        emit devicesChanged();
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(exe, { QStringLiteral("state") });
#endif
}

#ifdef Q_OS_DARWIN

void UsbForwardingBackend::refreshFromHelper()
{
    const QString helper = UsbForwardingLocalServer::locateHelper();
    if (helper.isEmpty()) {
        m_Devices.clear();
        emit devicesChanged();
        setError(tr("The bundled USB sharing component is missing. Reinstall Moonlight."));
        return;
    }
    setBusy(true);
    setError(QString());

    QProcess *probe = new QProcess(this);
    /* 与 Windows 分支同理：FailedToStart 只发 errorOccurred 不发 finished。 */
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart) {
                    return;
                }
                probe->deleteLater();
                setBusy(false);
                m_Devices.clear();
                emit devicesChanged();
                setError(tr("The USB helper could not be started."));
            });
    connect(probe, &QProcess::finished, this, [this, probe](int exitCode) {
        probe->deleteLater();
        setBusy(false);

        if (exitCode != 0) {
            const QString detail =
                QString::fromLocal8Bit(probe->readAllStandardError()).simplified();
            m_Devices.clear();
            emit devicesChanged();
            setError(detail.isEmpty() ? tr("The USB device list failed (exit %1).").arg(exitCode)
                                      : detail);
            return;
        }

        QString parseError;
        QVariantList devices = parseHelperDevices(probe->readAllStandardOutput(), &parseError);
        if (!parseError.isEmpty()) {
            m_Devices.clear();
            emit devicesChanged();
            setError(parseError);
            return;
        }

        const QStringList bound = StreamingPreferences::get()->usbForwardingBoundDevices();
        for (QVariant &value : devices) {
            QVariantMap device = value.toMap();
            device.insert(QStringLiteral("isBound"),
                          bound.contains(device.value(QStringLiteral("busId")).toString()));
            value = device;
        }

        m_Devices = devices;
        emit devicesChanged();
    });
    QTimer::singleShot(8000, probe, &QProcess::kill);
    probe->start(helper, { QStringLiteral("list"), QStringLiteral("--json") });
}

#endif

QVariantList UsbForwardingBackend::parseSysfsDevices(const QString &sysfsBusPath, QString *error)
{
    if (error) {
        error->clear();
    }
    const QDir busDir(sysfsBusPath);
    if (!busDir.exists()) {
        if (error) {
            *error = tr("USB device information is unavailable (sysfs is not mounted).");
        }
        return {};
    }

    // sysfs 属性读取失败返回空串：idVendor/idProduct 缺失的条目不是设备，
    // 直接跳过；其余属性允许缺失（比如 product 读不出来就用 vidPid 兜底）。
    const auto readAttr = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(file.readAll()).trimmed()
                                              : QString();
    };

    QVariantList devices;
    const QFileInfoList entries =
        busDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString busId = entry.fileName();
        // 接口目录形如 "1-1:1.0"，根 hub 是 "usb1"，都跳过。
        if (busId.contains(QLatin1Char(':')) || busId.startsWith(QLatin1String("usb"))) {
            continue;
        }
        const QString devicePath = entry.absoluteFilePath();
        const QString vendor = readAttr(devicePath + QStringLiteral("/idVendor"));
        const QString product = readAttr(devicePath + QStringLiteral("/idProduct"));
        if (vendor.isEmpty() || product.isEmpty()) {
            continue;
        }
        // bDeviceClass 09 = hub，usbip 不支持共享 hub。
        if (readAttr(devicePath + QStringLiteral("/bDeviceClass")) == QLatin1String("09")) {
            continue;
        }
        // vhci 导入进来的设备（本机也在用 USB/IP 时）再共享会被内核防环
        // 保护拒绝（"bind loop detected"），直接从列表排除。
        if (QFileInfo(devicePath).canonicalFilePath().contains(QLatin1String("vhci"))) {
            continue;
        }

        const QString vidPid = vendor.toLower() + QLatin1Char(':') + product.toLower();
        QString description = readAttr(devicePath + QStringLiteral("/product"));
        if (description.isEmpty()) {
            description = readAttr(devicePath + QStringLiteral("/manufacturer"));
        }
        if (description.isEmpty()) {
            description = vidPid;
        }

        // 绑定状态就是内核状态：driver symlink 指向 usbip-host 即已共享。
        // （重插后内核重新枚举、绑定丢失，refresh 后如实反映。）
        const QString driverTarget = QFile::symLinkTarget(devicePath + QStringLiteral("/driver"));
        const bool isBound = driverTarget.endsWith(QLatin1String("/usbip-host"));

        QVariantMap device;
        device.insert(QStringLiteral("busId"), busId);
        device.insert(QStringLiteral("description"), description);
        device.insert(QStringLiteral("instanceId"),
                      readAttr(devicePath + QStringLiteral("/serial")));
        device.insert(QStringLiteral("vidPid"), vidPid);
        device.insert(QStringLiteral("isBound"), isBound);
        device.insert(QStringLiteral("isConnected"), true);
        device.insert(QStringLiteral("isAttached"), false);
        device.insert(QStringLiteral("isSupported"), isTopologyBusId(busId));
        device.insert(QStringLiteral("isForced"), false);
        device.insert(QStringLiteral("persistedGuid"), QString());
        devices.append(device);
    }

    return devices;
}

QString UsbForwardingBackend::deviceIdentity(const QString &vidPid, const QString &serial)
{
    // 与 helper 的 `tr -d ' \t\n\r'` 对齐：快照与活体两侧同样剥离空白，
    // 否则序列号里含空格的设备刚共享就会被误判成「已更换」。
    // 空序列号退化为 vidPid 级身份：跨型号替换仍能识别；同型号且无序列
    // 号的两台设备在 Linux 上不存在跨重插稳定的设备级标识（Windows 的
    // 实例 ID 也只是端口计数），故不做区分、照常共享。
    static const QRegularExpression whitespaceRe(QStringLiteral("[ \\t\\r\\n]"));
    QString normalizedSerial = serial;
    normalizedSerial.remove(whitespaceRe);
    return vidPid.toLower() + QLatin1Char(':') + normalizedSerial;
}

QMap<QString, QString> UsbForwardingBackend::parseBindIdentities(const QByteArray &bindingsFile)
{
    // 每行：busid \t vidPid \t serial（serial 可能为空，行内不含换行——
    // helper 写入时已剥掉控制字符）。
    QMap<QString, QString> bindings;
    const QList<QByteArray> lines = bindingsFile.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() != 3) {
            continue;
        }
        const QString busId = QString::fromLatin1(fields[0]).trimmed();
        if (!isTopologyBusId(busId)) {
            continue;
        }
        bindings.insert(
            busId, deviceIdentity(QString::fromLatin1(fields[1]), QString::fromLatin1(fields[2])));
    }
    return bindings;
}

void UsbForwardingBackend::markReplacedDevices(QVariantList &devices,
                                               const QMap<QString, QString> &bindings)
{
    for (QVariant &value : devices) {
        QVariantMap device = value.toMap();
        if (!device.value(QStringLiteral("isBound")).toBool()) {
            continue;
        }
        const auto binding = bindings.constFind(device.value(QStringLiteral("busId")).toString());
        if (binding == bindings.constEnd()) {
            // 没有快照（早于本功能共享、或状态文件丢失）：无从判断，不标。
            continue;
        }
        const QString live = deviceIdentity(device.value(QStringLiteral("vidPid")).toString(),
                                            device.value(QStringLiteral("instanceId")).toString());
        if (binding.value() != live) {
            device.insert(QStringLiteral("isReplaced"), true);
            value = device;
        }
    }
}

void UsbForwardingBackend::bind(const QString &busId)
{
    if (busId.isEmpty()) {
        return;
    }
#ifdef Q_OS_DARWIN
    // macOS 上绑定只是记偏好：serve 进程由 Session 在转发时按需拉起，
    // 因此无需提权，立即生效（正在转发的会话不受影响，下次选择时生效）。
    StreamingPreferences *prefs = StreamingPreferences::get();
    QStringList bound = prefs->usbForwardingBoundDevices();
    if (!bound.contains(busId)) {
        bound.append(busId);
        prefs->setUsbForwardingBoundDevices(bound);
        prefs->save();
    }
    emit operationFinished(true, tr("Requested sharing. Refreshing device list…"));
    QTimer::singleShot(300, this, [this] { refresh(); });
#elif defined(Q_OS_LINUX)
    if (!isTopologyBusId(busId)) {
        emit operationFinished(false, tr("This USB device cannot be shared."));
        return;
    }
    const QString expectedIdentity = expectedIdentityFor(busId);
    if (expectedIdentity.isEmpty()) {
        // 列表过期（枚举后设备被拔/换）：不带身份盲绑正是 TOCTOU 要堵的口。
        emit operationFinished(false,
                               tr("The device list is out of date. Refresh it and try again."));
        return;
    }
    runPrivileged(QStringLiteral("bind"), busId, expectedIdentity);
#else
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
    if (exe.isEmpty()) {
        emit operationFinished(false, tr("usbipd-win is not installed."));
        return;
    }
#ifdef Q_OS_WIN32
    const QString params = QStringLiteral("bind --busid \"%1\"").arg(busId);
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"runas", reinterpret_cast<const wchar_t *>(exe.utf16()),
                      reinterpret_cast<const wchar_t *>(params.utf16()), nullptr, SW_HIDE);
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
#else
    const bool ok = false;
#endif
    if (ok) {
        emit operationFinished(true, tr("Requested sharing. Refreshing device list…"));
        QTimer::singleShot(1500, this, [this] { refresh(); });
    } else {
        emit operationFinished(false, tr("The elevation was cancelled or failed."));
    }
#endif
}

void UsbForwardingBackend::unbind(const QString &busId, const QString &persistedGuid)
{
    if (busId.isEmpty() && persistedGuid.isEmpty()) {
        return;
    }
#ifdef Q_OS_DARWIN
    // macOS：从偏好列表移除即可；persistedGuid 是 Windows 概念，忽略。
    StreamingPreferences *prefs = StreamingPreferences::get();
    QStringList bound = prefs->usbForwardingBoundDevices();
    if (bound.removeAll(busId) > 0) {
        prefs->setUsbForwardingBoundDevices(bound);
        prefs->save();
    }
    emit operationFinished(true, tr("Requested stop sharing. Refreshing device list…"));
    QTimer::singleShot(300, this, [this] { refresh(); });
#elif defined(Q_OS_LINUX)
    if (!isTopologyBusId(busId)) {
        return;
    }
    runPrivileged(QStringLiteral("unbind"), busId);
#else
    QStringList args;
    if (!busId.isEmpty() && isRealBusId(busId)) {
        args << QStringLiteral("unbind") << QStringLiteral("--busid") << busId;
    } else if (!persistedGuid.isEmpty()) {
        args << QStringLiteral("unbind") << QStringLiteral("--guid") << persistedGuid;
    } else {
        return;
    }
    const QString exe = UsbForwardingEnvironment::locateUsbipd();
    if (exe.isEmpty()) {
        emit operationFinished(false, tr("usbipd-win is not installed."));
        return;
    }
#ifdef Q_OS_WIN32
    QString params;
    for (const QString &arg : args) {
        params += QLatin1Char('"') + arg + QLatin1String("\" ");
    }
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"runas", reinterpret_cast<const wchar_t *>(exe.utf16()),
                      reinterpret_cast<const wchar_t *>(params.utf16()), nullptr, SW_HIDE);
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
#else
    const bool ok = false;
#endif
    if (ok) {
        emit operationFinished(true, tr("Requested stop sharing. Refreshing device list…"));
        QTimer::singleShot(1500, this, [this] { refresh(); });
    } else {
        emit operationFinished(false, tr("The elevation was cancelled or failed."));
    }
#endif
}

#ifdef Q_OS_LINUX

void UsbForwardingBackend::runPrivileged(const QString &action, const QString &busId,
                                         const QString &expectedIdentity)
{
    if (m_Busy) {
        return;
    }
    setBusy(true);
    setError(QString());

    const bool installed = QFileInfo::exists(QString::fromLatin1(kLinuxHelperPath)) &&
                           QFileInfo::exists(QString::fromLatin1(kLinuxPolicyPath)) &&
                           QFileInfo::exists(QString::fromLatin1(kLinuxUdevRulePath));
    if (installed) {
        runHelperAction(action, busId, expectedIdentity);
    } else {
        installPrivilegedHelper(action, busId, expectedIdentity);
    }
}

bool UsbForwardingBackend::installPrivilegedHelper(const QString &action, const QString &busId,
                                                   const QString &expectedIdentity)
{
    // 临时目录活到安装进程把它消费掉为止：finished 回调里显式释放。
    QTemporaryDir *temp = new QTemporaryDir();
    if (!temp->isValid()) {
        delete temp;
        setBusy(false);
        emit operationFinished(false, tr("Could not create a temporary file for the USB helper."));
        return false;
    }
    const QString helperTmp = temp->filePath(QStringLiteral("moonlight-usb-helper"));
    {
        QFile helperFile(helperTmp);
        if (!helperFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            helperFile.write(kLinuxHelperScript) < 0) {
            delete temp;
            setBusy(false);
            emit operationFinished(false,
                                   tr("Could not create a temporary file for the USB helper."));
            return false;
        }
        helperFile.setPermissions(QFile::ExeOwner | QFile::ReadOwner);
    }

    // 两步提权安装：先把 helper 本体放到位，再以 helper 的 install 动作
    // 写 policy + udev 规则（内容由 helper 自带）。两步都是 pkexec 直接
    // exec 固定程序、argv 直传不经 shell——路径再特殊也不可能注入
    // （CodeRabbit #242, CWE-78）。第一步走 pkexec 默认 admin action
    // （首次输一次密码）；之后 bind/unbind 走 app 自己的
    // auth_admin_keep action。
    const QString installTool = QStandardPaths::findExecutable(QStringLiteral("install"));
    if (installTool.isEmpty()) {
        delete temp;
        setBusy(false);
        emit operationFinished(false, tr("The install tool is not available on this system."));
        return false;
    }

    // 回调在事件循环里晚于本函数触发：栈上的 failInstall 必须按值捕获
    // （this 是单例，永生，引用安全）。
    const auto failInstall = [this, temp](const QString &message) {
        delete temp;
        setBusy(false);
        emit operationFinished(false, message);
    };

    QProcess *configStep = new QProcess(this);
    connect(configStep, &QProcess::errorOccurred, this,
            [this, configStep, failInstall](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart) {
                    return;
                }
                configStep->deleteLater();
                failInstall(
                    tr("pkexec is not available. A polkit authentication agent is required."));
            });
    connect(configStep, &QProcess::finished, this,
            [this, configStep, temp, action, busId, expectedIdentity, failInstall](int exitCode) {
                configStep->deleteLater();
                if (exitCode == 7) {
                    // helper 的 install 动作明确报告：文件已写但 udev 规则
                    // 未能激活。绑定未发生，用户修复后可重试。
                    failInstall(tr("USB sharing setup could not activate the unplug-cleanup "
                                   "rule. It will be retried on the next share attempt."));
                    return;
                }
                if (exitCode != 0 || !QFileInfo::exists(QString::fromLatin1(kLinuxHelperPath))) {
                    failInstall(tr("The elevation was cancelled or failed."));
                    return;
                }
                // 成功路径：failInstall 不会再被调用，这里显式释放。
                delete temp;
                runHelperAction(action, busId, expectedIdentity);
            });

    QProcess *helperStep = new QProcess(this);
    connect(helperStep, &QProcess::errorOccurred, this,
            [this, helperStep, failInstall](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart) {
                    return;
                }
                helperStep->deleteLater();
                failInstall(
                    tr("pkexec is not available. A polkit authentication agent is required."));
            });
    connect(helperStep, &QProcess::finished, this,
            [this, helperStep, configStep, failInstall](int exitCode) {
                helperStep->deleteLater();
                if (exitCode != 0) {
                    configStep->deleteLater();
                    failInstall(tr("The elevation was cancelled or failed."));
                    return;
                }
                configStep->start(QStringLiteral("pkexec"), { QString::fromLatin1(kLinuxHelperPath),
                                                              QStringLiteral("install") });
            });
    helperStep->start(QStringLiteral("pkexec"),
                      { installTool, QStringLiteral("-D"), QStringLiteral("-m"),
                        QStringLiteral("755"), helperTmp, QString::fromLatin1(kLinuxHelperPath) });
    return true;
}

void UsbForwardingBackend::runHelperAction(const QString &action, const QString &busId,
                                           const QString &expectedIdentity)
{
    QProcess *proc = new QProcess(this);
    connect(
        proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart) {
                return;
            }
            proc->deleteLater();
            setBusy(false);
            emit operationFinished(
                false, tr("pkexec is not available. A polkit authentication agent is required."));
        });
    connect(proc, &QProcess::finished, this, [this, proc, action](int exitCode) {
        setBusy(false);
        const QString stdoutText = QString::fromLocal8Bit(proc->readAllStandardOutput());
        const QString stderrText = QString::fromLocal8Bit(proc->readAllStandardError());
        proc->deleteLater();
        const QString combined = stdoutText + QLatin1Char('\n') + stderrText;
        if (exitCode == 0) {
            emit operationFinished(true, action == QLatin1String("bind")
                                             ? tr("Device shared. Refreshing device list…")
                                             : tr("Sharing stopped. Refreshing device list…"));
            QTimer::singleShot(500, this, [this] { refresh(); });
            return;
        }
        // 快照写不进（/var/lib 只读、磁盘满等）：绑定本身成功保留
        // （与 usbipd-win 等位），但替换检测对该端口不可用，如实告知。
        if (exitCode == 6 || combined.contains(QLatin1String("snapshot_failed"))) {
            emit operationFinished(true,
                                   tr("Device shared, but its identity could not be recorded. "
                                      "Replacement detection is unavailable for this device."));
            QTimer::singleShot(500, this, [this] { refresh(); });
            return;
        }
        // 授权等待期间端口上的设备变了：helper 拒绝绑定（TOCTOU 防护）。
        if (exitCode == 5 || combined.contains(QLatin1String("device_changed"))) {
            emit operationFinished(false,
                                   tr("The USB device changed while the authorization was pending. "
                                      "Refresh the device list and share again."));
            QTimer::singleShot(500, this, [this] { refresh(); });
            return;
        }
        // bind 一个已共享的设备是幂等成功（内核态已是目标状态）。
        if (action == QLatin1String("bind") && combined.contains(QLatin1String("already bound"))) {
            emit operationFinished(true, tr("Device is already shared."));
            QTimer::singleShot(500, this, [this] { refresh(); });
            return;
        }
        // pkexec 在用户取消认证时以 126 退出；agent 断开时报 "disconnected"。
        if (exitCode == 126 || combined.contains(QLatin1String("disconnected")) ||
            combined.contains(QLatin1String("Not authorized"))) {
            emit operationFinished(false, tr("The elevation was cancelled or failed."));
            return;
        }
        const QString detail = stderrText.section(QLatin1Char('\n'), 0, 0).simplified();
        emit operationFinished(false,
                               detail.isEmpty()
                                   ? tr("The USB sharing operation failed (exit %1).").arg(exitCode)
                                   : detail);
    });
    QStringList arguments{ QString::fromLatin1(kLinuxHelperPath), action, busId };
    if (action == QLatin1String("bind")) {
        arguments << expectedIdentity;
    }
    proc->start(QStringLiteral("pkexec"), arguments);
}

QString UsbForwardingBackend::expectedIdentityFor(const QString &busId) const
{
    for (const QVariant &entry : m_Devices) {
        const QVariantMap device = entry.toMap();
        if (device.value(QStringLiteral("busId")).toString() == busId) {
            return deviceIdentity(device.value(QStringLiteral("vidPid")).toString(),
                                  device.value(QStringLiteral("instanceId")).toString());
        }
    }
    return QString();
}

#endif // Q_OS_LINUX
