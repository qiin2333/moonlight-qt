#include "nativeusb.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

namespace NativeUsb {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t stopping = 0;
void stopSignal(int)
{
    stopping = 1;
}

struct Fd
{
    int value;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd()
    {
        if (value >= 0)
            close(value);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

std::string readText(const fs::path& path)
{
    std::ifstream file(path);
    std::string value;
    std::getline(file, value);
    if (value.size() > 4096)
        throw std::runtime_error("invalid_sysfs_attribute");
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r'))
        value.pop_back();
    return value;
}

unsigned number(const fs::path& path, int base = 10, bool optional = false)
{
    const auto value = readText(path);
    if (optional && value.empty())
        return 0;
    try {
        size_t end;
        const auto result = std::stoul(value, &end, base);
        if (end == value.size() && result <= UINT32_MAX)
            return static_cast<unsigned>(result);
    } catch (const std::exception&) {
    }
    throw std::runtime_error("device_not_found");
}

void writeText(const fs::path& path, const std::string& value)
{
    Fd fd(open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.value < 0 ||
        write(fd.value, value.data(), value.size()) != static_cast<ssize_t>(value.size())) {
        throw std::runtime_error("sysfs_write_failed");
    }
}

std::string jsonString(const std::string& input)
{
    std::string output = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') {
            output += '\\';
            output += c;
        } else if (c < 32) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            output += escaped;
        } else
            output += c;
    }
    return output + '"';
}

bool cancelled(int controlFd)
{
    pollfd control{ controlFd, POLLIN, 0 };
    return stopping || (controlFd >= 0 && poll(&control, 1, 0) > 0);
}

// Only distro executables and fixed arguments are accepted. No app-supplied
// command, PATH, module options, or dynamic-loader environment reaches root.
void runUsbip(const std::vector<std::string>& arguments)
{
    const char* executable = nullptr;
    for (const char* path : { "/usr/sbin/usbip", "/sbin/usbip", "/usr/bin/usbip", "/bin/usbip" }) {
        if (access(path, X_OK) == 0) {
            executable = path;
            break;
        }
    }
    if (!executable)
        throw std::runtime_error("usbip_not_found");
    std::vector<char*> args{ const_cast<char*>(executable) };
    for (const auto& argument : arguments)
        args.push_back(const_cast<char*>(argument.c_str()));
    args.push_back(nullptr);
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child < 0)
        throw std::runtime_error("usbip_failed");
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(127);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        char path[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
        char locale[] = "LC_ALL=C";
        char* environment[] = { path, locale, nullptr };
        execve(executable, args.data(), environment);
        _exit(127);
    }
    int status = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            break;
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("usbip_failed");
        if (Clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("usbip_timeout");
        }
        poll(nullptr, 0, 50);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("usbip_failed");
}

void loadModule()
{
    if (fs::exists("/sys/bus/usb/drivers/usbip-host"))
        return;
    const char* executable = nullptr;
    for (const char* candidate : { "/usr/sbin/modprobe", "/sbin/modprobe" }) {
        if (access(candidate, X_OK) == 0) {
            executable = candidate;
            break;
        }
    }
    if (!executable)
        throw std::runtime_error("module_unavailable");
    const pid_t child = fork();
    if (child < 0)
        throw std::runtime_error("module_load_failed");
    if (child == 0) {
        dup2(STDERR_FILENO, STDOUT_FILENO);
        // Do not inherit app-controlled module options or loader variables.
        char path[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
        char* environment[] = { path, nullptr };
        char* args[] = { const_cast<char*>(executable), const_cast<char*>("usbip-host"), nullptr };
        execve(executable, args, environment);
        _exit(127);
    }
    int status = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (cancelled(STDIN_FILENO) || Clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("module_load_failed");
        }
        poll(nullptr, 0, 50);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !fs::exists("/sys/bus/usb/drivers/usbip-host")) {
        throw std::runtime_error("module_unavailable");
    }
}

