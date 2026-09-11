// moonlight-usbd：macOS USB/IP 导出 helper。
//
// 反向隧道架构（docs/remote-usb-reverse-tunnel.md）中的本地 USB/IP 服务器：
// Moonlight 主程序按需拉起本进程，把选定的本地 USB 设备经 usbipdcpp 导出成
// 标准 USB/IP 服务；串流会话再把这个端口的字节流原样转发给 Sunshine。
//
// 与父进程（Session/UsbForwardingLocalServer）的全部约定：
//  - stdout 只写一行协议：serve 成功打 "READY <port>\n"，失败打
//    "ERROR {json}\n"；之后 stdout 永不再写（所有日志——包括 usbipdcpp 内部
//    的 spdlog——一律走 stderr）。
//  - 进程生命周期由父进程控制：关闭 stdin（EOF）或发 SIGTERM/SIGINT 优雅退出。
//  - 退出码：0 干净退出；1 用法/内部错误；2 设备未找到/绑定失败；
//    3 监听失败；4 设备被系统占用。
//
// 三个子命令：
//   moonlight-usbd --version
//   moonlight-usbd list --json
//   moonlight-usbd serve --bind <busid> [--bind <busid>...] --listen <host:port>

#include <asio.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <libusb-1.0/libusb.h>

#include "usbipdcpp/LibusbHandler/LibusbServer.h"

#include <cstdint>
#include <cstdio>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

sigset_t kTerminationSignals;

// JSON 字符串转义。设备描述符文本只会出现常规字符，这里按 RFC 8259 处理
// 必转义字符，非 ASCII 按 UTF-8 原样透传。
std::string jsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                escaped += buffer;
            } else {
                escaped += c;
            }
        }
    }
    return escaped;
}

// stdout 单行 ERROR（协议约定的失败路径）。
void printErrorLine(std::string_view error, std::string_view busId = {},
                    int code = 0, const std::string& detail = {})
{
    std::cout << "ERROR {\"error\":\"" << error << '"';
    if (!busId.empty()) {
        std::cout << ",\"busid\":\"" << jsonEscape(std::string(busId)) << '"';
    }
    if (code != 0) {
        std::cout << ",\"code\":" << code;
    }
    if (!detail.empty()) {
        std::cout << ",\"detail\":\"" << jsonEscape(detail) << '"';
    }
    std::cout << "}\n" << std::flush;
}

// busid 生成。必须与 usbipdcpp::get_device_busid（include/usbipdcpp/LibusbHandler/
// tools.h）保持字节级一致：find_by_busid 用字符串匹配反查设备，两边算法漂移
// 会导致 list 输出的 busid 在 serve 时绑不上。
std::string deviceBusId(libusb_device* device)
{
    uint8_t ports[8];
    const int count = libusb_get_port_numbers(device, ports, 8);
    std::string busid = std::to_string(libusb_get_bus_number(device));
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            busid += (i == 0 ? "-" : ".");
            busid += std::to_string(ports[i]);
        }
    } else {
        busid += "-" + std::to_string(libusb_get_device_address(device))
              + ":" + std::to_string(libusb_get_port_number(device));
    }
    return busid;
}

std::string stringDescriptor(libusb_device_handle* handle, uint8_t index)
{
    if (index == 0 || handle == nullptr) {
        return {};
    }
    char buffer[256];
    const int length = libusb_get_string_descriptor_ascii(
                handle, index, reinterpret_cast<unsigned char*>(buffer),
                sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<size_t>(length));
}

// 占用探测：对 active 配置的每个接口做一次 claim/release。macOS 上被系统
// 驱动（HID/存储/摄像头栈）或其他进程持有的接口 claim 返回 LIBUSB_ERROR_BUSY。
// 任一接口 claim 失败即视为占用。绝不做内核驱动 detach：Darwin 上需要 root，
// 属于未来特权 helper 的范畴。
bool interfacesClaimable(libusb_device* device, libusb_device_handle* handle)
{
    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS) {
        return false;
    }

    bool claimable = true;
    for (int i = 0; i < config->bNumInterfaces && claimable; i++) {
        if (libusb_claim_interface(handle, i) == LIBUSB_SUCCESS) {
            libusb_release_interface(handle, i);
        } else {
            claimable = false;
        }
    }

    libusb_free_config_descriptor(config);
    return claimable;
}

