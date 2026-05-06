cmake_minimum_required(VERSION 3.28)

# Shared helpers for test-tree binaries: the JSON log line abstraction,
# the random/wide log generators, and the Asio-based network log
# clients. Intentionally Catch2-free so the standalone `log_generator`
# console app can link it without pulling in the unit-test framework.
add_library(test_common STATIC src/json_log_line.cpp src/log_generator.cpp src/network_log_client.cpp)

target_include_directories(test_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(test_common PUBLIC glaze::glaze date::date asio::asio PRIVATE project_warnings)

# Match loglib's TLS gate so the network_log_client TLS branches
# compile in/out together; OpenSSL stays PRIVATE because no
# test_common public header exposes any OpenSSL type.
if(LOGLIB_NETWORK_TLS)
    target_compile_definitions(test_common PUBLIC LOGLIB_HAS_TLS)
    target_link_libraries(test_common PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
