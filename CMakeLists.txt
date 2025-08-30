cmake_minimum_required(VERSION 3.10)

project(lanbox VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(include)

# Add source files
add_executable(lanbox src/main.cpp)

# Link Windows socket library
if (WIN32)
    target_link_libraries(lanbox PRIVATE ws2_32)
endif()