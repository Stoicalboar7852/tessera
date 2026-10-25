# vdd-install.exe: create/remove/query the root-enumerated Tessera virtual display device (run elevated).
add_executable(vdd-install src/vdd_install.cpp)
target_include_directories(vdd-install PRIVATE "${PROJECT_SOURCE_DIR}/drivers/vdd/include")
target_link_libraries(vdd-install PRIVATE tessera_common setupapi newdev cfgmgr32)
tessera_apply_options(vdd-install)
if(MSVC)
  # Request elevation: installing drivers needs administrator rights.
  target_link_options(vdd-install PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
elseif(MINGW)
  target_link_options(vdd-install PRIVATE -municode)
endif()
