# mstream-client: command-line test client for multistream (docs/phase-2-design.md, protocol/multistream.md).
# Built on /common-c, so it needs OpenSSL: MinGW/MSYS2 only (skipped elsewhere, or where OpenSSL is missing).
# Linked statically, so the test harness can copy the one executable anywhere.
if(NOT MINGW)
  return()
endif()
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL QUIET)
if(NOT OPENSSL_FOUND)
  message(STATUS "mstream-client: OpenSSL not found, skipped")
  return()
endif()

enable_language(C)

# common-c as a static library, so the client is one executable
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../common-c" "${CMAKE_BINARY_DIR}/common-c" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

add_executable(mstream-client
  src/main.cpp
  src/http_client.cpp
  src/pairing.cpp
  src/session.cpp)
# The libraries rather than OpenSSL::SSL: the imported target adds the compiler's own include directory as -isystem,
# which breaks libstdc++'s #include_next with MinGW.
target_link_libraries(mstream-client PRIVATE moonlight-common-c ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY} nlohmann_json::nlohmann_json ws2_32 crypt32)
tessera_apply_options(mstream-client)
