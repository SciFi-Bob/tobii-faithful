add_library(tobii-device-calib monitor_id.cpp calibration_store.cpp calibration_layout.cpp)
target_include_directories(tobii-device-calib PUBLIC ${CMAKE_SOURCE_DIR}/include)
