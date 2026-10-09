find_program(MINGW_CC x86_64-w64-mingw32-gcc)

if(MINGW_CC)
    set(SESP_BRIDGE_EXE "${CMAKE_CURRENT_BINARY_DIR}/tobii-sesp-bridge.exe")
    add_custom_command(
        OUTPUT "${SESP_BRIDGE_EXE}"
        COMMAND "${MINGW_CC}" -O2 -Wall -Wextra -s
                -o "${SESP_BRIDGE_EXE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/sesp-bridge.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/sesp_codec.c"
                -lws2_32
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sesp-bridge.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/sesp_codec.c"
                "${CMAKE_CURRENT_SOURCE_DIR}/sesp_codec.h"
        COMMENT "Cross-compiling tobii-sesp-bridge.exe for Wine"
        VERBATIM)
    add_custom_target(sesp-bridge ALL DEPENDS "${SESP_BRIDGE_EXE}")
    install(PROGRAMS "${SESP_BRIDGE_EXE}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/tobii-faithful)
    message(STATUS "sesp bridge: ${MINGW_CC}")

elseif(TOBII_REQUIRE_BRIDGE)
    message(FATAL_ERROR
        "x86_64-w64-mingw32-gcc not found and TOBII_REQUIRE_BRIDGE is on. "
        "Install mingw64-gcc, or turn the option off for a build that does no "
        "head pose.")
else()
    message(WARNING
        "x86_64-w64-mingw32-gcc not found - the Wine SESP bridge will not be "
        "built, so head pose into Star Citizen will not work from this build. "
        "Install mingw64-gcc.")
endif()

add_library(tobii-sesp-codec STATIC sesp_codec.c)
target_include_directories(tobii-sesp-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
