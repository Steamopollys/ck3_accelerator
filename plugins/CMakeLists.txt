# Plugin DLLs. Each is a standalone SHARED lib, links nothing from core, talks
# to the host through the C ABI in <ck3accel/core_api.h>.
add_subdirectory(sample_noop)
# _dev_decompress_probe: dev-only save-decompression probe. Not shipped.
add_subdirectory(_dev_decompress_probe)
# accel_save_load: shipping plugin (libdeflate gamestate decompression).
add_subdirectory(accel_save_load)
# _dev_sampler: dev-only sampling profiler. Not shipped.
add_subdirectory(_dev_sampler)
# _dev_eval_probe: dev-only trigger/dispatch/freeze probe. Not shipped.
add_subdirectory(_dev_eval_probe)
# _dev_trigger_repeat: dev-only trigger repeat/consistency probe. Not shipped.
add_subdirectory(_dev_trigger_repeat)
# accel_family_lists: shipping plugin (O(N) family-list dedup).
add_subdirectory(accel_family_lists)
# _dev_attrib_probe: dev-only trigger-node / stack attribution probe. Not shipped.
add_subdirectory(_dev_attrib_probe)
# accel_ui_trigger_cache: shipping plugin (per-frame UI trigger-result cache).
add_subdirectory(accel_ui_trigger_cache)
# accel_family_cache: opt-in plugin (per-frame family-list memoization).
add_subdirectory(accel_family_cache)
# _dev_tick_cache_probe: observe-only tick trigger-cache viability probe. Not shipped.
add_subdirectory(_dev_tick_cache_probe)
# accel_tick_cache: opt-in plugin (whitelisted pure-trigger cache in the tick).
add_subdirectory(accel_tick_cache)

# accel_demo: opt-in gameplay-change DEMO (not shipped).
add_subdirectory(_demo_unleashed)

# accel_demo_overlay: opt-in ImGui overlay demo (not shipped).
add_subdirectory(_demo_overlay)
