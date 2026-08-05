cmake_minimum_required(VERSION 3.19)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(ifap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB sources
    "source/*.cpp"
    "source/*.hpp"
    "source/render/*.cpp"
    "source/render/*.hpp"
    "source/render/*/*.cpp"
    "source/render/*/*.hpp"
    "source/resource/ifap.rc"
)

add_executable(ifap WIN32 ${sources})

target_include_directories(ifap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)

if(MSVC)
    target_compile_options(ifap PRIVATE
        /W3
        $<$<CONFIG:Release>:/Ox>
    )
    target_compile_definitions(ifap PRIVATE UNICODE _UNICODE)
else()
    target_compile_options(ifap PRIVATE
        -Wall
        $<$<CONFIG:Release>:-O3>
    )
    if(NOT WIN32)
        target_compile_options(ifap PRIVATE -Wno-switch)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
        target_compile_options(ifap PRIVATE -mavx)
    endif()
endif()

find_package(mango REQUIRED)
target_link_libraries(ifap PRIVATE mango::mango)

message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Build: ${CMAKE_BUILD_TYPE}")