void runForwarding(SysfsDevice& access, Binding& binding, int controlFd)
{
    if (cancelled(controlFd))
        return;
    // usbipd is a distro-managed service. Verify its protocol before taking
    // the device; this helper never accepts or parses USB/IP network traffic.
    runUsbip({ "list", "--remote=127.0.0.1" });
    if (cancelled(controlFd))
        return;
    const auto identity = access.current().identity();
    binding.bind();
    if (cancelled(controlFd))
        return;
    if (access.current().identity() != identity)
        throw std::runtime_error("device_changed");
    std::puts("READY 3240");
    std::fflush(stdout);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    bool imported = false;
    while (!cancelled(controlFd)) {
        const auto device = access.current();
        if (device.identity() != identity)
            throw std::runtime_error("device_changed");
        if (device.attached)
            imported = true;
        else if (imported)
            return;
        else if (Clock::now() >= deadline)
            throw std::runtime_error("import_timeout");
        poll(nullptr, 0, 100);
    }
}
} // namespace

bool validBusId(const std::string& value)
{
    if (value.empty() || value.size() > 31)
        return false;
    bool dash = false, digit = false;
    for (char c : value) {
        if (c >= '0' && c <= '9') {
            digit = true;
            continue;
        }
        if (!digit || (c != '-' && c != '.') || (c == '-' && dash) || (c == '.' && !dash))
            return false;
        if (c == '-')
            dash = true;
        digit = false;
    }
    return dash && digit && value.front() != '0';
}

std::string Device::vidPid() const
{
    char value[10];
    std::snprintf(value, sizeof(value), "%04x:%04x", vendor, productId);
    return value;
}
std::string Device::identity() const
{
    return busId + ":" + std::to_string(bus) + ":" + std::to_string(number) + ":" + vidPid() + ":" +
           std::to_string(inode);
}
std::string Device::registrationKey() const
{
    std::string encoded;
    for (unsigned char c : serial) {
        const char* digits = "0123456789abcdef";
        encoded += digits[c >> 4];
        encoded += digits[c & 15];
    }
    return "linux:" + busId + ":" + vidPid() + ":" + encoded;
}
bool Device::supported() const
{
    return validBusId(busId) && deviceClass != 9 && path.find("/vhci_hcd") == std::string::npos &&
           (driver.empty() || driver == "usb" || driver == "usbip-host");
}

Device readDevice(const std::string& busId, const std::string& root, bool includeInterfaces)
{
    if (!validBusId(busId))
        throw std::runtime_error("invalid_busid");
    const fs::path path = fs::path(root) / busId;
    try {
        struct stat info{};
        if (stat(path.c_str(), &info) != 0)
            throw std::runtime_error("device_not_found");
        // A reset temporarily sets devnum to zero. The configuration read
        // waits for the kernel device lock, so the address can be restored by
        // the time we finish this snapshot. Retry that snapshot, but retain
        // the original inode across attempts so a replacement is never accepted.
        constexpr int maxSnapshotAttempts = 3;
        for (int attempt = 0; attempt < maxSnapshotAttempts; ++attempt) {
            Device d;
            d.busId = busId;
            d.path = fs::canonical(path).string();
            d.inode = info.st_ino;
            d.bus = number(path / "busnum");
            d.number = number(path / "devnum");
            d.vendor = number(path / "idVendor", 16);
            d.productId = number(path / "idProduct", 16);
            d.revision = number(path / "bcdDevice", 16);
            d.deviceClass = number(path / "bDeviceClass", 16);
            d.subClass = number(path / "bDeviceSubClass", 16);
            d.protocol = number(path / "bDeviceProtocol", 16);
            d.configuration = number(path / "bConfigurationValue", 10, true);
            d.configurations = number(path / "bNumConfigurations");
            const auto speed = readText(path / "speed");
            d.speed = speed == "1.5"                           ? 1
                      : speed == "12"                          ? 2
                      : speed == "480"                         ? 3
                      : speed == "5000"                        ? 5
                      : (speed == "10000" || speed == "20000") ? 6
                                                               : 0;
            d.product = readText(path / "product");
            d.manufacturer = readText(path / "manufacturer");
            d.serial = readText(path / "serial");
            std::error_code ec;
            const auto driver = fs::read_symlink(path / "driver", ec);
            if (!ec) {
                d.driver = driver.filename().string();
            } else if (ec != std::errc::no_such_file_or_directory) {
                throw std::runtime_error("driver_status_unavailable");
            }
            d.attached = readText(path / "usbip_status") == "2";
            // Enumerate actual interface directories: bInterfaceNumber need not be
            // contiguous, and usbip-host can initially leave the device unconfigured.
            std::vector<fs::path> interfaces;
            if (includeInterfaces) {
                for (const auto& entry : fs::directory_iterator(path)) {
                    if (entry.path().filename().string().find(busId + ":") == 0) {
                        interfaces.push_back(entry.path());
                    }
                }
            }
            std::sort(interfaces.begin(), interfaces.end());
            for (const auto& intf : interfaces) {
                d.interfaces.push_back(
                    { static_cast<uint8_t>(number(intf / "bInterfaceClass", 16)),
                      static_cast<uint8_t>(number(intf / "bInterfaceSubClass", 16)),
                      static_cast<uint8_t>(number(intf / "bInterfaceProtocol", 16)), 0 });
            }
            if (d.interfaces.size() > 255) {
                throw std::runtime_error("invalid_interfaces");
            }
            struct stat after{};
            if (stat(path.c_str(), &after) != 0 || after.st_ino != info.st_ino) {
                throw std::runtime_error("device_changed");
            }
            if (d.number != 0 && number(path / "devnum") == d.number) {
                return d;
            }
        }
        throw std::runtime_error("device_changed");
    } catch (...) {
        struct stat remaining{};
        if (stat(path.c_str(), &remaining) != 0 && (errno == ENOENT || errno == ENODEV))
            throw DeviceGone();
        throw;
    }
}

