#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <stdexcept>
#include <vector>

namespace NativeUsb {

bool validBusId(const std::string& value);

class DeviceGone final : public std::runtime_error
{
public:
    DeviceGone() : std::runtime_error("device_not_found") {}
};

struct Device
{
    std::string busId, path, product, manufacturer, serial, driver;
    uint32_t bus = 0, number = 0, speed = 0;
    uint16_t vendor = 0, productId = 0, revision = 0;
    uint8_t deviceClass = 0, subClass = 0, protocol = 0;
    uint8_t configuration = 0, configurations = 0;
    std::vector<std::array<uint8_t, 4>> interfaces;
    uint64_t inode = 0;
    bool attached = false;

    std::string vidPid() const;
    std::string identity() const;
    std::string registrationKey() const;
    bool supported() const;
};

Device readDevice(const std::string& busId, const std::string& root = "/sys/bus/usb/devices",
                  bool interfaces = true);
std::string deviceListJson(const std::string& root = "/sys/bus/usb/devices");
std::vector<uint8_t> deviceDescriptor(const Device& device);

// Import negotiation only. After exportSocket(), usbip-host owns all USB/IP
// transfer processing.
bool negotiate(int socket, const Device& device, const std::function<void(int)>& exportSocket,
               int controlFd);

// The supervisor and worker share this state for rollback after a worker crash.
// detached/matched record intent before writes; bound/automaticBindingDisabled
// record observed state of the pinned device.
struct BindingProgress
{
    volatile int detached = 0;
    volatile int matched = 0;
    volatile int bound = 0;
    volatile int automaticBindingDisabled = 0;
};

class DeviceAccess
{
public:
    virtual ~DeviceAccess() = default;
    // Retain the kernel object through any rollback, including after unplug.
    virtual void pin(const Device& device) = 0;
    virtual Device current() const = 0;
    virtual void writeDriver(const std::string& driver, const std::string& attribute,
                             const std::string& value) = 0;
    virtual void writeDevice(const std::string& attribute, const std::string& value) = 0;
    virtual bool hasMatch() const = 0;
};

class Binding
{
public:
    Binding(DeviceAccess& access, Device original, BindingProgress& progress);
    void bind();
    // Idempotent. A live supervisor must own replacement recovery and its
    // device pin; its worker passes false and leaves that cleanup to it.
    void restore(bool recoverReplacements = true);
    void exportSocket(int socket);

private:
    void requireSameDevice() const;
    void changeDriver(const std::string& driver, const std::string& attribute);
    void disableAutomaticBinding();
    DeviceAccess& m_Access;
    Device m_Original;
    BindingProgress& m_Progress;
};

// Production uses only the real sysfs tree. No privileged CLI accepts a root
// path, executable path, environment override, or arbitrary sysfs attribute.
class SysfsDevice final : public DeviceAccess
{
public:
    explicit SysfsDevice(std::string busId);
    ~SysfsDevice() override;
    void pin(const Device& device) override;
    Device current() const override;
    void writeDriver(const std::string&, const std::string&, const std::string&) override;
    void writeDevice(const std::string&, const std::string&) override;
    bool hasMatch() const override;

private:
    std::string m_BusId;
    std::vector<int> m_DeviceFds;
    int m_SysfsFd = -1;
};

int serve(const std::string& busId, const std::string& expectedIdentity);

} // namespace NativeUsb
