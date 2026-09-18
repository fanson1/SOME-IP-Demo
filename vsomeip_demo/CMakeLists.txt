cmake_minimum_required(VERSION 3.16)
project(vsomeip_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(vsomeip CONFIG QUIET)

if(NOT vsomeip_FOUND)
    find_package(vsomeip3 CONFIG QUIET)
endif()

if(TARGET vsomeip::vsomeip AND TARGET vsomeip::vsomeip-sd)
    # installed via CMake package (e.g. build from src, Debian packaging)
    set(VSOMEIP_TARGETS vsomeip::vsomeip vsomeip::vsomeip-sd)
else()
    # fallback: apt-get install libvsomeip-dev
    find_library(VSOMEIP_CORE_LIB NAMES vsomeip3 vsomeip)
    find_library(VSOMEIP_SD_LIB   NAMES vsomeip3-sd vsomeip-sd)
    if(NOT VSOMEIP_CORE_LIB)
        message(FATAL_ERROR "vsomeip not found (apt-get install libvsomeip-dev)")
    endif()
    set(VSOMEIP_TARGETS ${VSOMEIP_CORE_LIB} ${VSOMEIP_SD_LIB} Threads::Threads dl)
    find_path(VSOMEIP_INCLUDE_DIR vsomeip/vsomeip.hpp
              HINTS /usr/include /usr/local/include)
    include_directories(${VSOMEIP_INCLUDE_DIR})
endif()

find_package(Threads REQUIRED)

add_executable(service service.cpp)
target_link_libraries(service ${VSOMEIP_TARGETS} Threads::Threads)

add_executable(client client.cpp)
target_link_libraries(client ${VSOMEIP_TARGETS} Threads::Threads)

install(TARGETS service client RUNTIME DESTINATION bin)