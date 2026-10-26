# vdd-stress.exe: virtual display add/remove stress test with stall probes (brief §4.5, Phase 1).
# run/hold need an elevated prompt (driver control interfaces); probe does not, so no UAC manifest is set.
add_executable(vdd-stress src/main.cpp src/backends.cpp src/probes.cpp)
target_link_libraries(vdd-stress PRIVATE tessera_common setupapi cfgmgr32 dwmapi winmm ole32 user32 gdi32)
tessera_apply_options(vdd-stress)
if(MINGW)
  target_link_options(vdd-stress PRIVATE -municode)
endif()
