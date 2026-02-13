cmake_minimum_required(VERSION 3.14)
project(seastar-starter VERSION 0.0.1 LANGUAGES CXX)

# 设置C++20标准（支持协程）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 优化编译配置
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2 -fno-omit-frame-pointer -rdynamic")

# 屏蔽将警告信息认定为错误的配置
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -Wno-error")

# ========== Seastar 相关目标 ==========
# 配置seastar只生成库文件，不生成tests、demos、apps
set(Seastar_APPS OFF CACHE BOOL "Disable application targets" FORCE)
set(Seastar_DEMOS OFF CACHE BOOL "Disable demonstration targets" FORCE)
set(Seastar_TESTING OFF CACHE BOOL "Disable testing targets" FORCE)
set(Seastar_MASTER_PROJECT OFF CACHE BOOL "Disable master project mode" FORCE)
add_subdirectory(3rdparty/seastar)

# ========== libfork 相关目标 ==========
add_subdirectory(3rdparty/libfork)

# ========== 自定义目标 ==========

# Seastar程序
add_executable(big_file_splitter big_file_splitter.cpp)
target_link_libraries(big_file_splitter Seastar::seastar)

add_executable(glm5_seastar_prime glm5_seastar_prime.cpp)
target_link_libraries(glm5_seastar_prime Seastar::seastar)

add_executable(minimax_seastar_prime minimax_seastar_prime.cpp)
target_link_libraries(minimax_seastar_prime Seastar::seastar)

# libfork程序
add_executable(glm5_libfork_prime glm5_libfork_prime.cpp)
target_link_libraries(glm5_libfork_prime libfork::libfork)

add_executable(minimax_libfork_prime minimax_libfork_prime.cpp)
target_link_libraries(minimax_libfork_prime libfork::libfork)

# 其他程序
add_executable(prime_bench prime_bench.cpp)

add_executable(sequence_prime sequence_prime.cpp)
