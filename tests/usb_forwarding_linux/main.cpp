#include "../../usb-helper/linux/nativeusb.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <chrono>
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
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
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
        std::puts("PASS Linux USB reads: reset, replacement, unplug, retry limit");
    }
    return failures ? 1 : 0;
}
