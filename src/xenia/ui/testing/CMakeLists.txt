xe_test_suite(xenia-ui-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui mbedtls
    xenia-apu xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-hid xenia-hid-keyboard xenia-hid-portal xenia-kernel xenia-ui xenia-patcher
)
if(XE_TARGET_X86_64 AND TARGET xenia-ui-tests)
  target_link_libraries(xenia-ui-tests PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64 AND TARGET xenia-ui-tests)
  target_link_libraries(xenia-ui-tests PRIVATE xenia-cpu-backend-a64)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET xenia-ui-tests)
  target_link_libraries(xenia-ui-tests PRIVATE xenia-apu-sdl)
endif()
