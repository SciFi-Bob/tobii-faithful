add_executable(tobiictl main.cpp)
target_link_libraries(tobiictl PRIVATE tobii-ipc)
install(TARGETS tobiictl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
