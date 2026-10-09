add_library(tobii-adapter-translate translate.cpp)
target_include_directories(tobii-adapter-translate PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(tobii-adapter main.cpp)
target_link_libraries(tobii-adapter PRIVATE tobii-adapter-translate tobii-middleware
    tobii-ipc tobii-wine tobii-calibration tobii-games tobii-paths tobii-eventlog)
target_compile_definitions(tobii-adapter PRIVATE
    TOBII_BRIDGE_INSTALL="${TOBII_LIBEXEC}/tobii-sesp-bridge.exe"
    TOBII_BRIDGE_BUILD="${CMAKE_BINARY_DIR}/src/wine/tobii-sesp-bridge.exe")
install(TARGETS tobii-adapter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
