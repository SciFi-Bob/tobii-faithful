cmake_minimum_required(VERSION 3.20)
project(tobii-faithful VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_definitions(EIGEN_MPL2_ONLY)

include(GNUInstallDirs)

set(TOBII_MODEL_DIR "${CMAKE_SOURCE_DIR}/assets/models"
    CACHE PATH "Directory containing the ONNX face models, for the tests")

set(TOBII_DATADIR   "${CMAKE_INSTALL_FULL_DATADIR}/tobii-faithful")
set(TOBII_LIBEXEC   "${CMAKE_INSTALL_FULL_LIBDIR}/tobii-faithful")
set(TOBII_BINDIR    "${CMAKE_INSTALL_FULL_BINDIR}")

option(TOBII_REQUIRE_BRIDGE
       "Fail the build if the Wine bridge cannot be cross-compiled. On for a package." OFF)
option(TOBII_INSTALL_UDEV_RULES
       "Install the udev rule. Off for a distro package that ships it itself." ON)
set(TOBII_PROJECT_URL "https://github.com/SciFi-Bob/tobii-faithful"
    CACHE STRING "Project home, written into the unit files' Documentation=")

find_package(PkgConfig REQUIRED)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
add_library(tobii-opencv INTERFACE)
target_include_directories(tobii-opencv INTERFACE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(tobii-opencv INTERFACE opencv_core opencv_imgproc opencv_imgcodecs)
message(STATUS "opencv: ${OpenCV_VERSION}, linking core imgproc imgcodecs only")
pkg_check_modules(EIGEN  REQUIRED IMPORTED_TARGET eigen3)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
pkg_check_modules(SYSTEMD IMPORTED_TARGET libsystemd)
find_package(OpenGL)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET libcrypto)

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
          PATH_SUFFIXES onnxruntime onnxruntime/core/session)
find_library(ONNXRUNTIME_LIB NAMES onnxruntime)
if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
  message(FATAL_ERROR "onnxruntime not found - install onnxruntime-devel")
endif()
message(STATUS "onnxruntime: ${ONNXRUNTIME_LIB}")

add_subdirectory(src/paths)
add_subdirectory(src/eventlog)
add_subdirectory(src/usb)
add_subdirectory(src/ttp)
add_subdirectory(src/ipc)
add_subdirectory(src/games)
add_subdirectory(src/calibration)
add_subdirectory(src/device)
add_subdirectory(src/vision)
add_subdirectory(src/pose)
add_subdirectory(src/middleware)
add_subdirectory(src/wine)
add_subdirectory(src/wine_detect)
add_subdirectory(src/pipeline)
add_subdirectory(src/daemon)
add_subdirectory(src/ctl)
add_subdirectory(src/adapter)
if(GLFW_FOUND AND OpenGL_FOUND)
    add_subdirectory(third_party)
    add_subdirectory(src/manager)
    if(SYSTEMD_FOUND)
        add_subdirectory(src/tray)
    else()
        message(WARNING "libsystemd not found - tobii-tray will not be built")
    endif()
else()
    message(WARNING "glfw3 or OpenGL not found - tobii-manager will not be built")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/assets/models/face_detector.onnx")
    install(FILES
            assets/models/face_detector.onnx
            assets/models/face_landmarks_detector.onnx
            assets/models/README.md
            DESTINATION ${CMAKE_INSTALL_DATADIR}/tobii-faithful/models)
endif()

if(TOBII_INSTALL_UDEV_RULES)
    install(FILES configs/udev/60-tobii-eyetracker5.rules
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/udev/rules.d)
endif()

install(FILES configs/desktop/tobii-faithful.desktop
              configs/desktop/tobii-manager.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES configs/desktop/tobii-faithful.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES configs/desktop/tobii-tray.desktop
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
pkg_get_variable(SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
if(NOT SYSTEMD_USER_UNIT_DIR)
    set(SYSTEMD_USER_UNIT_DIR "lib/systemd/user")
endif()
set(TOBII_SYSTEMD_USER_UNIT_DIR "${SYSTEMD_USER_UNIT_DIR}"
    CACHE PATH "Where the systemd user units are installed")
foreach(unit tobiid tobii-adapter)
    configure_file(configs/systemd/${unit}.service.in
                   ${CMAKE_BINARY_DIR}/configs/systemd/${unit}.service @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/configs/systemd/${unit}.service
            DESTINATION ${TOBII_SYSTEMD_USER_UNIT_DIR})
endforeach()
