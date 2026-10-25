# tessera-agent.exe: user-session helper (Phase 0: emergency restore hotkey).
add_executable(tessera-agent WIN32 src/agent.cpp)
target_link_libraries(tessera-agent PRIVATE tessera_common user32)
tessera_apply_options(tessera-agent)
if(MINGW)
  target_link_options(tessera-agent PRIVATE -municode)
endif()
