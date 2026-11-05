cmake_minimum_required(VERSION 3.14)
project(ns6d VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -O2)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(ALSA   REQUIRED alsa)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

find_package(Threads REQUIRED)

message(STATUS "ns6d ${PROJECT_VERSION} — Numark NS6 MIDI daemon")
message(STATUS "  libusb: ${LIBUSB_VERSION}")
message(STATUS "  alsa:   ${ALSA_VERSION}")
message(STATUS "  PipeWire: ${PIPEWIRE_VERSION}")

add_executable(ns6d
    src/main.c
    src/usb.c
    src/protocol.c
    src/audio.c
)

target_include_directories(ns6d PRIVATE
    ${LIBUSB_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    src
)

target_link_libraries(ns6d
    ${LIBUSB_LIBRARIES}
    ${ALSA_LIBRARIES}
    PkgConfig::PIPEWIRE
    Threads::Threads
)

enable_testing()
add_executable(ns6-audio-transport-test src/ns6_audio_transport_test.c)
target_include_directories(ns6-audio-transport-test PRIVATE src)
add_test(NAME ns6-audio-transport COMMAND ns6-audio-transport-test)

install(TARGETS ns6d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(systemd/ns6d.service.in ns6d.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ns6d.service
        DESTINATION /usr/lib/systemd/user)
install(FILES udev/99-ns6.rules DESTINATION /etc/udev/rules.d)

# Remove the old system service and PCM aliases installed by earlier builds.
install(CODE [[
    execute_process(COMMAND systemctl disable --now ns6d.service
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND modprobe -r snd-aloop
                    OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE "/etc/systemd/system/ns6d.service")
    file(REMOVE "/etc/alsa/conf.d/99-numark-ns6.conf")
    execute_process(COMMAND systemctl daemon-reload
                    OUTPUT_QUIET ERROR_QUIET)
]])
