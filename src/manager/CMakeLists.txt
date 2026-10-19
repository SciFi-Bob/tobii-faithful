add_executable(tobii-manager
    main.cpp
    client.cpp
    head_view.cpp
    single_instance.cpp
    calibration_ui.cpp
    calibration_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/tray/eye_icon.cpp
)
target_link_libraries(tobii-manager PRIVATE imgui tobii-calibration tobii-device-calib
                      tobii-games tobii-ipc tobii-eventlog tobii-opencv)
target_include_directories(tobii-manager PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(tobii-manager PRIVATE TOBII_VERSION="${TOBII_VERSION}")
install(TARGETS tobii-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
