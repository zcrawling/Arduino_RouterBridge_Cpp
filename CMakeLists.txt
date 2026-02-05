cmake_minimum_required(VERSION 4.1)
project(Arduino_RouterBridge)

set(CMAKE_CXX_STANDARD 20)

add_executable(Arduino_RouterBridge Bridge.cpp
        event.cpp
        event.h)