std::string deviceListJson(const std::string& root)
{
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (validBusId(entry.path().filename().string()))
            ids.push_back(entry.path().filename().string());
    }
    std::sort(ids.begin(), ids.end());
    std::string result = "[";
    for (const auto& id : ids) {
        try {
            const auto d = readDevice(id, root);
            if (d.deviceClass == 9)
                continue;
            if (result.size() > 1)
                result += ',';
            result +=
                "{\"busId\":" + jsonString(d.busId) + ",\"vidPid\":" + jsonString(d.vidPid()) +
                ",\"product\":" + jsonString(d.product) +
                ",\"manufacturer\":" + jsonString(d.manufacturer) +
                ",\"serial\":" + jsonString(d.serial) +
                ",\"identity\":" + jsonString(d.identity()) +
                ",\"registrationKey\":" + jsonString(d.registrationKey()) +
                ",\"claimable\":" + (d.supported() && d.driver != "usbip-host" ? "true" : "false") +
                ",\"attached\":" + (d.attached ? "true" : "false") + "}";
        } catch (const std::exception&) { /* A disappearing device is normal during enumeration. */
        }
    }
    return result + ']';
}

Binding::Binding(DeviceAccess& access, Device original, BindingProgress& progress)
    : m_Access(access), m_Original(std::move(original)), m_Progress(progress)
{
}

void Binding::requireSameDevice() const
{
    if (m_Access.current().identity() != m_Original.identity())
        throw std::runtime_error("device_changed");
}

void Binding::changeDriver(const std::string& driver, const std::string& attribute)
{
    requireSameDevice();
    try {
        m_Access.writeDriver(driver, attribute, m_Original.busId);
    } catch (...) {
        // A replacement may have been automatically captured by our match,
        // even when the explicit bind failed because it was already bound.
        requireSameDevice();
        throw;
    }
    // A replacement's final driver does not prove which device this write
    // affected. Abort without guessing its previous non-exporter driver.
    requireSameDevice();
}

void Binding::disableAutomaticBinding()
{
    requireSameDevice();
    m_Access.writeDriver("usbip-host", "match_busid", "del " + m_Original.busId);
    try {
        requireSameDevice();
    } catch (const DeviceGone&) {
        // The completed removal still needs cleanup after unplug. Keep the
        // original device pin and this progress for the supervisor's retry.
        m_Progress.automaticBindingDisabled = 1;
        throw;
    }
    // A different identity must throw without marking its unpinned kernel
    // reference safe. Replacement recovery selects and pins its own target.
    m_Progress.automaticBindingDisabled = 1;
}

