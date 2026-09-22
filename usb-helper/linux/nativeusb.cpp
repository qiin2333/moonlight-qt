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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/mman.h>
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
void stopSignal(int) { stopping = 1; }

struct Fd {
    int value;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

std::string readText(const fs::path& path)
{
    std::ifstream file(path);
    std::string value;
    std::getline(file, value);
    if (value.size() > 4096) throw std::runtime_error("invalid_sysfs_attribute");
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}

unsigned number(const fs::path& path, int base = 10, bool optional = false)
{
    const auto value = readText(path);
    if (optional && value.empty()) return 0;
    try {
        size_t end;
        const auto result = std::stoul(value, &end, base);
        if (end == value.size() && result <= UINT32_MAX) return static_cast<unsigned>(result);
    } catch (const std::exception&) {}
    throw std::runtime_error("device_not_found");
}

void writeText(const fs::path& path, const std::string& value)
{
    Fd fd(open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.value < 0 || write(fd.value, value.data(), value.size()) != static_cast<ssize_t>(value.size())) {
        throw std::runtime_error("sysfs_write_failed");
    }
}

std::string jsonString(const std::string& input)
{
    std::string output = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { output += '\\'; output += c; }
        else if (c < 32) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            output += escaped;
        } else output += c;
    }
    return output + '"';
}

void put16(std::vector<uint8_t>& bytes, unsigned value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}
void put32(std::vector<uint8_t>& bytes, uint32_t value)
{
    put16(bytes, value >> 16); put16(bytes, value);
}
void putString(std::vector<uint8_t>& bytes, const std::string& value, size_t size)
{
    const size_t offset = bytes.size();
    bytes.resize(offset + size, 0);
    std::copy_n(value.begin(), std::min(size - 1, value.size()), bytes.begin() + offset);
}

bool cancelled(int controlFd)
{
    pollfd control {controlFd, POLLIN, 0};
    return stopping || (controlFd >= 0 && poll(&control, 1, 0) > 0);
}

void transfer(int socket, uint8_t* data, size_t size, bool sending, int controlFd)
{
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (size) {
        if (cancelled(controlFd)) throw std::runtime_error("cancelled");
        if (Clock::now() >= deadline) throw std::runtime_error("negotiation_timeout");
        pollfd pfd {socket, static_cast<short>(sending ? POLLOUT : POLLIN), 0};
        const int ready = poll(&pfd, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        const ssize_t count = sending ? send(socket, data, size, MSG_NOSIGNAL | MSG_DONTWAIT)
                                      : recv(socket, data, size, MSG_DONTWAIT);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) throw std::runtime_error("connection_closed");
        data += count; size -= count;
    }
}

void reply(int socket, uint16_t operation, uint32_t status,
           std::vector<uint8_t> payload, int controlFd)
{
    std::vector<uint8_t> bytes;
    put16(bytes, 0x0111); put16(bytes, operation); put32(bytes, status);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    transfer(socket, bytes.data(), bytes.size(), true, controlFd);
}

void loadModule()
{
    if (fs::exists("/sys/bus/usb/drivers/usbip-host")) return;
    const char* executable = nullptr;
    for (const char* candidate : {"/usr/sbin/modprobe", "/sbin/modprobe"}) {
        if (access(candidate, X_OK) == 0) { executable = candidate; break; }
    }
    if (!executable) throw std::runtime_error("module_unavailable");
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("module_load_failed");
    if (child == 0) {
        dup2(STDERR_FILENO, STDOUT_FILENO);
        // Do not inherit app-controlled module options or loader variables.
        char path[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
        char* environment[] = {path, nullptr};
        char* args[] = {const_cast<char*>(executable), const_cast<char*>("usbip-host"), nullptr};
        execve(executable, args, environment);
        _exit(127);
    }
    int status = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (cancelled(STDIN_FILENO) || Clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("module_load_failed");
        }
        poll(nullptr, 0, 50);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            !fs::exists("/sys/bus/usb/drivers/usbip-host")) {
        throw std::runtime_error("module_unavailable");
    }
}

