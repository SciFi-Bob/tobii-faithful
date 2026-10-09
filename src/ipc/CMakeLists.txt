add_library(tobii-ipc
    protocol.cpp
    server.cpp
)
target_include_directories(tobii-ipc PUBLIC ${CMAKE_SOURCE_DIR}/include)
