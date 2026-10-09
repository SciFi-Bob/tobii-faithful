add_executable(tobii-tray main.cpp eye_icon.cpp
    ${CMAKE_SOURCE_DIR}/src/manager/single_instance.cpp)
target_include_directories(tobii-tray PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-tray PRIVATE tobii-ipc tobii-paths tobii-games tobii-eventlog PkgConfig::SYSTEMD)
target_compile_definitions(tobii-tray PRIVATE
    TOBII_MANAGER_INSTALL="${TOBII_BINDIR}/tobii-manager"
    TOBII_MANAGER_BUILD="${CMAKE_BINARY_DIR}/src/manager/tobii-manager"
    TOBII_DAEMON_INSTALL="${TOBII_BINDIR}/tobiid"
    TOBII_DAEMON_BUILD="${CMAKE_BINARY_DIR}/src/daemon/tobiid")
install(TARGETS tobii-tray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
