cmake_minimum_required(VERSION 3.10)
project(ifap)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------
# source files
# ----------------------------------------------------------------------

FILE(GLOB sources
    "source/*.cpp"
    "source/resource/ifap.rc"
)

# ----------------------------------------------------------------------
# build executable
# ----------------------------------------------------------------------

add_executable(ifap WIN32 ${sources})

# ----------------------------------------------------------------------
# configuration
# ----------------------------------------------------------------------

message(STATUS "Compiler: " ${CMAKE_CXX_COMPILER_ID})

if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    # msvc is set by cmake
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CLANG 1)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Intel")
    set(GCC 1)
endif ()

if (MSVC)
    add_compile_options(/Ox)
else ()
    add_compile_options(-Wall -O3)
endif ()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    add_compile_options(-mavx)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(arm)")
    if (!APPLE)
        # The compiler for Apple Silicons CPUs don't recognize these
        add_definitions (-mfpu=neon -mfloat-abi=hard)
    endif()
    set(CMAKE_CXX_FLAGS "-fpermissive -Wno-psabi")
endif ()

if (NOT WIN32)
    add_definitions (-Wno-switch)
endif ()

find_package(mango REQUIRED)
target_link_libraries(ifap PUBLIC mango)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build: ${CMAKE_BUILD_TYPE}")

if (APPLE)

    target_link_libraries(ifap PUBLIC "-framework OpenGL -framework Cocoa")

    # fix wrapped compiler dropping /usr/local
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I/usr/local/include")
    #link_directories("/usr/local/lib")

elseif (UNIX)

    target_link_libraries(ifap PUBLIC X11 GL)

elseif (WIN32)

    target_compile_options(ifap PUBLIC "/DUNICODE")
    target_link_libraries(ifap PUBLIC opengl32)

endif ()
