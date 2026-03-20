cmake_minimum_required(VERSION 3.15)
project(server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output binaries to build/ at workspace root
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../build)

find_package(Threads REQUIRED)

add_executable(server server.cpp)

target_include_directories(server PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(server PRIVATE Threads::Threads)
