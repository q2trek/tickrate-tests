/* Probe the engine's normal loader and keep the real packet path end to end. */
#include <sys/mman.h>
#include "shared/unlag.h"

typedef enum {
    LOAD_NORMAL, LOAD_SHORT, LOAD_VERSION, LOAD_NO_GET, LOAD_NO_REPLY, LOAD_NO_DIV,
    LOAD_UNLAG_ONLY, LOAD_FRAMEDIV_ONLY, LOAD_API3,
} display_load_t;

static display_load_t display_load;
static bool display_observe_load;
static int display_queried_div, display_queried_unlag, display_calls, display_changed;
static game_entry_ex_t display_entry;
static const game_import_ex_t *display_engine_import;
static const game_export_ex_t *display_module_export;
static qboolean (*display_customize)(edict_t *, edict_t *, customize_entity_t *);
static edict_t *display_watched;

static void *display_get_extension(const char *name)
{
    if (!strcmp(name, OPENTDM60_FRAMEDIV_V1)) {
        static opentdm60_framediv_v1_t empty;
        display_queried_div++;
        if (display_load == LOAD_NO_DIV)
            return &empty;
        if (display_load == LOAD_NO_REPLY || display_load == LOAD_UNLAG_ONLY)
            return NULL;
    }
    if (!strcmp(name, UNLAG_API_V1)) {
        display_queried_unlag++;
        if (display_load == LOAD_NO_REPLY || display_load == LOAD_FRAMEDIV_ONLY)
            return NULL;
    }
    return display_engine_import->GetExtension(name);
}

static qboolean display_spy(edict_t *receiver, edict_t *ent, customize_entity_t *out)
{
    qboolean changed = display_customize(receiver, ent, out);
    if (ent == display_watched) {
        display_calls++;
        display_changed += changed;
    }
    return changed;
}

static const game_export_ex_t *display_load_entry(const game_import_ex_t *import)
{
    if (!display_observe_load)
        return display_entry(import);
    display_queried_div = display_queried_unlag = 0;
    display_module_export = NULL;
    if (display_load == LOAD_API3)
        return NULL;

    display_engine_import = import;
    static game_import_ex_t offered;
    offered = *import;
    offered.GetExtension = display_load == LOAD_NO_GET ? NULL : display_get_extension;
    if (display_load == LOAD_VERSION)
        offered.apiversion = 0;
    if (display_load == LOAD_SHORT) {
        // The field at structsize is outside the accessible page.
        size_t page = sysconf(_SC_PAGESIZE);
        char *mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Q_assert(mapping != MAP_FAILED);
        Q_assert(!mprotect(mapping + page, page, PROT_NONE));
        offered.structsize = offsetof(game_import_ex_t, GetExtension);
        void *short_import = mapping + page - offered.structsize;
        memcpy(short_import, &offered, offered.structsize);
        display_module_export = display_entry(short_import);
        munmap(mapping, 2 * page);
    } else {
        display_module_export = display_entry(&offered);
    }
    if (!display_module_export || !display_module_export->CustomizeEntityToClient)
        return display_module_export;

    static game_export_ex_t observed;
    observed = *display_module_export;
    display_customize = observed.CustomizeEntityToClient;
    observed.CustomizeEntityToClient = display_spy;
    return &observed;
}

void *__real_Sys_GetProcAddress(void *handle, const char *name);
void *__wrap_Sys_GetProcAddress(void *handle, const char *name)
{
    void *symbol = __real_Sys_GetProcAddress(handle, name);
    if (!strcmp(name, "GetGameAPIEx") && getenv("CL_GRID_DISPLAY")) {
        display_projectile = __real_Sys_GetProcAddress(handle, "display_projectile");
        Q_assert(display_projectile);
        display_phase = __real_Sys_GetProcAddress(handle, "display_phase");
        Q_assert(display_phase);
        display_angles = __real_Sys_GetProcAddress(handle, "display_angles");
        Q_assert(display_angles);
        display_move = __real_Sys_GetProcAddress(handle, "display_move");
        Q_assert(display_move);
        display_freeze = __real_Sys_GetProcAddress(handle, "display_freeze");
        Q_assert(display_freeze);
        display_in_timeout = __real_Sys_GetProcAddress(handle, "display_in_timeout");
        Q_assert(display_in_timeout);
        display_schedule_resume = __real_Sys_GetProcAddress(handle, "display_schedule_resume");
        Q_assert(display_schedule_resume);
        display_free_flight_frame = __real_Sys_GetProcAddress(handle, "display_free_flight_frame");
        Q_assert(display_free_flight_frame);
        display_grid_frame = __real_Sys_GetProcAddress(handle, "display_grid_frame");
        Q_assert(display_grid_frame);
        display_fire = __real_Sys_GetProcAddress(handle, "display_fire");
        Q_assert(display_fire);
        display_flight_trace = __real_Sys_GetProcAddress(handle, "display_flight_trace");
        Q_assert(display_flight_trace);
        display_flight_trace_active = __real_Sys_GetProcAddress(handle, "display_flight_trace_active");
        Q_assert(display_flight_trace_active);
        display_age = __real_Sys_GetProcAddress(handle, "display_age");
        Q_assert(display_age);
        display_remove = __real_Sys_GetProcAddress(handle, "display_remove");
        Q_assert(display_remove);
        display_partial_push = __real_Sys_GetProcAddress(handle, "display_partial_push");
        Q_assert(display_partial_push);
        if (symbol && display_observe_load) {
            display_entry = symbol;
            return display_load_entry;
        }
    }
    return symbol;
}

