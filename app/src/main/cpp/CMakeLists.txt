cmake_minimum_required(VERSION 4.1.2)
project(dcam_node_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(dcam_node_loader SHARED
    jni_main.cpp
    session.cpp
    host_api.cpp
    graph.cpp
)

target_include_directories(dcam_node_loader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/dcam-node-sdk/include"
)

target_compile_options(dcam_node_loader PRIVATE
    -O3
    -flto=thin
    -march=armv8-a+simd
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
)

target_link_options(dcam_node_loader PRIVATE
    -O3
    -flto=thin
    -Wl,--gc-sections
    -Wl,--icf=safe
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(dcam_node_loader PRIVATE -g1)
else()
    target_compile_options(dcam_node_loader PRIVATE -DNDEBUG)
endif()

target_compile_options(dcam_node_loader PRIVATE
    -Wall
    -Wextra
    -Wno-missing-designated-field-initializers
    -Werror
)

target_link_libraries(dcam_node_loader PRIVATE
    android
    dl
    log
)
