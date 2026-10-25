# tessera_common: code shared by the host (MinGW), the helpers and the tools (MSVC).
# Keep it to plain Win32 + C++20 + nlohmann_json so it compiles with both toolchains.
# This file is also added from host/tools/CMakeLists.txt, so it must not depend on the root project.
if(NOT COMMAND tessera_apply_options)
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/tessera_compile_options.cmake")
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.11 CONFIG REQUIRED)
endif()

add_library(tessera_common STATIC
  src/crypto.cpp
  src/display_topology.cpp
  src/state_journal.cpp
  src/state_restore.cpp
  src/win32.cpp)
target_include_directories(tessera_common PUBLIC include)
target_link_libraries(tessera_common
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE bcrypt shell32 ole32 advapi32 user32)
tessera_apply_options(tessera_common)