static void display_check_load(bool want_display, bool query_div, bool query_unlag)
{
    client_t *client = h_sv_client();
    CHECK(client, "connected after game load");
    if (!client)
        return;
    CHECK(display_queried_div == query_div && display_queried_unlag == query_unlag,
          "extension requests: frame divisor %d, unlag %d (expected %d / %d)", display_queried_div,
          display_queried_unlag, query_div, query_unlag);
    CHECK(sv_game_requested_framediv == want_display, "engine frame divisor request flag: %d",
          sv_game_requested_framediv);
    framediv_negotiate(client, h_rate);
    const float from[] = { 64, 96, 128 }, to[] = { 124, 96, 128 };
    display_watched = display_projectile(from, to, 0);
    display_calls = display_changed = 0;
    display_write(client, cl.frame.number + 100, -1, true);
    CHECK(display_calls == want_display && display_changed == want_display,
          "snapshot customization: callbacks %d, substitutions %d", display_calls, display_changed);
    vec3_t want = { want_display ? 74 : to[0], to[1], to[2] };
    display_check(&cl_entities[display_watched->s.number], want, "decoded after loading the game");
    display_watched = NULL;
}

static void grid_load_case(display_load_t mode, bool customize, bool query_div, bool query_unlag)
{
    if (h_rate != 60) {
        h_skipped = true;
        puts("  SKIP load matrix uses a fast recipient on a 60 Hz server");
        return;
    }
    display_observe_load = true;
    display_load = mode;
    if (h_join())
        display_check_load(customize, query_div, query_unlag);
}

static void grid_load_short(void) { grid_load_case(LOAD_SHORT, false, false, false); }
static void grid_load_version(void) { grid_load_case(LOAD_VERSION, false, false, false); }
static void grid_load_no_get(void) { grid_load_case(LOAD_NO_GET, false, false, false); }
static void grid_load_no_reply(void) { grid_load_case(LOAD_NO_REPLY, false, true, true); }
static void grid_load_no_div(void) { grid_load_case(LOAD_NO_DIV, false, true, true); }
static void grid_load_unlag_only(void) { grid_load_case(LOAD_UNLAG_ONLY, false, true, true); }
static void grid_load_framediv_only(void) { grid_load_case(LOAD_FRAMEDIV_ONLY, true, true, true); }
static void grid_load_api3(void) { grid_load_case(LOAD_API3, false, false, false); }

static void grid_load_production(void)
{
    grid_load_case(LOAD_NORMAL, !getenv("CL_DISPLAY_LEGACY"), !getenv("CL_DISPLAY_LEGACY"),
                   !getenv("CL_DISPLAY_NO_UNLAG"));
}

static void grid_load_reload(void)
{
    if (h_rate != 60) {
        h_skipped = true;
        puts("  SKIP load matrix uses a fast recipient on a 60 Hz server");
        return;
    }

    display_observe_load = true;
    if (!h_join())
        return;
    display_check_load(true, true, true);
    SV_ShutdownGameProgs();
    CHECK(!gex && !sv_game_requested_framediv,
          "unloading the game clears the engine request flag");

    // Re-enter the stock loader twice with different available extensions.
    display_load = LOAD_UNLAG_ONLY;
    h_exec(va("map %s force", h_map()));
    h_run(300);
    bool active = h_wait_active(5000);
    CHECK(active, "active after first reload");
    if (!active)
        return;
    display_check_load(false, true, true);

    display_load = LOAD_NORMAL;
    h_exec(va("map %s force", h_map()));
    h_run(300);
    active = h_wait_active(5000);
    CHECK(active, "active after second reload");
    if (active)
        display_check_load(true, true, true);
}

/* Optional loopback check against a dedicated binary, using a normal shot. */
static void grid_load_peer(void)
{
    CHECK(getenv("CL_CONNECT"), "dedicated loopback address supplied");
    if (!getenv("CL_CONNECT") || !h_join())
        return;

    if (getenv("CL_DISPLAY_OPENTDM")) {
        h_exec("cmd team a");
    } else {
        h_exec("+attack");
        h_run(150);
        h_exec("-attack");
    }
    h_run(350);
    h_exec("cmd give rocket launcher");
    h_exec("cmd give rockets 50");
    h_exec("cmd use rocket launcher");
    h_run(750);
    cl.viewangles[PITCH] = -30;
    h_exec("+attack");

    int previous = cl.frame.number, moving = 0, steps = 0;
    bool grid = getenv("CL_DISPLAY_LEGACY") != NULL;
    float expected = grid ? 65 : 650.0f / H_CRATE;
    // The difference between two packed coordinates can be off by one quantum per axis.
    float tolerance = sqrtf(3) / 8;
    for (int msec = 0; msec < 3000 && moving < 6; msec++) {
        h_tick(1);
        if (!cl.frame.valid || cl.frame.number == previous)
            continue;
        previous = cl.frame.number;
        for (int i = 0; i < cl.frame.numEntities; i++) {
            const centity_state_t *state = &cl.entityStates[(cl.frame.firstEntity + i) & PARSE_ENTITIES_MASK];
            if (!(state->effects & EF_ROCKET))
                continue;
            const centity_t *shot = &cl_entities[state->number];
            if (!(shot->prev.effects & EF_ROCKET))
                continue;
            vec3_t delta;
            VectorSubtract(shot->current.origin, shot->prev.origin, delta);
            float distance = VectorLength(delta);
            if (distance > 0) {
                CHECK(fabsf(distance - expected) <= tolerance,
                      "dedicated rocket snapshot %d advances %.3f, expected %.3f at %d Hz",
                      moving + 1, distance, expected, H_CRATE);
                steps += fabsf(distance - expected) <= tolerance;
                moving++;
            }
        }
    }
    h_exec("-attack");
    CHECK(moving == 6 && steps == moving,
          "dedicated peer decoded six rocket steps at the agreed rate (%d/%d)", steps, moving);
}
