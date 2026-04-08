cmake_minimum_required(VERSION 3.31.6)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(service)
include(os)
set(SOURCES
    service.cpp
)
os_add_executable(memory_available "Memory available test" ${SOURCES})
os_add_stdout(memory_available default_stdout)

configure_file(test.py ${CMAKE_CURRENT_BINARY_DIR})

configure_file(vm-default.json ${CMAKE_CURRENT_BINARY_DIR})
configure_file(vm-128m.json ${CMAKE_CURRENT_BINARY_DIR})
configure_file(vm-2g.json ${CMAKE_CURRENT_BINARY_DIR})
configure_file(vm-4g.json ${CMAKE_CURRENT_BINARY_DIR})
configure_file(vm-6g.json ${CMAKE_CURRENT_BINARY_DIR})
configure_file(vm-16g.json ${CMAKE_CURRENT_BINARY_DIR})