void Binding::bind()
{
    requireSameDevice();
    if (!m_Original.supported() || m_Access.current().driver == "usbip-host")
        throw std::runtime_error("device_busy");
    m_Progress.detached = 1;
    m_Progress.matched = 1;
    try {
        m_Access.bindDevice();
    } catch (...) {
        requireSameDevice();
        throw;
    }
    requireSameDevice();
    if (m_Access.current().driver != "usbip-host")
        throw std::runtime_error("bind_failed");
    m_Progress.bound = 1;
    // Mark for removal while still bound. Otherwise unplugging leaves an
    // ADDED match which can seize a different device reusing this port.
    disableAutomaticBinding();
}
void Binding::restore(bool recoverReplacements)
{
    if (!m_Progress.detached)
        return;
    bool same = false;
    try {
        const auto current = m_Access.current();
        same = current.identity() == m_Original.identity();
        if (!same && !recoverReplacements)
            throw std::runtime_error("device_changed");
        if (!same && m_Progress.matched && current.driver == "usbip-host") {
            // Recover only an exporter claim made through our active match.
            // The supervisor selects and pins this target after the worker
            // exits, before unbinding. Retain the target and shared progress
            // in this Binding so a failed cleanup can resume after unbind.
            m_Access.pin(current);
            m_Original = current;
            m_Original.driver.clear();
            m_Progress.bound = 1;
            m_Progress.automaticBindingDisabled = 0;
            same = true;
        }
    } catch (const DeviceGone&) {
    }
    if (same && m_Access.current().driver == "usbip-host") {
        m_Progress.bound = 1;
        disableAutomaticBinding();
        // The distro tool performs unbind, match removal and local rebind.
        // Retain our progress and device pin if it fails partway through.
        requireSameDevice();
        try {
            m_Access.unbindDevice();
        } catch (...) {
            requireSameDevice();
            throw;
        }
        requireSameDevice();
    }
    std::exception_ptr matchError;
    try {
        if (m_Progress.matched && m_Access.hasMatch()) {
            m_Access.writeDriver("usbip-host", "match_busid", "del " + m_Original.busId);
            // rebind dereferences the kernel's saved usb_device. Only use it
            // after disabling automatic capture for the verified, pinned
            // object, so another hotplug cannot replace that saved pointer.
            if (m_Progress.bound && m_Progress.automaticBindingDisabled) {
                m_Access.writeDriver("usbip-host", "rebind", m_Original.busId);
            }
        }
    } catch (...) {
        matchError = std::current_exception();
    }
    if (same) {
        requireSameDevice();
        if (!m_Original.driver.empty() && m_Access.current().driver.empty())
            changeDriver("usb", "bind");
        if (m_Original.driver.empty() && m_Access.current().driver == "usb")
            changeDriver("usb", "unbind");
        if (!m_Original.driver.empty() && m_Access.current().driver != m_Original.driver)
            throw std::runtime_error("restore_failed");
    }
    if (matchError)
        std::rethrow_exception(matchError);
    if (m_Progress.matched && m_Access.hasMatch())
        throw std::runtime_error("match_cleanup_failed");
    m_Progress.detached = 0;
    m_Progress.matched = 0;
    m_Progress.bound = 0;
}

