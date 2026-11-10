xe_test_suite(xenia-vfs-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS xenia-vfs mbedtls
)

# The library catalog is pure metadata/filesystem code, with no GUI or kernel.
target_sources(xenia-vfs-tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src/xenia/app/game_library.cc
  ${PROJECT_SOURCE_DIR}/src/xenia/app/title_metadata_cache.cc
)
