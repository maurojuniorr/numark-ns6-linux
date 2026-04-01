cmake_minimum_required(VERSION 3.14)
project(ns6d VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -O2)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(ALSA   REQUIRED alsa)

find_package(Threads REQUIRED)

message(STATUS "ns6d ${PROJECT_VERSION} — Numark NS6 MIDI daemon")
message(STATUS "  libusb: ${LIBUSB_VERSION}")
message(STATUS "  alsa:   ${ALSA_VERSION}")

add_executable(ns6d
    src/main.c
    src/usb.c
    src/protocol.c
    src/audio.c   # <-- ADICIONE ESTA LINHA!
)

target_include_directories(ns6d PRIVATE
    ${LIBUSB_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    src
)

target_link_libraries(ns6d
    ${LIBUSB_LIBRARIES}
    ${ALSA_LIBRARIES}
    Threads::Threads
)
