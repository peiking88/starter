cmake_minimum_required(VERSION 3.7.1)
project(seastar-starter VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(seastar)

add_executable(big_file_splitter big_file_splitter.cpp)
target_link_libraries(big_file_splitter Seastar::seastar)

add_executable(prime_calculator prime_calculator.cpp)
target_link_libraries(prime_calculator Seastar::seastar)

add_executable(test_simple test_simple.cpp)
target_link_libraries(test_simple Seastar::seastar)