cmake_minimum_required(VERSION 3.16)
project(SignalingServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json REQUIRED)

add_executable(signaling_server signaling_server.cpp)

target_include_directories(signaling_server PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(signaling_server PRIVATE ${Boost_LIBRARIES} pthread nlohmann_json::nlohmann_json)
