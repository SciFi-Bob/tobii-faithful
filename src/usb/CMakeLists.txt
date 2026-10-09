add_library(tobii-usb
    protocol.cpp
    realm_secrets.cpp
    decode.cpp
    device.cpp
    transport_libusb.cpp
)
target_include_directories(tobii-usb PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(tobii-usb PUBLIC PkgConfig::LIBUSB PkgConfig::OPENSSL)
