cmake_minimum_required(VERSION 3.7.1)
project(seastar-starter VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(seastar)

add_executable(main big_file_splitter.cpp)
target_link_libraries(main Seastar::seastar)
