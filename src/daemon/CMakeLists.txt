add_library(tobii-daemon-commands commands.cpp)
target_include_directories(tobii-daemon-commands PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-daemon-commands PUBLIC
    tobii-pipeline tobii-ipc tobii-calibration tobii-opencv)

add_executable(tobiid main.cpp)
target_link_libraries(tobiid PRIVATE
    tobii-daemon-commands tobii-pipeline tobii-usb tobii-ipc tobii-calibration
    tobii-device-calib tobii-paths tobii-eventlog tobii-opencv)
target_compile_definitions(tobiid PRIVATE
    TOBII_MODEL_DIR_INSTALL="${TOBII_DATADIR}/models"
    TOBII_MODEL_DIR_BUILD="${CMAKE_SOURCE_DIR}/assets/models")
install(TARGETS tobiid RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
