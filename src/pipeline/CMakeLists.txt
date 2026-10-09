add_library(tobii-pipeline pipeline.cpp)
target_include_directories(tobii-pipeline PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-pipeline PUBLIC
    tobii-vision tobii-pose tobii-usb tobii-calibration tobii-opencv
    PkgConfig::EIGEN)
