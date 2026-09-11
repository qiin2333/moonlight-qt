cmake_minimum_required(VERSION 3.24)
project(moonlight-usbd VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- vendored 依赖（third_party/ 下 4 个 git submodule，.gitmodules 锁 tag）---
#
# asio/spdlog 官方只在安装后才提供 CMake config 包，submodule 场景拿不到；
# 用我们自己的极简 config 包（cmake/packages/）把 find_package 指到源码树。
# libusb/usbipdcpp 直接 add_subdirectory 静态构建。
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/packages")

# libusb 静态库。v1.0.29 没有 CMake 构建（autotools/Xcode），也不想在打包
# 脚本里引入 autotools 依赖：直接按 Makefile.am 的 darwin+posix 源文件清单
# 编译，config.h 用 cmake/libusb-config.h.in（内容同源于 Xcode/config.h）。
# usbipdcpp 按 Homebrew 布局 include <libusb-1.0/libusb.h>，vendored 源码树是
# libusb/libusb.h —— 再生成一个转发头补齐布局。
set(LIBUSB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb/libusb")
add_library(usb-1.0 STATIC
    "${LIBUSB_DIR}/core.c"
    "${LIBUSB_DIR}/descriptor.c"
    "${LIBUSB_DIR}/hotplug.c"
    "${LIBUSB_DIR}/io.c"
    "${LIBUSB_DIR}/strerror.c"
    "${LIBUSB_DIR}/sync.c"
    "${LIBUSB_DIR}/os/events_posix.c"
    "${LIBUSB_DIR}/os/threads_posix.c"
    "${LIBUSB_DIR}/os/darwin_usb.c")
set_target_properties(usb-1.0 PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
target_include_directories(usb-1.0 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb/libusb")
target_include_directories(usb-1.0 PRIVATE "${CMAKE_BINARY_DIR}/libusb-config")
target_compile_options(usb-1.0 PRIVATE -fvisibility=hidden)
target_link_libraries(usb-1.0 PRIVATE objc)
target_link_options(usb-1.0 INTERFACE
    "-framework" "IOKit" "-framework" "CoreFoundation" "-framework" "Security")

configure_file(cmake/libusb-config.h.in
               "${CMAKE_BINARY_DIR}/libusb-config/config.h" COPYONLY)
set(LIBUSB_HEADER_PATH "${LIBUSB_DIR}/libusb.h")
configure_file(cmake/libusb-1.0.h.in
               "${CMAKE_BINARY_DIR}/libusb-shim/libusb-1.0/libusb.h" @ONLY)

# usbipdcpp 的 libusb 组件无条件走 pkg-config（pkg_check_modules libusb-1.0），
# 给上面的静态构建生成一个 .pc 注入 PKG_CONFIG_PATH。框架/objc 放 Libs 而不是
# Libs.private：pkg_check_modules 不带 --static，不会看 private。
configure_file(cmake/libusb-1.0.pc.in
               "${CMAKE_BINARY_DIR}/pkgconfig/libusb-1.0.pc" @ONLY)
set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${CMAKE_BINARY_DIR}/pkgconfig")

set(USBIPDCPP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USBIPDCPP_BUILD_VIRTUAL_DEVICE OFF CACHE BOOL "" FORCE)
set(USBIPDCPP_BUILD_LIBUSB_COMPONENTS ON CACHE BOOL "" FORCE)
set(USBIPDCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(USBIPDCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/usbipdcpp EXCLUDE_FROM_ALL)

add_executable(moonlight-usbd main.cpp)
target_compile_definitions(moonlight-usbd PRIVATE
    MOONLIGHT_USB_HELPER_VERSION="${PROJECT_VERSION}"
    USBIPDCPP_PINNED_VERSION="1.0.9")
target_link_libraries(moonlight-usbd PRIVATE usbipdcpp_libusb)