SysfsDevice::SysfsDevice(std::string busId) : m_BusId(std::move(busId))
{
    if (!validBusId(m_BusId))
        throw std::runtime_error("invalid_busid");
}
SysfsDevice::~SysfsDevice()
{
    for (int fd : m_DeviceFds)
        close(fd);
    if (m_SysfsFd >= 0)
        close(m_SysfsFd);
}
void SysfsDevice::pin(const Device& device)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", device.bus, device.number);
    Fd fd(open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat info{};
    if (fd.value < 0 || fstat(fd.value, &info) != 0 || !S_ISCHR(info.st_mode) ||
        major(info.st_rdev) != 189 ||
        minor(info.st_rdev) != (device.bus - 1) * 128 + device.number - 1 ||
        current().identity() != device.identity())
        throw std::runtime_error("device_changed");
    if (m_SysfsFd < 0) {
        Fd directory(open((fs::path("/sys/bus/usb/devices") / m_BusId).c_str(),
                          O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (directory.value < 0 || fstat(directory.value, &info) != 0 ||
            info.st_ino != device.inode)
            throw std::runtime_error("device_changed");
        m_SysfsFd = directory.value;
        directory.value = -1;
    }
    m_DeviceFds.push_back(fd.value);
    fd.value = -1;
}
Device SysfsDevice::current() const
{
    return readDevice(m_BusId, "/sys/bus/usb/devices", false);
}
void SysfsDevice::writeDriver(const std::string& driver, const std::string& attribute,
                              const std::string& value)
{
    writeText(fs::path("/sys/bus/usb/drivers") / driver / attribute, value);
}
void SysfsDevice::bindDevice()
{
    runUsbip({ "bind", "--busid=" + m_BusId });
}
void SysfsDevice::unbindDevice()
{
    runUsbip({ "unbind", "--busid=" + m_BusId });
}
bool SysfsDevice::hasMatch() const
{
    std::istringstream entries(readText("/sys/bus/usb/drivers/usbip-host/match_busid"));
    std::string id;
    while (entries >> id)
        if (id == m_BusId)
            return true;
    return false;
}

int serve(const std::string& busId, const std::string& expectedIdentity)
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTERM, stopSignal);
    std::signal(SIGINT, stopSignal);
    if (cancelled(STDIN_FILENO))
        return 0;
    // Adopt distro-tool children if the worker dies. No command may finish
    // binding a device after the supervisor has already restored it.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
        throw std::runtime_error("supervisor_failed");
    loadModule();
    SysfsDevice access(busId);
    const auto original = access.current();
    if (original.identity() != expectedIdentity)
        throw std::runtime_error("device_changed");
    if (!original.supported() || original.driver == "usbip-host")
        throw std::runtime_error("device_busy");
    Fd lock(open(("/run/lock/moonlight-usb-" + busId + ".lock").c_str(),
                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat lockInfo{};
    if (lock.value < 0 || fstat(lock.value, &lockInfo) != 0 || !S_ISREG(lockInfo.st_mode) ||
        lockInfo.st_uid != 0 || flock(lock.value, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("device_busy");
    if (access.hasMatch())
        throw std::runtime_error("device_busy");
    access.pin(original);
    auto* progress =
        static_cast<BindingProgress*>(mmap(nullptr, sizeof(BindingProgress), PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (progress == MAP_FAILED)
        throw std::runtime_error("supervisor_failed");
    new (progress) BindingProgress;
    Binding binding(access, original, *progress);
    int channels[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channels) != 0) {
        munmap(progress, sizeof(*progress));
        throw std::runtime_error("supervisor_failed");
    }
    const pid_t supervisorPid = getpid();
    const pid_t child = fork();
    if (child < 0) {
        close(channels[0]);
        close(channels[1]);
        munmap(progress, sizeof(*progress));
        throw std::runtime_error("supervisor_failed");
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0)
            _exit(1);
        close(channels[0]);
        close(STDIN_FILENO);
        int result = 0;
        try {
            runForwarding(access, binding, channels[1]);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "USB control: %s\n", error.what());
            // If startup failed, the parent receives the exit, rather than a
            // second stdout protocol line after READY.
            result = 1;
        }
        try {
            // Leave replacement cleanup to the live supervisor. Otherwise
            // its worker could die holding the only pin and recovery state
            // for that replacement. An orphan must perform its own cleanup.
            binding.restore(getppid() != supervisorPid);
        } catch (const std::exception&) {
            result = 1;
        }
        close(channels[1]);
        _exit(result);
    }
    setpgid(child, child);
    close(channels[1]);
    Fd control(channels[0]);
    int status = 0;
    const auto workerExited = [child] {
        siginfo_t info{};
        // Keep its PID reserved until the process group has been stopped.
        return waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
               info.si_pid == child;
    };
    while (!cancelled(STDIN_FILENO) && !workerExited()) {
        poll(nullptr, 0, 100);
    }
    if (!workerExited()) {
        // The child also observes EOF if this supervisor dies, and performs
        // its own rollback. If the worker dies, the supervisor restores it.
        shutdown(control.value, SHUT_RDWR);
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!workerExited() && Clock::now() < deadline)
            poll(nullptr, 0, 50);
    }
    kill(-child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    while (waitpid(-1, nullptr, 0) > 0 || errno == EINTR) {
    }
    bool restored = false;
    for (int attempt = 0; attempt < 3 && !restored; ++attempt) {
        try {
            binding.restore();
            restored = true;
        } catch (const std::exception&) {
            poll(nullptr, 0, 100);
        }
    }
    munmap(progress, sizeof(*progress));
    if (!restored)
        std::fputs("USB control: restore_failed; reconnect the USB device\n", stderr);
    return restored && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
} // namespace NativeUsb
