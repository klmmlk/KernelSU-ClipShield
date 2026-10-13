cmake_minimum_required(VERSION 3.22)
project(clipshield LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# LSPlant v6.4 的头文件使用 std::function 但未自行 include <functional>，
# 强制包含以修复（放在 CXX FLAGS 里：只对 C++ 生效，且被子目录继承）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include functional")

# Dobby 使用 Maven Central 预编译库（io.github.vvb2060.ndk:dobby:1.2，
# 与 LSPlant 官方测试所用一致；Dobby master 存在损坏期，源码构建不可靠）
add_library(dobby_static STATIC IMPORTED GLOBAL)
set_target_properties(dobby_static PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dobby-prebuilt/android.arm64-v8a/libdobby.a
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dobby-prebuilt)

# 提供 lsplant_static 目标（内部会 add_subdirectory external/dex_builder）
add_subdirectory(third_party/LSPlant/lsplant/src/main/jni lsplant_build)

add_library(clipshield SHARED
    native/src/main.cpp
    native/src/config.cpp
    native/src/hook_installer.cpp
    # ElfImg：支持 .gnu_debugdata 压缩符号表的 libart 符号解析器
    # （GPL-3.0, LSPosed/Aliucord；2024-08 起 ART 主线更新把内部符号挪进该段）
    native/src/elfimg/elf_util.cpp
    # XZ Embedded 解码器（public domain）供 ElfImg 解压 .gnu_debugdata
    native/src/elfimg/xz/xz_crc32.c
    native/src/elfimg/xz/xz_crc64.c
    native/src/elfimg/xz/xz_dec_stream.c
    native/src/elfimg/xz/xz_dec_lzma2.c
)

target_include_directories(clipshield PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/native/src
    ${CMAKE_CURRENT_SOURCE_DIR}/native/src/embed
    ${CMAKE_CURRENT_SOURCE_DIR}/native/src/elfimg
)

target_link_libraries(clipshield PRIVATE
    lsplant_static
    dobby_static
    log
)

target_compile_options(clipshield PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_options(clipshield PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
