# recover.exe: restores journaled host state (brief §4.2).
add_executable(recover src/recover.cpp)
target_link_libraries(recover PRIVATE tessera_common)
tessera_apply_options(recover)

# topoctl.exe: inspect the display layout and make deliberate journaled changes (tests/dev only).
add_executable(topoctl src/topoctl.cpp)
target_link_libraries(topoctl PRIVATE tessera_common user32)
tessera_apply_options(topoctl)

if(MINGW)
  # wmain entry point
  target_link_options(recover PRIVATE -municode)
  target_link_options(topoctl PRIVATE -municode)
endif()
