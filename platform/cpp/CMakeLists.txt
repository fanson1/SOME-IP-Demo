cmake_minimum_required(VERSION 3.12)
project(someip_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# Core interface library: layered headers (wire/ser/types) + legacy (v1) layer.
add_library(someip INTERFACE)
target_include_directories(someip INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(someip INTERFACE Threads::Threads)

foreach(exe service_demo client_demo)
    add_executable(${exe} examples/${exe}.cpp)
    target_link_libraries(${exe} PRIVATE someip)
endforeach()

add_executable(test_wire_ser tests/test_wire_ser.cpp)
target_link_libraries(test_wire_ser PRIVATE someip)

enable_testing()
add_test(NAME wire_ser COMMAND test_wire_ser)