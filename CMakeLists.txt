cmake_minimum_required(VERSION 3.10.1)
project(seastar-starter VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0 -fno-omit-frame-pointer -rdynamic -w")

add_subdirectory(seastar)

add_executable(big_file_splitter big_file_splitter.cpp)
target_link_libraries(big_file_splitter Seastar::seastar)

add_executable(prime_calculator prime_calculator.cpp)
target_link_libraries(prime_calculator Seastar::seastar)

add_executable(prime_bench prime_bench.cpp)
target_link_libraries(prime_bench Seastar::seastar)
