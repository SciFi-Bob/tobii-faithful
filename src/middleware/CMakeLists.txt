add_library(tobii-middleware server.cpp)
target_include_directories(tobii-middleware PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-middleware PUBLIC tobii-ttp)
