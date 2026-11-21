# Sources used by both the Mac dylib and the portable sampler check.
# The caller supplies the absolute repository root in repo.
shared_sources() {
  set -- \
    "$repo/coop/host/src/game_memory.cpp" \
    "$repo/coop/host/src/player_reader.cpp" \
    "$repo/coop/host/src/sampler.cpp" \
    "$repo/coop/host/src/game_flags.cpp" \
    "$repo/coop/host/src/game_flag_events.cpp" \
    "$repo/coop/host/src/game_items.cpp" \
    "$repo/coop/host/src/flags.cpp" \
    "$repo/coop/host/src/map_drop_log.cpp" \
    "$repo/coop/host/src/one_way_keeper.cpp" \
    "$repo/coop/protocol/src/codec.cpp"
  # Invoke the requested compiler function with correctly quoted paths.
  compile_shared "$@"
}
