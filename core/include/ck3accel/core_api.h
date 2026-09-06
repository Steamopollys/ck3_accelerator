#pragma once
#include <stdint.h>
#include <stddef.h>   /* offsetof (tick-epoch capability check) */
#include <ck3accel/log_level.h>
#include <ck3accel/version_info.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CK3ACCEL_ABI_VERSION   1u
#define CK3ACCEL_PLUGIN_MAGIC  0x434B3341u   /* 'CK3A' */

/* Session-mode bit flags (CK3AccelPluginInfo.mode_flags & SessionContext mode). */
#define CK3ACCEL_MODE_SP           0x1u
#define CK3ACCEL_MODE_IRONMAN      0x2u
#define CK3ACCEL_MODE_MULTIPLAYER  0x4u

/* Opaque hook handle; concrete type defined by the hook engine. */
struct HookHandle;
typedef uint32_t HookSetId;

/* Shared trigger-evaluator service. The core hooks the evaluator once and runs registered handlers
   as a chain ending in the original, so several plugins can observe or override triggers at once
   (which MinHook can't do by each hooking the function directly). A handler either returns its own
   result or calls next() to continue the chain. Higher-priority handlers run first (outermost). */
typedef char (*ck3accel_trigger_next)(void* node, void* ctx, unsigned char skip, void* next_ctx);
typedef char (*ck3accel_trigger_handler)(void* node, void* ctx, unsigned char skip,
                                         ck3accel_trigger_next next, void* next_ctx, void* user);

/* Host -> plugin services. APPEND-ONLY; never reorder/repurpose fields. */
typedef struct CoreApi {
    uint32_t struct_size;     /* = sizeof(CoreApi), set by core */
    uint32_t abi_version;     /* = CK3ACCEL_ABI_VERSION         */
    void  (*log)(int level, const char* message);
    const struct VersionInfo_C* (*game_version)(void);
    int   (*is_kill_switch_active)(void);
    void  (*report_metric)(const char* name, double value);
    void* (*scan)(const char* signature);     /* matched address or null */
    struct HookHandle* (*install_hook)(HookSetId set, void* target,
                                       void* detour, void** trampoline_out);

    /* --- shared tick-epoch service (appended) -------------------------------------------------
       core owns the effect executor + UpdateTurnTick hooks and hands out the two signals a cache
       needs, so caches coexist (MinHook can't share a hooked function). gate on
       ck3accel_has_tick_epoch() below before touching these. */
    int      (*ensure_tick_epoch)(void); /* lazily install the shared hooks; 1 = live, 0 = unavailable */
    uint32_t (*tick_epoch)(void);        /* epoch, bumped on every effect + tick boundary */
    int      (*in_tick)(void);           /* > 0 while inside the day-tick simulation */
    void     (*bump_epoch)(void);        /* force a bump (periodic safety flush); no-op if not live */

    /* --- control-panel registry (appended) ---------------------------------------------------
       a plugin publishes a runtime toggle + live counters; an overlay plugin reads them to show
       status and flip plugins on/off. gate on ck3accel_has_panels() below. */
    void (*register_panel)(const struct CK3AccelPanel* panel);  /* plugin -> core (core copies it) */
    int  (*panel_count)(void);
    const struct CK3AccelPanel* (*panel_at)(int index);

    /* --- shared trigger-evaluator service (appended) -----------------------------------------
       gate on ck3accel_has_trigger_service() below. */
    int  (*ensure_trigger_service)(void);                        /* lazily install the hook; 1 = live */
    void (*register_trigger_handler)(ck3accel_trigger_handler h, void* user, int priority);
} CoreApi;

/* Published by a plugin via register_panel. The pointers reference plugin globals (static lifetime),
   so the core keeps a shallow copy. `enabled` is a plain int the plugin checks each call; a reader
   writes it to toggle. stat_values point at u64 counters, read for display. */
typedef struct CK3AccelPanel {
    uint32_t    struct_size;
    const char* name;
    int*        enabled;                       /* runtime 1/0; NULL if the plugin has no toggle */
    int         stat_count;                    /* 0..4 */
    const char* stat_labels[4];
    const unsigned long long* stat_values[4];
} CK3AccelPanel;

/* answers "does this core have the tick-epoch service?". bound is offsetof(last
   field)+sizeof rather than sizeof(CoreApi): sizeof grows when a later field lands
   and would then miss the service on any core built before it. call in Init; stay
   inert on 0. */
static inline int ck3accel_has_tick_epoch(const CoreApi* h) {
    return h != 0 &&
           h->struct_size >= (uint32_t)(offsetof(CoreApi, bump_epoch) + sizeof(void*)) &&
           h->ensure_tick_epoch != 0 && h->tick_epoch != 0 &&
           h->in_tick != 0 && h->bump_epoch != 0;
}

/* Same idea for the control-panel registry. */
static inline int ck3accel_has_panels(const CoreApi* h) {
    return h != 0 &&
           h->struct_size >= (uint32_t)(offsetof(CoreApi, panel_at) + sizeof(void*)) &&
           h->register_panel != 0 && h->panel_count != 0 && h->panel_at != 0;
}

/* ...and for the shared trigger service. */
static inline int ck3accel_has_trigger_service(const CoreApi* h) {
    return h != 0 &&
           h->struct_size >= (uint32_t)(offsetof(CoreApi, register_trigger_handler) + sizeof(void*)) &&
           h->ensure_trigger_service != 0 && h->register_trigger_handler != 0;
}

/* C-friendly mirror of ck3accel::VersionInfo. */
typedef struct VersionInfo_C {
    int status;                  /* 0=KnownTested,1=KnownUntested,2=Unknown */
    const char* version;
    unsigned int pe_timestamp;
    const char* text_sha256;
    const char* const* auto_disable;
    int auto_disable_count;
} VersionInfo_C;

/* Static, immutable metadata the core reads via CK3Accel_Query BEFORE Init. */
typedef struct CK3AccelPluginInfo {
    uint32_t    struct_size;      /* = sizeof(CK3AccelPluginInfo), set by plugin; host clamps field reads to min(struct_size, sizeof) */
    uint32_t    magic;            /* == CK3ACCEL_PLUGIN_MAGIC          */
    uint32_t    required_abi;     /* core refuses if > abi_version     */
    const char* name;             /* must match [plugins] allowlist key */
    const char* semver;
    const char* min_game_version; /* or "any" */
    const char* max_game_version; /* or "any" */
    uint32_t    mode_flags;       /* OR of CK3ACCEL_MODE_* supported    */
} CK3AccelPluginInfo;

/* Passed to CK3Accel_Init; carries the plugin's pre-assigned hook set. */
typedef struct CK3AccelRegistrar {
    uint32_t  struct_size;        /* = sizeof(CK3AccelRegistrar) */
    HookSetId hook_set;
} CK3AccelRegistrar;

/* Plugin export signatures (each plugin DLL exports CK3Accel_Query + CK3Accel_Init).
   host_abi_version is passed by-value (width-stable uint32_t). */
typedef const CK3AccelPluginInfo* (*CK3Accel_Query_t)(uint32_t host_abi_version);
typedef int (*CK3Accel_Init_t)(const CoreApi* host, struct CK3AccelRegistrar* reg);

#ifdef __cplusplus
}
#endif
