cmake_minimum_required(VERSION 3.16)
project(SignalingServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.83 REQUIRED COMPONENTS system thread date_time)
find_package(nlohmann_json REQUIRED)

add_executable(signaling_server signaling_server.cpp)

target_include_directories(signaling_server PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(signaling_server PRIVATE
    Boost::system
    Boost::thread
    Boost::date_time 
    
    Threads::Threads 
    
    nlohmann_json::nlohmann_json
)
