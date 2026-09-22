#include "../../usb-helper/linux/nativeusb.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
int failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void writeAttribute(const fs::path& path, const char* value)
{
    std::ofstream file(path);
    file << value << '\n';
    file.close();
    if (!file) {
        throw std::runtime_error("Could not write test attribute");
    }
}

struct DeviceFixture
{
    QTemporaryDir directory;
    fs::path root;
    fs::path device;

    DeviceFixture()
    {
        if (!directory.isValid()) {
            throw std::runtime_error("Could not create test directory");
        }
        root = directory.path().toStdString();
        device = root / "1-2";
        createDevice();
    }

    void createDevice()
    {
        fs::create_directory(device);
        for (const auto& attribute : { std::pair{ "busnum", "1" },
                                       { "devnum", "7" },
                                       { "idVendor", "1234" },
                                       { "idProduct", "5678" },
                                       { "bcdDevice", "0100" },
                                       { "bDeviceClass", "0" },
                                       { "bDeviceSubClass", "0" },
                                       { "bDeviceProtocol", "0" },
                                       { "bConfigurationValue", "1" },
                                       { "bNumConfigurations", "1" },
                                       { "speed", "480" },
                                       { "usbip_status", "2" } }) {
            writeAttribute(device / attribute.first, attribute.second);
        }
    }

    NativeUsb::Device read() const
    {
        // This is the same read used by SysfsDevice::current() while forwarding.
        return NativeUsb::readDevice("1-2", root.string(), false);
    }
};

