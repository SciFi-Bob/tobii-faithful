add_library(tobii-pose head_pose.cpp)
target_include_directories(tobii-pose PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-pose PUBLIC PkgConfig::EIGEN)