void runExporter(SysfsDevice& access, Binding& binding, int controlFd)
{
    if (cancelled(controlFd)) return;
    binding.bind();
    if (cancelled(controlFd)) return;
    Fd listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener.value < 0 || ::bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            listen(listener.value, 1) < 0) throw std::runtime_error("listen_failed");
    socklen_t size = sizeof(address);
    if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) < 0)
        throw std::runtime_error("listen_failed");
    std::printf("READY %u\n", ntohs(address.sin_port));
    std::fflush(stdout);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!cancelled(controlFd)) {
        if (Clock::now() >= deadline) throw std::runtime_error("import_timeout");
        pollfd pfd {listener.value, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) continue;
        Fd connection(accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC));
        if (connection.value < 0) continue;
        int on = 1;
        setsockopt(connection.value, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        const auto device = readDevice(access.current().busId);
        const bool imported = negotiate(connection.value, device,
                                       [&binding](int fd) { binding.exportSocket(fd); }, controlFd);
        if (!imported) continue;
        // From here only usbip-host reads/writes the socket. Do not consume
        // even a single byte of its URBs in user space.
        while (!cancelled(controlFd)) {
            if (!access.current().attached) return;
            poll(nullptr, 0, 100);
        }
        return;
    }
}
} // namespace

