#include "nativeusb.h"

#include <cstdio>
#include <exception>
#include <string>
#include <unistd.h>

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::puts("moonlight-usb-host 1 (Linux usbip-host)");
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "list" && std::string(argv[2]) == "--json") {
            std::puts(NativeUsb::deviceListJson().c_str());
            return 0;
        }
        if (argc == 6 && std::string(argv[1]) == "serve" &&
                std::string(argv[2]) == "--busid" &&
                std::string(argv[4]) == "--identity" &&
                NativeUsb::validBusId(argv[3]) && std::string(argv[5]).size() <= 512) {
            if (geteuid() != 0) {
                std::puts("ERROR permission_denied");
                return 1;
            }
            return NativeUsb::serve(argv[3], argv[5]);
        }
        std::fputs("Usage: moonlight-usb-host --version | list --json | "
                   "serve --busid ID --identity IDENTITY\n", stderr);
        return 2;
    } catch (const std::exception& error) {
        // Do not include USB descriptor strings or tunnel credentials.
        std::fprintf(stderr, "USB exporter: %s\n", error.what());
        std::printf("ERROR %s\n", error.what());
        return 1;
    }
}
