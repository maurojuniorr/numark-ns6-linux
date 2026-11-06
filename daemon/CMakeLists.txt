cmake_minimum_required(VERSION 3.14)
project(ns6d VERSION 0.1.9 LANGUAGES C)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

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
target_compile_definitions(ns6d PRIVATE NS6D_VERSION="${PROJECT_VERSION}")

enable_testing()
add_executable(ns6-audio-transport-test src/ns6_audio_transport_test.c)
target_include_directories(ns6-audio-transport-test PRIVATE src)
target_compile_options(ns6-audio-transport-test PRIVATE -UNDEBUG)
add_test(NAME ns6-audio-transport COMMAND ns6-audio-transport-test)

install(TARGETS ns6d RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS status-app/ns6-status DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES status-app/io.github.maurojuniorr.ns6-status.desktop
        DESTINATION share/applications)
configure_file(systemd/ns6d.service.in ns6d.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ns6d.service
        DESTINATION /usr/lib/systemd/user)
install(FILES udev/99-ns6.rules DESTINATION /etc/udev/rules.d)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "numark-ns6-driver")
set(CPACK_PACKAGE_VENDOR "Mauro Junior")
set(CPACK_PACKAGE_CONTACT "Mauro Junior")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Numark NS6 userspace audio and MIDI driver")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "python3-gi, gir1.2-gtk-3.0")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Mauro Junior")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst")
include(CPack)