// A FIFO models bConfigurationValue waiting for the kernel's device lock.
// Opening its writer proves readDevice has already captured devnum. No sleeps
// determine the interleaving: release the read only after changing the device.
template <typename Change> NativeUsb::Device readDuringReset(DeviceFixture& fixture, Change change)
{
    writeAttribute(fixture.device / "devnum", "0");
    const auto configuration = fixture.device / "bConfigurationValue";
    fs::remove(configuration);
    if (mkfifo(configuration.c_str(), 0600) != 0) {
        throw std::runtime_error("Could not create configuration barrier");
    }

    auto reading = std::async(std::launch::async, [&fixture] { return fixture.read(); });
    int writer = -1;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (writer < 0 && std::chrono::steady_clock::now() < deadline) {
        writer = open(configuration.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (writer < 0) {
            std::this_thread::sleep_for(1ms);
        }
    }
    if (writer < 0) {
        qFatal("readDevice did not reach the configuration barrier");
    }

    // Subsequent snapshots use an ordinary attribute. The blocked reader
    // retains the FIFO descriptor until this snapshot has been released.
    fs::remove(configuration);
    writeAttribute(configuration, "1");
    change();
    require(write(writer, "1\n", 2) == 2, "release the configuration read");
    close(writer);
    if (reading.wait_for(5s) != std::future_status::ready) {
        qFatal("readDevice did not finish after the reset");
    }
    return reading.get();
}

// Inject hotplug inside a bus-ID write, after Binding's pre-write check.
// Model the kernel match states: deleting an allocated match marks it for
// removal; unbind does not erase it, and rebind needs its saved device pinned.
class HotplugAccess final : public NativeUsb::DeviceAccess
{
public:
    HotplugAccess()
    {
        device.busId = "1-2";
        device.bus = 1;
        device.number = 7;
        device.inode = 42;
        device.driver = "usb";
    }
    NativeUsb::Device current() const override
    {
        if (gone)
            throw NativeUsb::DeviceGone();
        return device;
    }
    void pin(const NativeUsb::Device& expected) override
    {
        if (current().identity() != expected.identity())
            throw std::runtime_error("device_changed");
        pinned.push_back(device.inode);
    }
    bool hasMatch() const override { return match != None; }
    void writeDevice(const std::string&, const std::string&) override
    {
        require(false, "replacement tests must never export a socket");
    }
    void writeDriver(const std::string& driver, const std::string& attribute,
                     const std::string& value) override
    {
        ++writes;
        const auto operation =
            driver + "/" + attribute + (attribute == "match_busid" ? "/" + value.substr(0, 3) : "");
        if (operation == replaceAt) {
            replaceAt.clear();
            ++device.inode;
            device.driver = attribute == "unbind" ? driver : "";
            if (attribute == "match_busid") {
                device.driver = "usbip-host";
                match = Allocated;
                matchedInode = device.inode;
            }
        }
        if (attribute == "match_busid") {
            if (value == "add " + device.busId)
                match = Added;
            else if (match == Allocated)
                match = Removing;
        } else if (attribute == "bind") {
            if (!device.driver.empty())
                throw std::runtime_error("device_busy");
            if (driver == "usbip-host") {
                if (match != Added && match != Allocated)
                    throw std::runtime_error("no_match");
                match = Allocated;
                matchedInode = device.inode;
            }
            device.driver = driver;
        } else if (attribute == "unbind") {
            if (device.driver != driver)
                throw std::runtime_error("wrong_driver");
            device.driver.clear();
            if (driver == "usbip-host" && match == Allocated)
                match = Added;
        } else if (attribute == "rebind") {
            require(std::find(pinned.begin(), pinned.end(), matchedInode) != pinned.end(),
                    "rebind must retain the actual kernel device");
            if (failNextRebind) {
                failNextRebind = false;
                throw std::runtime_error("injected_rebind_failure");
            }
            if (!gone && device.inode == matchedInode && device.driver.empty())
                device.driver = "usb";
            match = None;
        } else {
            throw std::runtime_error("unexpected_write");
        }
        if (replaceAfterUnbind && driver == "usb" && attribute == "unbind") {
            replaceAfterUnbind = false;
            ++device.inode;
            device.driver.clear();
        }
        if (unplugAfterRemoval && operation == "usbip-host/match_busid/del") {
            unplugAfterRemoval = false;
            gone = true;
            device.driver.clear();
        }
    }

    NativeUsb::Device device;
    std::string replaceAt;
    bool replaceAfterUnbind = false;
    bool failNextRebind = false;
    bool unplugAfterRemoval = false;
    bool gone = false;
    int writes = 0;

private:
    enum Match
    {
        None,
        Added,
        Allocated,
        Removing
    } match = None;
    uint64_t matchedInode = 0;
    std::vector<uint64_t> pinned;
};

void testDriverMutationRaces()
{
    for (const std::string driver : { "usb", "" }) {
        HotplugAccess access;
        access.device.driver = driver;
        access.pin(access.device);
        NativeUsb::BindingProgress progress;
        NativeUsb::Binding binding(access, access.device, progress);
        binding.bind();
        binding.restore();
        binding.restore();
        require(access.device.driver == driver && !access.hasMatch(),
                "ordinary bind/release must preserve the original driver");
    }
    for (const std::string operation :
         { "usb/unbind", "usbip-host/bind", "usbip-host/match_busid/del" }) {
        HotplugAccess access;
        access.pin(access.device);
        access.replaceAt = operation;
        NativeUsb::BindingProgress progress;
        NativeUsb::Binding binding(access, access.device, progress);
        try {
            binding.bind();
            require(false, "hotplug inside a mutation must fail activation");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed", "report mutation replacement");
        }
        require(!progress.automaticBindingDisabled,
                "a confirmed replacement must not authorize rebind through the original pin");
        binding.restore();
        require(access.device.driver.empty() && !access.hasMatch(),
                "release an exporter claim without guessing a replacement's local driver");
    }
    for (const bool replacement : { false, true }) {
        // Unplug after a successful del, before its post-write identity read.
        // Exercise both initial binding and supervisor replacement cleanup.
        HotplugAccess access;
        const auto original = access.device;
        access.pin(original);
        NativeUsb::BindingProgress progress;
        NativeUsb::Binding worker(access, original, progress);
        NativeUsb::Binding supervisor(access, original, progress);
        if (replacement)
            access.replaceAt = "usbip-host/bind";
        else
            access.unplugAfterRemoval = true;
        try {
            worker.bind();
            require(false, "hotplug during activation must be reported");
        } catch (const NativeUsb::DeviceGone&) {
            require(!replacement, "only the original should unplug during activation");
        } catch (const std::runtime_error& error) {
            require(replacement && std::string(error.what()) == "device_changed",
                    "report replacement capture before supervisor cleanup");
        }
        if (replacement) {
            access.unplugAfterRemoval = true;
            try {
                supervisor.restore();
                require(false, "unplug during replacement cleanup must be reported");
            } catch (const NativeUsb::DeviceGone&) {
            }
        }
        bool restored = false;
        for (int attempt = 0; attempt < 3 && !restored; ++attempt) {
            try {
                supervisor.restore();
                restored = true;
            } catch (const std::runtime_error&) {
            }
        }
        require(restored && !access.hasMatch() && !progress.detached,
                "unplug after successful removal must not leave a stale match");
    }
    {
        // A was detached successfully; an intentionally unbound B appears
        // before the post-write read. Nothing in that read proves we detached B.
        HotplugAccess access;
        access.pin(access.device);
        access.replaceAfterUnbind = true;
        NativeUsb::BindingProgress progress;
        NativeUsb::Binding binding(access, access.device, progress);
        try {
            binding.bind();
            require(false, "replacement after a successful unbind must fail activation");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed",
                    "report post-unbind replacement");
        }
        binding.restore();
        require(access.device.driver.empty() && !access.hasMatch() && access.writes == 1,
                "an untouched, unbound replacement must remain unbound");
    }
    {
        // The worker can exit before observing the post-bind identity. Its
        // supervisor must also release a replacement using shared progress.
        HotplugAccess access;
        const auto original = access.device;
        access.pin(original);
        NativeUsb::BindingProgress progress;
        progress.detached = 1;
        progress.matched = 1;
        access.writeDriver("usb", "unbind", original.busId);
        access.writeDriver("usbip-host", "match_busid", "add " + original.busId);
        access.replaceAt = "usbip-host/bind";
        access.writeDriver("usbip-host", "bind", original.busId);
        NativeUsb::Binding supervisor(access, original, progress);
        supervisor.restore();
        require(access.device.driver.empty() && !access.hasMatch(),
                "supervisor cleanup must undo a replacement bind too");
    }
    {
        HotplugAccess access;
        const auto original = access.device;
        access.pin(original);
        NativeUsb::BindingProgress progress;
        NativeUsb::Binding worker(access, original, progress);
        NativeUsb::Binding supervisor(access, original, progress);
        access.replaceAt = "usbip-host/bind";
        try {
            worker.bind();
            require(false, "capturing a replacement must fail activation");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed", "report replacement capture");
        }
        const int writes = access.writes;
        try {
            worker.restore(false);
            require(false, "worker must defer replacement recovery to its supervisor");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed", "defer replacement recovery");
        }
        require(access.device.driver == "usbip-host" && access.writes == writes,
                "worker must leave the replacement available for its supervisor to pin");
        access.failNextRebind = true;
        try {
            supervisor.restore();
            require(false, "the injected cleanup failure must be reported");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "injected_rebind_failure",
                    "preserve the cleanup error after releasing the replacement");
        }
        require(access.device.driver.empty() && access.hasMatch() && progress.detached,
                "failed cleanup must retain progress after replacement unbind");
        supervisor.restore();
        supervisor.restore();
        require(access.device.driver.empty() && !access.hasMatch() && !progress.detached,
                "supervisor retry must resume replacement cleanup and clear the match");
    }
    {
        HotplugAccess access;
        access.device.driver.clear();
        access.pin(access.device);
        NativeUsb::BindingProgress progress;
        NativeUsb::Binding binding(access, access.device, progress);
        binding.bind();
        access.replaceAt = "usb/unbind";
        try {
            binding.restore();
            require(false, "hotplug during restoration must be detected");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed", "report replacement in cleanup");
        }
        binding.restore();
        require(access.device.driver.empty() && !access.hasMatch(),
                "cleanup must not guess the replacement's previous driver");
    }
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        testDriverMutationRaces();
        DeviceFixture reset;
        const auto original = reset.read();
        try {
            const auto device =
                readDuringReset(reset, [&] { writeAttribute(reset.device / "devnum", "7"); });
            require(device.identity() == original.identity() && device.attached,
                    "a completed reset must preserve the attached device's identity");
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Reset read failed: %s\n", error.what());
            require(false, "a normal reset must not terminate forwarding");
        }

        DeviceFixture replacement;
        try {
            readDuringReset(replacement, [&] {
                // Keep the old inode allocated; use identical USB descriptors
                // and address to ensure replacement detection uses the inode.
                fs::rename(replacement.device, replacement.root / "removed");
                replacement.createDevice();
            });
            require(false, "a replacement at the same port must be rejected");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed", "report replacement");
        }

        DeviceFixture unplugged;
        try {
            readDuringReset(unplugged,
                            [&] { fs::rename(unplugged.device, unplugged.root / "removed"); });
            require(false, "an unplugged device must be rejected");
        } catch (const NativeUsb::DeviceGone&) {
            // Expected: retries must not hide an actual disconnect.
        }

        DeviceFixture unfinished;
        writeAttribute(unfinished.device / "devnum", "0");
        try {
            auto reading = std::async(std::launch::async, [&] { return unfinished.read(); });
            if (reading.wait_for(5s) != std::future_status::ready) {
                qFatal("readDevice did not bound its reset retries");
            }
            reading.get();
            require(false, "a device stuck at address zero must not be accepted");
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "device_changed", "bound reset retries");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Unexpected test failure: %s\n", error.what());
        ++failures;
    }
    if (!failures) {
        std::puts("PASS Linux USB: reset, replacement, retry limits, driver mutation rollback");
    }
    return failures ? 1 : 0;
}