// 独立探测（不持有其他 handle 时用，如 serve 的 fail-fast 检查）。
bool deviceClaimable(libusb_device* device)
{
    libusb_device_handle* handle = nullptr;
    if (libusb_open(device, &handle) != LIBUSB_SUCCESS) {
        return false;
    }
    const bool claimable = interfacesClaimable(device, handle);
    libusb_close(handle);
    return claimable;
}

int runList()
{
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(nullptr, &devices);
    if (count < 0) {
        printErrorLine("enumerate_failed", {}, 0,
                       libusb_error_name(static_cast<int>(count)));
        return 1;
    }

    std::string json = "[";
    bool first = true;
    for (ssize_t i = 0; i < count; i++) {
        libusb_device* device = devices[i];
        libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) {
            continue;
        }
        // hub 不导出（usbipdcpp skip_hub 语义；共享 hub 本身没有意义）。
        if (descriptor.bDeviceClass == LIBUSB_CLASS_HUB) {
            continue;
        }

        const std::string busId = deviceBusId(device);
        std::string serial, manufacturer, product;
        bool claimable = false;
        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == LIBUSB_SUCCESS) {
            serial = stringDescriptor(handle, descriptor.iSerialNumber);
            manufacturer = stringDescriptor(handle, descriptor.iManufacturer);
            product = stringDescriptor(handle, descriptor.iProduct);
            claimable = interfacesClaimable(device, handle);
            libusb_close(handle);
        }

        char vidPid[12];
        std::snprintf(vidPid, sizeof(vidPid), "%04x:%04x",
                      descriptor.idVendor, descriptor.idProduct);

        if (!first) {
            json += ",";
        }
        first = false;
        json += "{\"busId\":\"" + jsonEscape(busId) + "\"";
        json += ",\"vid\":" + std::to_string(descriptor.idVendor);
        json += ",\"pid\":" + std::to_string(descriptor.idProduct);
        json += ",\"vidPid\":\"";
        json += vidPid; // %04x:%04x 自产，无需转义
        json += "\"";
        json += ",\"serial\":\"" + jsonEscape(serial) + "\"";
        json += ",\"manufacturer\":\"" + jsonEscape(manufacturer) + "\"";
        json += ",\"product\":\"" + jsonEscape(product) + "\"";
        json += ",\"claimable\":" + std::string(claimable ? "true" : "false");
        json += "}";
    }
    libusb_free_device_list(devices, 1);

    std::cout << json << "\n" << std::flush;
    return 0;
}

int runServe(const std::vector<std::string>& bindBusIds,
             const std::string& listenHost, uint16_t listenPort)
{
    // 默认配置即所需：skip_hub=true、auto_bind_hotplug=false（热插拔监控仍会
    // 清理已绑定的失效设备，只是不会自动把新设备加进来）。
    usbipdcpp::LibusbServerConfig config;
    usbipdcpp::LibusbServer server(config);

    for (const std::string& busId : bindBusIds) {
        // find_by_busid 返回带引用计数的 device；bind_host_device 接管该引用，
        // 调用方不要 unref（与上游 examples/libusb_server 的用法一致）。
        libusb_device* device = usbipdcpp::LibusbServer::find_by_busid(busId);
        if (device == nullptr) {
            printErrorLine("device_not_found", busId);
            return 2;
        }
        // fail-fast：被系统占用的设备就算 READY 了也会在客户端 attach 时挂掉，
        // 不如现在就报清楚。此路径 bind_host_device 还没接管引用，自己收尾。
        if (!deviceClaimable(device)) {
            libusb_unref_device(device);
            printErrorLine("device_occupied", busId);
            return 4;
        }
        const auto result = server.bind_host_device(device);
        if (result != usbipdcpp::DeviceOperationResult::Success) {
            printErrorLine("bind_failed", busId, static_cast<int>(result));
            return 2;
        }
    }

    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(listenHost),
                                           listenPort);
    const usbipdcpp::error_code error = server.start(endpoint);
    if (error) {
        printErrorLine("listen_failed", {}, 0, error.message());
        return 3;
    }

    // 端口传 0 时由系统分配；start() 之后查询实际端口。
    std::cout << "READY " << server.get_server().endpoint().port() << std::endl;

    // 退出条件：stdin EOF（父进程关闭管道）或 SIGTERM/SIGINT。stdin 监视线
    // 线程用 detach：EOF 时它 raise(SIGTERM) 唤醒 sigwait，进程退出时线程随
    // 之消亡；反过来信号先到时它可能永远阻塞在 read 上，join 会卡死。
    std::thread stdinWatcher([] {
        std::string line;
        while (std::getline(std::cin, line)) {
        }
        std::raise(SIGTERM);
    });
    stdinWatcher.detach();

    int signalNumber = 0;
    sigwait(&kTerminationSignals, &signalNumber);
    server.stop();
    return 0;
}

