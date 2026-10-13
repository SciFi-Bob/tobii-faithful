add_library(tobii-games catalogue.cpp)
target_include_directories(tobii-games PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-games PRIVATE tobii-eventlog)
