cmake_minimum_required(VERSION 3.20)
project(PolySparQPlusPlus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Executables do not export a C++ ABI. Hide template instantiations to prevent
# libpqxx type_name globals from interposing with the shared library and being
# destroyed twice at exit (caught by the candle/database-linked regression).
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
    -fno-omit-frame-pointer
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX     REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(SIMDJSON    REQUIRED IMPORTED_TARGET simdjson)
pkg_check_modules(NOTCURSES   REQUIRED IMPORTED_TARGET notcurses++)
pkg_check_modules(CURL        REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SECP        REQUIRED IMPORTED_TARGET libsecp256k1)
pkg_check_modules(FMT         REQUIRED IMPORTED_TARGET fmt)
pkg_check_modules(SPDLOG      REQUIRED IMPORTED_TARGET spdlog)
pkg_check_modules(SODIUM      REQUIRED IMPORTED_TARGET libsodium)

# Boost: header-only Beast/Asio in 1.90 (boost_system is now header-only).
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include_directories(BEFORE src/third_party)

add_library(vp_core STATIC
    src/core/clock.cpp
    src/config/config.cpp
    src/db/pg.cpp
    src/market/orderbook.cpp
    src/net/ws_client.cpp
    src/net/http_client.cpp
    src/feeds/binance.cpp
    src/feeds/coinbase_ws.cpp
    src/feeds/polymarket_md.cpp
    src/feeds/polymarket_user_ws.cpp
    src/feeds/polymarket_rest.cpp
    src/feeds/polymarket_rtds.cpp
    src/exec/paper.cpp
    src/exec/positions.cpp
    src/exec/live.cpp
    src/engine/rules.cpp
    src/engine/indicators.cpp
    src/engine/candle_store.cpp
    src/engine/trade_flow_buffer.cpp
    src/engine/pm_flow_buffer.cpp
    src/engine/tick_buffer.cpp
    src/engine/scalp_manager.cpp
    src/engine/window_bots.cpp
    src/engine/predictor.cpp
    src/risk/risk_manager.cpp
    src/sign/eip712.cpp
    src/sign/hmac_auth.cpp
    src/llm/advisor.cpp
    src/tui/tui.cpp
)
target_include_directories(vp_core PUBLIC src)
target_link_libraries(vp_core PUBLIC
    PkgConfig::LIBPQXX
    PkgConfig::SIMDJSON
    PkgConfig::NOTCURSES
    PkgConfig::CURL
    PkgConfig::SECP
    PkgConfig::FMT
    PkgConfig::SPDLOG
    PkgConfig::SODIUM
    OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads
)

add_executable(polypulse-forecast src/engine/forecast_cli.cpp)
target_link_libraries(polypulse-forecast PRIVATE PkgConfig::SIMDJSON PkgConfig::FMT)

# Offline threshold sweeper for the CB-lead strategy over recorded ticks.
# Links vp_core (not bare libpqxx) — direct pqxx template instantiation in a
# standalone binary hits the known libpqxx type_name double-free at exit.
add_executable(polypulse-replay src/engine/replay_cli.cpp)
target_link_libraries(polypulse-replay PRIVATE vp_core)

add_executable(polypulse src/main.cpp)
target_link_libraries(polypulse PRIVATE vp_core)

add_executable(polypulse-collector src/collector/main.cpp)
target_link_libraries(polypulse-collector PRIVATE vp_core)

# C++ WebSocket fan-out proxy (replaces scripts/feed_proxy.py). Reuses vp::WsClient
# for the TLS upstreams; serves the local fleet over a plain ws:// loopback server.
add_executable(polypulse-feedproxy src/proxy/feed_proxy.cpp)
target_link_libraries(polypulse-feedproxy PRIVATE vp_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test-persistence tests/test_persistence.cpp)
    target_link_libraries(test-persistence PRIVATE vp_core)
    add_test(NAME persistence-atomic COMMAND test-persistence)
    set_tests_properties(persistence-atomic PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test-forecast tests/test_forecast.cpp)
    target_link_libraries(test-forecast PRIVATE vp_core)
    add_test(NAME forecast-reference-candles COMMAND test-forecast)
    add_executable(test-predictor tests/test_predictor.cpp)
    target_link_libraries(test-predictor PRIVATE vp_core)
    add_test(NAME predictor-policy COMMAND test-predictor)
    add_executable(test-rules tests/test_rules.cpp)
    target_link_libraries(test-rules PRIVATE vp_core)
    add_test(NAME strategy-rules COMMAND test-rules)
    add_executable(test-maker tests/test_maker.cpp)
    target_link_libraries(test-maker PRIVATE vp_core)
    add_test(NAME maker-lifecycle COMMAND test-maker)
    add_executable(test-replay tests/test_replay.cpp)
    target_link_libraries(test-replay PRIVATE vp_core)
    add_test(NAME replay-window COMMAND test-replay)
    add_executable(test-risk tests/test_risk.cpp)
    target_link_libraries(test-risk PRIVATE vp_core)
    add_test(NAME risk-checks COMMAND test-risk)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
message(STATUS "PolySparQ++ build: ${CMAKE_BUILD_TYPE}")
