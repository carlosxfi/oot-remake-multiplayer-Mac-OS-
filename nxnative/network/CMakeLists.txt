cmake_minimum_required(VERSION 3.24)
project(nxnative_coop_network LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${repo}/cmake/enet.cmake")
include("${repo}/cmake/monocypher.cmake")
find_package(Threads REQUIRED)

add_library(nxnative_shared STATIC
  "${repo}/coop/host/src/game_memory.cpp"
  "${repo}/coop/host/src/player_reader.cpp"
  "${repo}/coop/host/src/sampler.cpp"
  "${repo}/coop/host/src/game_flags.cpp"
  "${repo}/coop/host/src/game_flag_events.cpp"
  "${repo}/coop/host/src/game_items.cpp"
  "${repo}/coop/host/src/flags.cpp"
  "${repo}/coop/host/src/map_drop_log.cpp"
  "${repo}/coop/host/src/one_way_keeper.cpp"
  "${repo}/coop/protocol/src/codec.cpp"
  "${repo}/coop/protocol/src/join_password.cpp"
  "${repo}/coop/protocol/src/mod_world.cpp"
  "${repo}/coop/protocol/src/race.cpp"
  "${repo}/coop/protocol/src/official_mark.cpp"
  "${repo}/coop/protocol/src/traffic_crypto.cpp"
  "${repo}/coop/net/src/client.cpp"
  "${repo}/coop/net/src/clock.cpp"
  "${repo}/coop/presence/src/presence.cpp"
)
target_include_directories(nxnative_shared PUBLIC
  "${repo}/coop/host/include" "${repo}/coop/protocol/include" "${repo}/coop/net/include"
  "${repo}/coop/presence/include")
target_link_libraries(nxnative_shared PUBLIC enet monocypher Threads::Threads)

add_library(nxnative_loopback STATIC
  loopback.cpp remote_peer.cpp
  "${repo}/server/enet_server.cpp" "${repo}/server/session.cpp"
  "${repo}/server/world.cpp" "${repo}/server/test_delay.cpp")
target_include_directories(nxnative_loopback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${repo}/server")
target_link_libraries(nxnative_loopback PUBLIC nxnative_shared)

if(APPLE)
  enable_language(ASM OBJCXX)
  # DYLD_INSERT_LIBRARIES requires MH_DYLIB; MODULE emits MH_BUNDLE on macOS.
  add_library(coop_nxnative_probe SHARED
    "${repo}/nxnative/probe/link_position_probe.cpp"
    "${repo}/nxnative/probe/game_call.cpp"
    "${repo}/nxnative/probe/game_call.S"
    "${repo}/nxnative/probe/multiplayer_menu.mm"
    "${repo}/lichii/hook/coop_colors.cpp")
  target_compile_definitions(coop_nxnative_probe PRIVATE COOP_NXNATIVE_NETWORK=1)
  set_property(TARGET coop_nxnative_probe PROPERTY OBJCXX_STANDARD 20)
  target_compile_options(coop_nxnative_probe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
  target_link_libraries(coop_nxnative_probe PRIVATE nxnative_loopback "${APPKIT_FRAMEWORK}")
  # The existing run.sh injects this path for both build variants.
  set_target_properties(coop_nxnative_probe PROPERTIES
    SUFFIX ".dylib" LIBRARY_OUTPUT_DIRECTORY "${repo}/nxnative/probe/build")
endif()

enable_testing()
add_executable(nxnative_network_check loopback_test.cpp)
target_link_libraries(nxnative_network_check PRIVATE nxnative_loopback)
if(MSVC)
  target_compile_options(nxnative_network_check PRIVATE /UNDEBUG)
else()
  target_compile_options(nxnative_shared PRIVATE -Wall -Wextra -Werror)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(nxnative_shared PRIVATE -Wno-error=maybe-uninitialized)
  endif()
  target_compile_options(nxnative_loopback PRIVATE -Wall -Wextra -Werror)
  target_compile_options(nxnative_network_check PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME nxnative_network_check COMMAND nxnative_network_check)
set_tests_properties(nxnative_network_check PROPERTIES TIMEOUT 15)

# Exercise dyld itself, not just the static libraries used by the network test.
# Use our own executable: protected system programs may strip DYLD variables.
if(APPLE)
  add_executable(nxnative_dylib_load_check "${repo}/nxnative/probe/module_detection_test.cpp")
  target_compile_options(nxnative_dylib_load_check PRIVATE -UNDEBUG -Wall -Wextra -Werror)
  add_dependencies(nxnative_dylib_load_check coop_nxnative_probe)
  add_test(NAME nxnative_dylib_load_check
    COMMAND "${CMAKE_COMMAND}" -E env
      "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:coop_nxnative_probe>"
      "$<TARGET_FILE:nxnative_dylib_load_check>")
  set_tests_properties(nxnative_dylib_load_check PROPERTIES TIMEOUT 10)
endif()

add_executable(nxnative_remote_check remote_peer_test.cpp)
target_include_directories(nxnative_remote_check PRIVATE "${repo}/server")
target_link_libraries(nxnative_remote_check PRIVATE nxnative_loopback)
if(MSVC)
  target_compile_options(nxnative_remote_check PRIVATE /UNDEBUG)
else()
  target_compile_options(nxnative_remote_check PRIVATE -UNDEBUG -Wall -Wextra -Werror)
endif()
add_test(NAME nxnative_remote_check COMMAND nxnative_remote_check)
set_tests_properties(nxnative_remote_check PROPERTIES TIMEOUT 20)