bool validBusId(const std::string& value)
{
    if (value.empty() || value.size() > 31) return false;
    bool dash = false, digit = false;
    for (char c : value) {
        if (c >= '0' && c <= '9') { digit = true; continue; }
        if (!digit || (c != '-' && c != '.') || (c == '-' && dash) || (c == '.' && !dash)) return false;
        if (c == '-') dash = true;
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
    return busId + ":" + std::to_string(bus) + ":" + std::to_string(number) + ":" +
           vidPid() + ":" + std::to_string(inode);
}
std::string Device::registrationKey() const
{
    std::string encoded;
    for (unsigned char c : serial) {
        const char* digits = "0123456789abcdef";
        encoded += digits[c >> 4]; encoded += digits[c & 15];
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
        if (validBusId(entry.path().filename().string())) ids.push_back(entry.path().filename().string());
    }
    std::sort(ids.begin(), ids.end());
    std::string result = "[";
    for (const auto& id : ids) {
        try {
            const auto d = readDevice(id, root);
            if (d.deviceClass == 9) continue;
            if (result.size() > 1) result += ',';
            result += "{\"busId\":" + jsonString(d.busId) + ",\"vidPid\":" + jsonString(d.vidPid()) +
                      ",\"product\":" + jsonString(d.product) + ",\"manufacturer\":" + jsonString(d.manufacturer) +
                      ",\"serial\":" + jsonString(d.serial) + ",\"identity\":" + jsonString(d.identity()) +
                      ",\"registrationKey\":" + jsonString(d.registrationKey()) +
                      ",\"claimable\":" + (d.supported() && d.driver != "usbip-host" ? "true" : "false") +
                      ",\"attached\":" + (d.attached ? "true" : "false") + "}";
        } catch (const std::exception&) { /* A disappearing device is normal during enumeration. */ }
    }
    return result + ']';
}

std::vector<uint8_t> deviceDescriptor(const Device& d)
{
    std::vector<uint8_t> bytes;
    putString(bytes, d.path, 256); putString(bytes, d.busId, 32);
    put32(bytes, d.bus); put32(bytes, d.number); put32(bytes, d.speed);
    put16(bytes, d.vendor); put16(bytes, d.productId); put16(bytes, d.revision);
    bytes.insert(bytes.end(), {d.deviceClass, d.subClass, d.protocol, d.configuration,
                              d.configurations, static_cast<uint8_t>(d.interfaces.size())});
    return bytes;
}

bool negotiate(int socket, const Device& d, const std::function<void(int)>& exportSocket, int controlFd)
{
    std::array<uint8_t, 8> header {};
    transfer(socket, header.data(), header.size(), false, controlFd);
    if (header[0] != 1 || header[1] != 0x11 || header[2] != 0x80 ||
            header[4] || header[5] || header[6] || header[7]) throw std::runtime_error("invalid_request");
    if (header[3] == 5) {
        std::vector<uint8_t> payload;
        put32(payload, 1);
        const auto descriptor = deviceDescriptor(d);
        payload.insert(payload.end(), descriptor.begin(), descriptor.end());
        for (const auto& intf : d.interfaces) payload.insert(payload.end(), intf.begin(), intf.end());
        reply(socket, 5, 0, std::move(payload), controlFd);
        return false;
    }
    if (header[3] != 3) throw std::runtime_error("invalid_operation");
    std::array<uint8_t, 32> bus {};
    transfer(socket, bus.data(), bus.size(), false, controlFd);
    const auto end = std::find(bus.begin(), bus.end(), 0);
    if (end == bus.end() || std::string(bus.begin(), end) != d.busId) {
        reply(socket, 3, 4, {}, controlFd); // ST_NODEV
        return false;
    }
    try {
        // Match Linux usbipd: export before replying, so a success response
        // never promises an import which the kernel has already rejected.
        exportSocket(socket);
    } catch (const std::exception&) {
        reply(socket, 3, 1, {}, controlFd);
        throw;
    }
    reply(socket, 3, 0, deviceDescriptor(d), controlFd);
    return true;
}

Binding::Binding(DeviceAccess& access, Device original, BindingProgress& progress)
    : m_Access(access), m_Original(std::move(original)), m_Progress(progress) {}

void Binding::requireSameDevice() const
{
    if (m_Access.current().identity() != m_Original.identity()) throw std::runtime_error("device_changed");
}
void Binding::bind()
{
    requireSameDevice();
    if (!m_Original.supported() || m_Access.current().driver == "usbip-host")
        throw std::runtime_error("device_busy");
    m_Progress.detached = 1;
    if (!m_Original.driver.empty()) m_Access.writeDriver(m_Original.driver, "unbind", m_Original.busId);
    requireSameDevice();
    m_Progress.matched = 1;
    m_Access.writeDriver("usbip-host", "match_busid", "add " + m_Original.busId);
    requireSameDevice();
    m_Access.writeDriver("usbip-host", "bind", m_Original.busId);
    if (m_Access.current().driver != "usbip-host") throw std::runtime_error("bind_failed");
    m_Progress.bound = 1;
    // Mark for removal while still bound. Otherwise unplugging leaves an
    // ADDED match which can seize a different device reusing this port.
    m_Access.writeDriver("usbip-host", "match_busid", "del " + m_Original.busId);
    m_Progress.automaticBindingDisabled = 1;
}
void Binding::exportSocket(int socket)
{
    requireSameDevice();
    const auto current = m_Access.current();
    if (current.driver != "usbip-host" || current.attached) throw std::runtime_error("device_busy");
    m_Access.writeDevice("usbip_sockfd", std::to_string(socket) + "\n");
}
void Binding::restore()
{
    if (!m_Progress.detached) return;
    bool same = false;
    try { same = m_Access.current().identity() == m_Original.identity(); }
    catch (const DeviceGone&) {}
    if (same && m_Access.current().driver == "usbip-host") {
        m_Progress.bound = 1;
        m_Access.writeDriver("usbip-host", "match_busid", "del " + m_Original.busId);
        m_Progress.automaticBindingDisabled = 1;
        requireSameDevice();
        // Unbind also terminates the kernel connection and waits for URBs.
        m_Access.writeDriver("usbip-host", "unbind", m_Original.busId);
    }
    std::exception_ptr matchError;
    try {
        if (m_Progress.matched && m_Access.hasMatch()) {
            m_Access.writeDriver("usbip-host", "match_busid", "del " + m_Original.busId);
            // rebind dereferences the kernel's saved usb_device. Only use it
            // after an observed successful bind. SysfsDevice pins that object
            // through cleanup, even after unplug. Never pass an uninitialized
            // match entry.
            if (m_Progress.bound && (same || m_Progress.automaticBindingDisabled)) {
                m_Access.writeDriver("usbip-host", "rebind", m_Original.busId);
            }
        }
    } catch (...) { matchError = std::current_exception(); }
    if (same) {
        requireSameDevice();
        if (!m_Original.driver.empty() && m_Access.current().driver.empty()) m_Access.probe();
        if (m_Original.driver.empty() && m_Access.current().driver == "usb")
            m_Access.writeDriver("usb", "unbind", m_Original.busId);
        if (!m_Original.driver.empty() && m_Access.current().driver != m_Original.driver)
            throw std::runtime_error("restore_failed");
    }
    if (matchError) std::rethrow_exception(matchError);
    if (m_Progress.matched && m_Access.hasMatch()) throw std::runtime_error("match_cleanup_failed");
    m_Progress.detached = 0;
    m_Progress.matched = 0;
    m_Progress.bound = 0;
}

SysfsDevice::SysfsDevice(std::string busId) : m_BusId(std::move(busId))
{
    if (!validBusId(m_BusId)) throw std::runtime_error("invalid_busid");
}
SysfsDevice::~SysfsDevice() { if (m_DeviceFd >= 0) close(m_DeviceFd); }
void SysfsDevice::pin(const Device& device)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", device.bus, device.number);
    m_DeviceFd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info {};
    if (m_DeviceFd < 0 || fstat(m_DeviceFd, &info) != 0 || !S_ISCHR(info.st_mode) ||
        major(info.st_rdev) != 189 || minor(info.st_rdev) != (device.bus - 1) * 128 + device.number - 1 ||
        current().identity() != device.identity()) throw std::runtime_error("device_changed");
}
Device SysfsDevice::current() const { return readDevice(m_BusId, "/sys/bus/usb/devices", false); }
void SysfsDevice::writeDriver(const std::string& driver, const std::string& attribute, const std::string& value)
{
    writeText(fs::path("/sys/bus/usb/drivers") / driver / attribute, value);
}
void SysfsDevice::writeDevice(const std::string& attribute, const std::string& value)
{
    writeText(fs::path("/sys/bus/usb/devices") / m_BusId / attribute, value);
}
void SysfsDevice::probe() { writeDriver("usb", "bind", m_BusId); }
bool SysfsDevice::hasMatch() const
{
    std::istringstream entries(readText("/sys/bus/usb/drivers/usbip-host/match_busid"));
    std::string id;
    while (entries >> id) if (id == m_BusId) return true;
    return false;
}

