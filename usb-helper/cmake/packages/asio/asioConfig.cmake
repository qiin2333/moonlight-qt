# 极简 asio config 包：asio 官方只在安装后才提供 config 文件，submodule
# 场景拿不到。asio 是纯头文件库（define ASIO_STANDALONE 时无 Boost 依赖），
# 一个 INTERFACE IMPORTED 目标足够满足 usbipdcpp 的 find_package(asio CONFIG)。
add_library(asio::asio INTERFACE IMPORTED)
set_target_properties(asio::asio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/asio/asio/include"
    INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE")