void usage()
{
    std::fprintf(stderr,
                 "usage: moonlight-usbd --version\n"
                 "       moonlight-usbd list --json\n"
                 "       moonlight-usbd serve --bind <busid> [--bind <busid>...]"
                 " --listen <host:port>\n");
}

} // namespace

int main(int argc, char** argv)
{
    // 第一件事：usbipdcpp 内部用 spdlog 默认 logger（指向 stdout）打日志，
    // 而 stdout 是父进程解析的行协议通道，必须整个重定向到 stderr。
    spdlog::set_default_logger(spdlog::stderr_color_mt("moonlight-usbd"));
    spdlog::set_level(spdlog::level::info);

    std::signal(SIGPIPE, SIG_IGN);
    sigemptyset(&kTerminationSignals);
    sigaddset(&kTerminationSignals, SIGINT);
    sigaddset(&kTerminationSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &kTerminationSignals, nullptr);

    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "moonlight-usbd " << MOONLIGHT_USB_HELPER_VERSION
                  << " (usbipdcpp v" << USBIPDCPP_PINNED_VERSION << ")"
                  << std::endl;
        return 0;
    }

    if (argc >= 2 && std::string_view(argv[1]) == "list") {
        if (argc != 3 || std::string_view(argv[2]) != "--json") {
            usage();
            return 1;
        }
        if (libusb_init(nullptr) != LIBUSB_SUCCESS) {
            printErrorLine("init_failed");
            return 1;
        }
        const int result = runList();
        libusb_exit(nullptr);
        return result;
    }

    if (argc >= 2 && std::string_view(argv[1]) == "serve") {
        std::vector<std::string> bindBusIds;
        std::string listen = "127.0.0.1:0";
        for (int i = 2; i < argc; i++) {
            const std::string_view argument(argv[i]);
            if (argument == "--bind" && i + 1 < argc) {
                bindBusIds.emplace_back(argv[++i]);
            } else if (argument == "--listen" && i + 1 < argc) {
                listen = argv[++i];
            } else {
                usage();
                return 1;
            }
        }
        if (bindBusIds.empty()) {
            usage();
            return 1;
        }

        const auto colon = listen.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == listen.size()) {
            usage();
            return 1;
        }
        const std::string host = listen.substr(0, colon);
        const std::string portText = listen.substr(colon + 1);
        unsigned long portValue = 0;
        try {
            portValue = std::stoul(portText);
        } catch (const std::exception&) {
            usage();
            return 1;
        }
        if (portValue > 65535) {
            usage();
            return 1;
        }

        if (libusb_init(nullptr) != LIBUSB_SUCCESS) {
            printErrorLine("init_failed");
            return 1;
        }
        const int result = runServe(bindBusIds, host, static_cast<uint16_t>(portValue));
        libusb_exit(nullptr);
        return result;
    }

    usage();
    return 1;
}
