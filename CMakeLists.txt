cmake_minimum_required(VERSION 3.15)
project(DeCrusher)

set(CMAKE_CXX_STANDARD 17)
add_compile_definitions(UNICODE _UNICODE _WINSOCK_DEPRECATED_NO_WARNINGS)

add_executable(DeCrusher WIN32
    src/main.cpp
    src/AudioReset.cpp
    src/AudioReset.h
)

target_link_libraries(DeCrusher PRIVATE
    kernel32
    user32
    gdi32
    shell32
    ole32
    oleaut32
    mmdevapi
    propsys
    ws2_32
    iphlpapi
)