int serve(const std::string& busId, const std::string& expectedIdentity)
{
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTERM, stopSignal);
    std::signal(SIGINT, stopSignal);
    if (cancelled(STDIN_FILENO)) return 0;
    loadModule();
    SysfsDevice access(busId);
    const auto original = access.current();
    if (original.identity() != expectedIdentity) throw std::runtime_error("device_changed");
    if (!original.supported() || original.driver == "usbip-host") throw std::runtime_error("device_busy");
    Fd lock(open(("/run/lock/moonlight-usb-" + busId + ".lock").c_str(),
                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat lockInfo {};
    if (lock.value < 0 || fstat(lock.value, &lockInfo) != 0 || !S_ISREG(lockInfo.st_mode) ||
            lockInfo.st_uid != 0 || flock(lock.value, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("device_busy");
    if (access.hasMatch()) throw std::runtime_error("device_busy");
    access.pin(original);
    auto* progress = static_cast<BindingProgress*>(mmap(nullptr, sizeof(BindingProgress),
                                    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (progress == MAP_FAILED) throw std::runtime_error("supervisor_failed");
    new (progress) BindingProgress;
    Binding binding(access, original, *progress);
    int channels[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channels) != 0) {
        munmap(progress, sizeof(*progress));
        throw std::runtime_error("supervisor_failed");
    }
    const pid_t child = fork();
    if (child < 0) {
        close(channels[0]); close(channels[1]); munmap(progress, sizeof(*progress));
        throw std::runtime_error("supervisor_failed");
    }
    if (child == 0) {
        close(channels[0]); close(STDIN_FILENO);
        int result = 0;
        try { runExporter(access, binding, channels[1]); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "USB exporter: %s\n", error.what());
            // If startup failed, the parent receives the exit, rather than a
            // second stdout protocol line after READY.
            result = 1;
        }
        try { binding.restore(); }
        catch (const std::exception&) { result = 1; }
        close(channels[1]);
        _exit(result);
    }
    close(channels[1]);
    Fd control(channels[0]);
    int status = 0;
    bool exited = false;
    while (!cancelled(STDIN_FILENO)) {
        if (waitpid(child, &status, WNOHANG) == child) { exited = true; break; }
        poll(nullptr, 0, 100);
    }
    if (!exited) {
        // The child also observes EOF if this supervisor dies, and performs
        // its own rollback. If the worker dies, the supervisor restores it.
        shutdown(control.value, SHUT_RDWR);
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (waitpid(child, &status, WNOHANG) == 0 && Clock::now() < deadline) poll(nullptr, 0, 50);
        if (waitpid(child, &status, WNOHANG) == 0) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    bool restored = false;
    for (int attempt = 0; attempt < 3 && !restored; ++attempt) {
        try { binding.restore(); restored = true; }
        catch (const std::exception&) { poll(nullptr, 0, 100); }
    }
    munmap(progress, sizeof(*progress));
    if (!restored) std::fputs("USB exporter: restore_failed; reconnect the USB device\n", stderr);
    return restored && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
} // namespace NativeUsb
