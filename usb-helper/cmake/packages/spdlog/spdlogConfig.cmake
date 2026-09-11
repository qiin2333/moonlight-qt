# 极简 spdlog config 包（header-only 模式）：满足 usbipdcpp 的
# find_package(spdlog CONFIG)。spdlog 默认就是头文件模式，未定义
# SPDLOG_COMPILED_LIB 时不需要链接任何库。
add_library(spdlog::spdlog INTERFACE IMPORTED)
set_target_properties(spdlog::spdlog PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/spdlog/include")
