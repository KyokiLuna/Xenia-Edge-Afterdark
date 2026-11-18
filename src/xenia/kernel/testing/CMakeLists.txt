xe_test_suite(xenia-kernel-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui mbedtls
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-hid-portal xenia-kernel xenia-ui xenia-patcher
)
if(XE_TARGET_X86_64 AND TARGET xenia-kernel-tests)
  target_link_libraries(xenia-kernel-tests PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64 AND TARGET xenia-kernel-tests)
  target_link_libraries(xenia-kernel-tests PRIVATE xenia-cpu-backend-a64)
endif()
# xenia-apu pulls in xenia-apu-sdl on Linux (see src/xenia/apu/CMakeLists.txt).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET xenia-kernel-tests)
  target_link_libraries(xenia-kernel-tests PRIVATE xenia-apu-sdl)
endif()

# The explicit genuine-library integration case also checks the real kernel
# license setting and PNG decoder. Reuse the same pure metadata sources as
# xenia-vfs-tests; these files do not start the frontend or an emulated title.
if(TARGET xenia-kernel-tests)
  target_sources(xenia-kernel-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/app/game_library.cc
    ${PROJECT_SOURCE_DIR}/src/xenia/app/title_metadata_cache.cc
  )
endif()
