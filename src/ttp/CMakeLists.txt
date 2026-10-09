add_library(tobii-ttp
    frame.cpp
    xds.cpp
)
target_include_directories(tobii-ttp PUBLIC ${CMAKE_SOURCE_DIR}/include)
