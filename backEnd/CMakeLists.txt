cmake_minimum_required(VERSION 3.15)
project(server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../build)

include(FetchContent)

# cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
)
FetchContent_MakeAvailable(httplib)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# AWS SDK (only the components we need)
FetchContent_Declare(
    aws-sdk-cpp
    GIT_REPOSITORY https://github.com/aws/aws-sdk-cpp.git
    GIT_TAG        1.11.321
)
set(BUILD_ONLY "bedrock-runtime" CACHE STRING "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(AUTORUN_UNIT_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(aws-sdk-cpp)

find_package(Threads REQUIRED)

add_executable(server server.cpp)

target_include_directories(server PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(server PRIVATE
    Threads::Threads
    httplib::httplib
    nlohmann_json::nlohmann_json
    AWS::aws-sdk-cpp-bedrock-runtime
)
