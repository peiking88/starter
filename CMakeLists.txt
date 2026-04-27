cmake_minimum_required(VERSION 3.14)
project(seastar-starter VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2 -fno-omit-frame-pointer -rdynamic")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -Wno-error")

set(Seastar_APPS OFF CACHE BOOL "Disable application targets" FORCE)
set(Seastar_DEMOS OFF CACHE BOOL "Disable demonstration targets" FORCE)
set(Seastar_TESTING OFF CACHE BOOL "Disable testing targets" FORCE)
set(Seastar_MASTER_PROJECT OFF CACHE BOOL "Disable master project mode" FORCE)
add_subdirectory(external/seastar)

add_subdirectory(external/libfork)

add_executable(big_file_splitter src/big_file_splitter.cpp)
target_link_libraries(big_file_splitter Seastar::seastar)

add_executable(glm5_seastar_prime src/glm5_seastar_prime.cpp)
target_link_libraries(glm5_seastar_prime Seastar::seastar)


add_executable(glm5_libfork_prime src/glm5_libfork_prime.cpp)
target_link_libraries(glm5_libfork_prime libfork::libfork)

add_executable(minimax_seastar_prime src/minimax_seastar_prime.cpp)
target_link_libraries(minimax_seastar_prime Seastar::seastar)

add_executable(minimax_libfork_prime src/minimax_libfork_prime.cpp)
target_link_libraries(minimax_libfork_prime libfork::libfork)

add_executable(sequence_prime src/sequence_prime.cpp)


add_executable(prime_bench src/prime_bench.cpp)


add_executable(sonnet46_seastar_prime src/sonnet46_seastar_prime.cpp)
target_link_libraries(sonnet46_seastar_prime Seastar::seastar)

add_executable(kimi_seastar_prime src/kimi_seastar_prime.cpp)
target_link_libraries(kimi_seastar_prime Seastar::seastar)

add_executable(dk4_seastar_prime src/dk4_seastar_prime.cpp)
target_link_libraries(dk4_seastar_prime Seastar::seastar)

