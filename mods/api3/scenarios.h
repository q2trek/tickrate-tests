/*
 * T09-T15 against the mod module. Expected numbers are the
 * original 10 Hz contract (weapon frame tables, hurt/crush timestamps),
 * derived in the comments. Scenarios express it in game time (h_rate
 * ticks = 1 s, h_div ticks = one 100 ms weapon step), so they also run at
 * HARNESS_RATE=60; at 10 Hz they reduce to the original numbers.
 */

/* idle world ticks until the frame phase (h_frames mod h_div) is k */
static void h_phase(edict_t *p, int k)
{
    while (h_frames % h_div != k) h_tick(p, 0, 1);
}

static void sc_smoke(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    CHECK(p->inuse && h_health(p) == 100, "player spawned with 100 health (got %d)", h_health(p));
}

/* The engine rounds sv_fps down to a multiple of 10 before the game starts
 * (engine/scheduler t01_rate_round); a module that still sees 25 refuses it
 * instead of running a world clock the engine does not keep */
static void sc_t01_rate_refuse(void)
{
    jmp_buf outer;
    memcpy(outer, h_abort, sizeof(outer));
    imp_cvar("sv_fps", "25", 0);
    imp_cvar("sv_features", va("%d", GMF_VARIABLE_FPS), 0);
    int refused = 0;
    if (setjmp(h_abort))
        refused = 1;
    else {
        h_scene_base();
        h_start(h_ents);
    }
    memcpy(h_abort, outer, sizeof(outer));
#ifdef HARNESS_OPENTDM
    const char *want = "Invalid server FPS";
#else
    const char *want = "Invalid value '25' for 'sv_fps' cvar";
#endif
    CHECK(refused && !strcmp(h_errmsg, want), "sv_fps 25: the module refuses to start (error '%s')",
          refused ? h_errmsg : "none");
}

/* The usercmd schedules (h_tick_sched): over any second (h_rate ticks)
 * the commands last 1000 ms, at 60 Hz every tick command 16 or 17 ms; the
 * world ticks without a ClientThink are the ones the schedule leaves out */
static void sc_t04_cmd_schedule(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    const int idle[4] = { 0, h_rate > 30 ? h_rate - 30 : 0, h_rate - 10, h_rate - h_rate / 5 };
    for (int s = 0; s < 4; s++) {
        h_sched = s;
        for (int w = 0; w < 3; w++) {      /* three consecutive seconds, any phase */
            int empty = 0, n0 = h_cmds, m0 = h_cmd_msec;
            h_cmd_min = 1000; h_cmd_max = 0;
            for (int i = 0; i < h_rate; i++) {
                int c = h_cmds;
                h_tick_sched(p, 0);
                empty += h_cmds == c;
            }
            CHECK(h_cmd_msec - m0 == 1000, "schedule %s, second %d: commands last %d ms", h_sched_names[s], w, h_cmd_msec - m0);
            CHECK(empty == idle[s], "schedule %s, second %d: %d of %d ticks without a usercmd (expected %d)",
                  h_sched_names[s], w, empty, h_rate, idle[s]);
            if (s == H_SCHED_TICK || s == H_SCHED_BATCH)
                CHECK(h_cmds - n0 == h_rate && h_cmd_min == 1000 / h_rate && h_cmd_max == (1000 + h_rate - 1) / h_rate,
                      "schedule %s: one command per tick of %d..%d ms (%d commands, %d..%d ms)", h_sched_names[s],
                      1000 / h_rate, (1000 + h_rate - 1) / h_rate, h_cmds - n0, h_cmd_min, h_cmd_max);
        }
        h_tick(p, 0, 1);                   /* shift the phase of the next schedule */
    }
    h_sched = H_SCHED_TICK;
}

/* ---------------------------------------------------------------- T09 */

static int h_shots(edict_t *p, int mz, int from)
{
    int c = 0;
    for (int i = from; i < h_nmsgs; i++)
        if (h_msgs[i].svc == H_SVC_MUZZLEFLASH && h_msgs[i].entnum == NUM(p) &&
            (h_msgs[i].sub & ~MZ_SILENCED) == mz)
            c++;
    return c;
}

static edict_t *h_armed(const char *weapon, const char *ammo, int count)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_give(p, weapon, 0);
    h_give(p, ammo, count);
    h_use(p, weapon);
    return p;
}

/* hold attack for `ticks` world ticks (one usercmd per tick), then release
 * and let the weapon settle; returns shots, *used = ammo spent */
static int h_hold(edict_t *p, int mz, int ticks, int *used)
{
    int m = h_nmsgs, a = h_ammo(p);
    for (int i = 0; i < ticks; i++) h_tick(p, BUTTON_ATTACK, 1);
    int shots = h_shots(p, mz, m);
    *used = a - h_ammo(p);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    return shots;
}

/* Machinegun: fire frames 4/5 alternate while held, one bullet per world
 * frame -> 10 shots and 10 bullets in 1 s (10 ticks). */
static void sc_t09_machinegun_held(void)
{
    edict_t *p = h_armed("machinegun", "bullets", 50);
    CHECK(h_ammo(p) == 50, "50 bullets before (got %d)", h_ammo(p));
    int used, shots = h_hold(p, MZ_MACHINEGUN, h_rate, &used);
    CHECK(shots == 10, "machinegun held 1 s: 10 shots (got %d)", shots);
    CHECK(used == 10, "machinegun held 1 s: 10 bullets spent (got %d)", used);
    int after = h_shots(p, MZ_MACHINEGUN, 0);
    CHECK(after == 10, "no shot after release (total %d)", after);
}

/* HyperBlaster: fire frames 6..11, one cell each, held -> loops 6..11:
 * one shot per frame -> 10 shots / 10 cells in 1 s. */
static void sc_t09_hyperblaster_held(void)
{
    edict_t *p = h_armed("hyperblaster", "cells", 50);
    int used, shots = h_hold(p, MZ_HYPERBLASTER, h_rate, &used);
    CHECK(shots == 10, "hyperblaster held 1 s: 10 shots (got %d)", shots);
    CHECK(used == 10, "hyperblaster held 1 s: 10 cells spent (got %d)", used);
}

/* Rocket launcher: fire frame 5, cycle 5..12 = 8 frames per shot ->
 * held 2 s (20 ticks): shots at ticks 0, 8, 16 -> 3 rockets. */
static void sc_t09_rocket_held(void)
{
    edict_t *p = h_armed("rocket launcher", "rockets", 20);
    int used, shots = h_hold(p, MZ_ROCKET, 2 * h_rate, &used);
    CHECK(shots == 3, "rocket held 2 s: 3 shots (got %d)", shots);
    CHECK(used == 3, "rocket held 2 s: 3 rockets spent (got %d)", used);
}

/* A press and release inside one world tick (two usercmds) fires one
 * discrete shot: the latched press runs the weapon from ClientThink. */
static void sc_t09_rocket_tap(void)
{
    edict_t *p = h_armed("rocket launcher", "rockets", 20);
    for (int k = 0; k < h_div; k++) {
        h_phase(p, k);
        int m = h_nmsgs, a = h_ammo(p);
        h_think(p, BUTTON_ATTACK, 500 / h_rate);
        h_think(p, 0, 500 / h_rate);
        h_frame();
        for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
        CHECK(h_shots(p, MZ_ROCKET, m) == 1, "tap inside one tick, phase %d/%d: 1 rocket (got %d)", k, h_div, h_shots(p, MZ_ROCKET, m));
        CHECK(a - h_ammo(p) == 1, "tap, phase %d/%d: 1 rocket spent (got %d)", k, h_div, a - h_ammo(p));
    }
}

/* Attack held for exactly one world tick, then released, in every phase of
 * the 100 ms weapon step: one discrete shot and one ammo (the 10 Hz
 * contract: a one-tick press is one shot of any weapon). */
static void h_tick_tap(const char *weapon, const char *ammo, int mz)
{
    edict_t *p = h_armed(weapon, ammo, 50);
    for (int k = 0; k < h_div; k++) {
        h_phase(p, k);
        int m = h_nmsgs, a = h_ammo(p);
        h_tick(p, BUTTON_ATTACK, 1);
        for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
        int shots = h_shots(p, mz, m), used = a - h_ammo(p);
        CHECK(shots == 1 && used == 1, "%s one-tick tap, phase %d/%d: 1 shot, 1 ammo (got %d, %d)", weapon, k, h_div, shots, used);
    }
}
static void sc_t09_rocket_tick_tap(void) { h_tick_tap("rocket launcher", "rockets", MZ_ROCKET); }
static void sc_t09_machinegun_tick_tap(void) { h_tick_tap("machinegun", "bullets", MZ_MACHINEGUN); }
static void sc_t09_hyperblaster_tick_tap(void) { h_tick_tap("hyperblaster", "cells", MZ_HYPERBLASTER); }

/* idle until the gun shows `frame` (player_state, as the client sees it) */
static int h_until_gunframe(edict_t *p, int frame)
{
    for (int i = 0; i < 3 * h_rate; i++) {
#ifndef HARNESS_OPENTDM
        /* OpenFFA copies weaponframe to ps.gunframe on FRAMESYNC only: right
         * after a shot a stale idle frame (rocket: 13) must not count */
        if (p->client->weaponframe != frame) { h_tick(p, 0, 1); continue; }
#endif
        if (p->client->ps.gunframe == frame) return 1;
        h_tick(p, 0, 1);
    }
    return 0;
}

/* Second tap after a weapon call (OpenFFA runs the weapon from ClientThink
 * once per 100 ms step): after a shot the gun shows its last firing frame
 * `last` (= FRAME_FIRE_LAST + 1); tap A then makes the weapon ready (the
 * weapon call of that step, no shot), and tap B j world ticks later,
 * j = 1..h_div (every phase up to and including the next weapon step), is
 * one shot and one ammo at the nearest allowed weapon processing. At 10 Hz
 * j = 1: the next step. */
static void h_second_tap(const char *weapon, const char *ammo, int mz, int last)
{
    edict_t *p = h_armed(weapon, ammo, 50);
    for (int j = 1; j <= h_div; j++) {
        h_tick(p, BUTTON_ATTACK, 1);
        if (!h_until_gunframe(p, last)) { CHECK(0, "j=%d: gun reaches its last firing frame %d", j, last); return; }
        int m = h_nmsgs, a = h_ammo(p);
        h_think(p, BUTTON_ATTACK, 500 / h_rate);
        h_think(p, 0, 500 / h_rate);
        h_frame();
        int after_a = h_shots(p, mz, m);
        for (int i = 1; i < j; i++) h_tick(p, 0, 1);
        h_think(p, BUTTON_ATTACK, 500 / h_rate);
        h_think(p, 0, 500 / h_rate);
        h_frame();
        for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
        int shots = h_shots(p, mz, m), used = a - h_ammo(p);
        CHECK(after_a == 0 && shots == 1 && used == 1,
              "%s second tap %d/%d ticks after the ready-making tap: 1 shot, 1 ammo (got %d after A, %d, %d)",
              weapon, j, h_div, after_a, shots, used);
    }
}
static void sc_t09_rocket_second_tap(void) { h_second_tap("rocket launcher", "rockets", MZ_ROCKET, 13); }
static void sc_t09_machinegun_second_tap(void) { h_second_tap("machinegun", "bullets", MZ_MACHINEGUN, 6); }
static void sc_t09_hyperblaster_second_tap(void) { h_second_tap("hyperblaster", "cells", MZ_HYPERBLASTER, 21); }

/* Tapping in every world tick (press and release inside each tick) is no
 * faster than holding: the presses never queue up past the weapon cadence
 * (rocket 3 in 2 s, machinegun 10 in 1 s, as held). One more shot is
 * allowed for the step phase: in the window (OpenFFA's ClientThink call
 * lands on any tick, so its first two calls can straddle a step) or right
 * after it (the last taps follow the last step inside the window). */
static void h_tap_spam(const char *weapon, const char *ammo, int mz, int secs, int held)
{
    edict_t *p = h_armed(weapon, ammo, 50);
    int m = h_nmsgs, a = h_ammo(p);
    for (int i = 0; i < secs * h_rate; i++) {
        h_think(p, BUTTON_ATTACK, 500 / h_rate);
        h_think(p, 0, 500 / h_rate);
        h_frame();
    }
    int shots = h_shots(p, mz, m);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    int late = h_shots(p, mz, m) - shots, used = a - h_ammo(p);
    CHECK(shots >= held && shots <= held + 1 && late <= 1 && used == shots + late,
          "%s tapped every tick for %d s: %d (+1) shots as held, at most 1 after, ammo per shot (got %d, %d after, %d spent)",
          weapon, secs, held, shots, late, used);
}
static void sc_t09_rocket_tap_spam(void) { h_tap_spam("rocket launcher", "rockets", MZ_ROCKET, 2, 3); }
static void sc_t09_machinegun_tap_spam(void) { h_tap_spam("machinegun", "bullets", MZ_MACHINEGUN, 1, 10); }

/* One rocket, attack held 1 s: one shot, then the no-ammo click and the
 * automatic switch away; no further rocket. */
static void sc_t09_rocket_noammo(void)
{
    edict_t *p = h_armed("rocket launcher", "rockets", 1);
    int s = h_nsounds;
    int used, shots = h_hold(p, MZ_ROCKET, h_rate, &used);
    int click = h_find(h_sounds, "weapons/noammo.wav");
    CHECK(shots == 1 && used == 1, "1 rocket held 1 s: 1 shot (got %d, spent %d)", shots, used);
    CHECK(click > 0 && h_count_sounds(click, s) >= 1, "then the no-ammo sound (%d)", click > 0 ? h_count_sounds(click, s) : -1);
    int m = h_nmsgs;
    h_hold(p, MZ_ROCKET, h_rate, &used);
    CHECK(h_shots(p, MZ_ROCKET, m) == 0, "empty: no rocket (got %d)", h_shots(p, MZ_ROCKET, m));
}

/* Pending press losing its context. A press that the weapon has not
 * processed yet at 60 Hz, then a context change in the same world tick
 * (before RunFrame), then control comes back: the old press fires nothing
 * afterwards. At 10 Hz the same press is processed at once, before the
 * change (the info line shows where each rate fired). The control case
 * (no change) proves the press is live: exactly one rocket. */

/* any weapon muzzleflash of p (not the login/logout effects) */
static int h_any_shots(edict_t *p, int from)
{
    int c = 0;
    for (int i = from; i < h_nmsgs; i++) {
        int mz = h_msgs[i].sub & ~MZ_SILENCED;
        if (h_msgs[i].svc == H_SVC_MUZZLEFLASH && h_msgs[i].entnum == NUM(p) &&
            mz != MZ_LOGIN && mz != MZ_LOGOUT && mz != MZ_RESPAWN)
            c++;
    }
    return c;
}

/* Rocket launcher, after one shot. The press (usercmds only, RunFrame is
 * left to the caller; returns the message index before it) lands where the weapon cannot see it yet at 60 Hz:
 * OpenFFA: after this 100 ms step's weapon call from ClientThink (tap A
 * returns the gun from its last firing frame 13 to ready); OpenTDM: one
 * tick after a weapon step with the gun ready (idle frame changed). */
static int h_pending_press(edict_t *p)
{
    h_tick(p, BUTTON_ATTACK, 1);
    h_until_gunframe(p, 13);
#ifdef HARNESS_OPENTDM
    h_until_gunframe(p, 14);    /* ready: first idle frame, set by a step */
#else
    h_think(p, BUTTON_ATTACK, 500 / h_rate);
    h_think(p, 0, 500 / h_rate);
    h_frame();
#endif
    int mb = h_nmsgs;
    h_think(p, BUTTON_ATTACK, 500 / h_rate);
    h_think(p, 0, 500 / h_rate);
    return mb;
}

static edict_t *h_wait_alive(edict_t *p)
{
    for (int i = 0; i < 5 * h_rate && (p->deadflag || p->health <= 0); i++) h_tick(p, BUTTON_ATTACK, 1);
    return p;
}

enum { H_LC_NONE, H_LC_DEATH, H_LC_SPECTATOR, H_LC_MAP, H_LC_SWITCH, H_LC_NOAMMO, H_LC_COUNTDOWN, H_LC_TIMEOUT };
static const char *const h_lc_names[] = { "none", "death", "spectator", "map", "switch", "no-ammo", "countdown", "timeout" };

static void h_lifecycle(int ev)
{
    int two = ev == H_LC_COUNTDOWN || ev == H_LC_TIMEOUT;   /* OpenTDM match: two teams */
    h_scene_base();
    if (two) h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1"), *b = two ? h_join(1, "p2") : NULL;
    int s = h_nsounds;
    (void)b;
    h_give(p, "rocket launcher", 0);
    h_give(p, "rockets", ev == H_LC_NOAMMO ? 1 : 20);
    h_use(p, "rocket launcher");
    h_frames_n(5 * h_rate);     /* past the 5 s kill / mode-change / team limits */
#ifdef HARNESS_OPENTDM
    if (ev == H_LC_TIMEOUT) {   /* a running match */
        h_client_command(p, "ready");
        h_client_command(b, "ready");
        for (int i = 0; i < 30 * h_rate && tdm_match_status != MM_PLAYING; i++) h_frame();
        CHECK(tdm_match_status == MM_PLAYING, "match started (status %d)", tdm_match_status);
        h_give(p, "rocket launcher", 0);
        h_give(p, "rockets", 20);
        h_use(p, "rocket launcher");
        h_frames_n(5 * h_rate);
    }
#endif
    int m0 = h_nmsgs;
    int mb = h_pending_press(p);
    int pressed = h_any_shots(p, m0) - h_any_shots(p, mb);  /* the setup shot */
    int before = h_any_shots(p, mb);   /* the press, at once (10 Hz) */
    int m = h_nmsgs;
    switch (ev) {
    case H_LC_DEATH:
        h_client_command(p, "kill");
        h_frame();
        CHECK(p->deadflag, "kill: dead");
        h_frames_n(2 * h_rate);
        h_wait_alive(p);
        break;
    case H_LC_SPECTATOR:
#ifdef HARNESS_OPENTDM
        h_client_command(p, "spec");
        h_frame();
        CHECK(!p->client->pers.team, "spectator");
        h_frames_n(5 * h_rate);
        h_client_command(p, "team a");
#else
        h_client_command(p, "spectator");
        h_frame();
        CHECK(p->client->pers.connected == CONN_SPECTATOR, "spectator");
        h_frames_n(5 * h_rate);
        h_client_command(p, "join");
#endif
        break;
    case H_LC_MAP:
        ge->SpawnEntities("harness", h_ents, "");
        ge->ClientBegin(p);
        h_frame();
#ifndef HARNESS_OPENTDM
        /* OpenFFA starts the new map in pregame: attack spawns, as on connect */
        CHECK(p->client->pers.connected == CONN_PREGAME, "new map: pregame (%d)", p->client->pers.connected);
        h_tick(p, BUTTON_ATTACK, 1);
        h_tick(p, 0, 1);
        CHECK(p->client->pers.connected == CONN_SPAWNED, "spawned (%d)", p->client->pers.connected);
#else
        CHECK(p->client->pers.team == TEAM_A, "new map: still in team a (%d)", p->client->pers.team);
#endif
        break;
    case H_LC_SWITCH:
        h_client_command(p, "use blaster");
        h_frame();
        break;
#ifdef HARNESS_OPENTDM
    case H_LC_COUNTDOWN:
        h_client_command(p, "ready");
        h_client_command(b, "ready");
        h_frame();
        CHECK(tdm_match_status == MM_COUNTDOWN, "countdown (status %d)", tdm_match_status);
        for (int i = 0; i < 30 * h_rate && tdm_match_status != MM_PLAYING; i++) h_frame();
        CHECK(tdm_match_status == MM_PLAYING, "match started (status %d)", tdm_match_status);
        break;
    case H_LC_TIMEOUT:
        h_client_command(p, "time");
        h_frame();
        CHECK(tdm_match_status == MM_TIMEOUT, "timeout (status %d)", tdm_match_status);
        h_frames_n(h_rate);
        h_client_command(p, "time");
        for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) h_frame();
        CHECK(tdm_match_status == MM_PLAYING, "resumed (status %d)", tdm_match_status);
        break;
#endif
    default:    /* none, no-ammo: the weapon step itself */
        h_frame();
        break;
    }
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    int after = h_any_shots(p, m);
    printf("  info %s: press fired %d before the change, %d after\n", h_lc_names[ev], before, after);
    CHECK(pressed == 1, "setup shot (got %d)", pressed);
    CHECK(p->inuse && !p->deadflag && p->health > 0, "control back: alive (health %d)", p->health);
    if (ev == H_LC_NONE) {
        CHECK(before + after == 1, "no change: the press is one rocket (got %d + %d)", before, after);
    } else if (ev == H_LC_NOAMMO) {
        int click = h_find(h_sounds, "weapons/noammo.wav");
        CHECK(before + after == 0, "no ammo: no shot of any weapon (got %d + %d)", before, after);
        CHECK(click > 0 && h_count_sounds(click, s) >= 1, "no-ammo sound");
    } else if (ev == H_LC_SWITCH) {
        CHECK(h_shots(p, MZ_BLASTER, m) == 0, "switch: no blaster shot (got %d)", h_shots(p, MZ_BLASTER, m));
        CHECK(before + h_shots(p, MZ_ROCKET, m) <= 1, "switch: at most the one rocket of the press (got %d + %d)", before, h_shots(p, MZ_ROCKET, m));
    } else {
        CHECK(after == 0, "%s: no shot after control is back (got %d)", h_lc_names[ev], after);
    }
    /* control is really back: a new tap fires once */
    int mf = h_nmsgs;
    h_tick(p, BUTTON_ATTACK, 1);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    CHECK(h_any_shots(p, mf) == 1, "then a new tap: 1 shot (got %d)", h_any_shots(p, mf));
}
static void sc_t09_lifecycle_none(void) { h_lifecycle(H_LC_NONE); }
static void sc_t09_lifecycle_death(void) { h_lifecycle(H_LC_DEATH); }
static void sc_t09_lifecycle_spectator(void) { h_lifecycle(H_LC_SPECTATOR); }
static void sc_t09_lifecycle_map(void) { h_lifecycle(H_LC_MAP); }
static void sc_t09_lifecycle_switch(void) { h_lifecycle(H_LC_SWITCH); }
static void sc_t09_lifecycle_noammo(void) { h_lifecycle(H_LC_NOAMMO); }
#ifdef HARNESS_OPENTDM
static void sc_t09_lifecycle_countdown(void) { h_lifecycle(H_LC_COUNTDOWN); }
static void sc_t09_lifecycle_timeout(void) { h_lifecycle(H_LC_TIMEOUT); }
#endif

#ifdef HARNESS_OPENTDM
/* OpenTDM attention blocks (warmup): g_ready_attention with half of the
 * players ready (team safety, 3 players: not a duel) and g_vote_attention
 * 2 for a player who has not voted. A tap while blocked fires nothing,
 * neither during the block nor after it is lifted (the pending press is
 * dropped); then a new tap fires once. */
static void h_attention(int byvote)
{
    h_set_override(byvote ? "g_vote_attention" : "g_ready_attention", byvote ? "2" : "1");
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"-300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1"), *q = h_join(1, "p2"), *r = byvote ? NULL : h_join(2, "p3");
    h_give(p, "rocket launcher", 0);
    h_give(p, "rockets", 20);
    h_use(p, "rocket launcher");
    h_frames_n(5 * h_rate);
    if (byvote) {
        h_client_command(q, "vote timelimit 15");
        CHECK(vote.active && p->client->resp.vote == VOTE_HOLD, "vote on, p1 has not voted");
    } else {
        h_client_command(q, "ready");
        h_client_command(r, "ready");
        CHECK(teaminfo[TEAM_A].safety && tdm_match_status == MM_WARMUP, "half ready: team safety on, warmup");
    }
    int m = h_nmsgs;
    h_tick(p, BUTTON_ATTACK, 1);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    int during = h_any_shots(p, m);
    m = h_nmsgs;
    if (byvote) {
        h_client_command(p, "vote no");
        CHECK(!vote.active || p->client->resp.vote != VOTE_HOLD, "p1 voted: no longer held");
    } else {
        h_client_command(r, "notready");
        CHECK(!teaminfo[TEAM_A].safety, "one of three ready: safety off");
    }
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    int after = h_any_shots(p, m);
    CHECK(during == 0, "%s: a tap while blocked fires nothing (got %d)", byvote ? "vote hold" : "ready attention", during);
    CHECK(after == 0, "%s: nothing fires once the block is lifted (got %d)", byvote ? "vote hold" : "ready attention", after);
    m = h_nmsgs;
    h_tick(p, BUTTON_ATTACK, 1);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    CHECK(h_any_shots(p, m) == 1, "then a new tap: 1 shot (got %d)", h_any_shots(p, m));
}
static void sc_t09_lifecycle_readyattn(void) { h_attention(0); }
static void sc_t09_lifecycle_votehold(void) { h_attention(1); }
#endif

/* ---------------------------------------------------------------- T10 */

#ifdef HARNESS_OPENTDM
/* TDM: 400 + (3.0 - 1.0) * (400 / 3.0) = 666.67 -> int 666 */
#define H_GRENADE_SPEED_1S 666
#else
/* OpenFFA: 400 + (30 - 10) * (400 / 30) = 400 + 20 * 13 = 660 */
#define H_GRENADE_SPEED_1S 660
#endif
#ifdef HARNESS_OPENTDM
/* quick release, thrown one step after arming, 3.1 s left:
 * TDM 400 + (3.0 - 3.1) * (400 / 3.0) = 386.67 -> int 386 */
#define H_GRENADE_SPEED_QUICK 386
#else
/* OpenFFA 400 + (30 - 31) * 13 = 387 */
#define H_GRENADE_SPEED_QUICK 387
#endif

static int h_explosions(int from)
{
    int c = 0;
    for (int i = from; i < h_nmsgs; i++)
        if (h_msgs[i].svc == H_SVC_TEMP_ENTITY &&
            (h_msgs[i].sub == TE_GRENADE_EXPLOSION || h_msgs[i].sub == TE_ROCKET_EXPLOSION ||
             h_msgs[i].sub == TE_GRENADE_EXPLOSION_WATER || h_msgs[i].sub == TE_ROCKET_EXPLOSION_WATER))
            c++;
    return c;
}

/* a thrown hand grenade: entity state with the grenade model and effect */
static edict_t *h_grenade(void)
{
    int model = h_find(h_models, "models/objects/grenade2/tris.md2");
    for (int n = 1; n < ge->num_edicts; n++) {
        edict_t *e = EDICT(n);
        if (e->inuse && model > 0 && e->s.modelindex == model && (e->s.effects & EF_GRENADE))
            return e;
    }
    return NULL;
}

/* press attack with the hand grenade until the fuse arms (the timer sound
 * starts on the player); returns the world frame of arming */
static int h_arm_grenade(edict_t *p)
{
    int tick = h_find(h_sounds, "weapons/hgrenc1b.wav");
    for (int i = 0; i < 3 * h_rate; i++) {
        h_tick(p, BUTTON_ATTACK, 1);
        if (tick < 0) tick = h_find(h_sounds, "weapons/hgrenc1b.wav");
        if (tick > 0 && p->s.sound == tick) return h_frames - 1;
    }
    return -1;
}

/* Hold until the fuse runs out: the grenade explodes in the hand 3.2 s
 * (32 world frames) after arming, one explosion, one grenade spent, no
 * flying grenade left. */
static void sc_t10_grenade_held_explodes(void)
{
    edict_t *p = h_armed("grenades", "grenades", 5);
    int a = h_ammo(p), m = h_nmsgs;
    int arm = h_arm_grenade(p);
    CHECK(arm >= 0, "fuse armed (frame %d)", arm);
    int boom = -1;
    for (int i = 0; i < 5 * h_rate && boom < 0; i++) {
        h_tick(p, BUTTON_ATTACK, 1);
        if (h_explosions(m)) boom = h_frames - 1;
    }
    /* deadline 3.2 s, seen on the weapon step at or after it */
    CHECK(boom - arm >= 32 * h_div && boom - arm < 33 * h_div,
          "explodes in hand 3.2 s after arming: frames in [%d, %d) (got %d)", 32 * h_div, 33 * h_div, boom - arm);
    CHECK(h_health(p) < 100, "the explosion hurts the holder (health %d)", h_health(p));
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, BUTTON_ATTACK, 1);
    CHECK(h_explosions(m) == 1, "one explosion (got %d)", h_explosions(m));
    CHECK(a - h_ammo(p) == 1, "one grenade spent (got %d)", a - h_ammo(p));
    CHECK(h_grenade() == NULL, "no flying grenade");
}

/* Release with 1 s remaining: the throw happens on the next weapon frame
 * after release; forward speed (x displacement per frame on level aim)
 * is the original curve at timer 1 s. */
static void sc_t10_grenade_release_1s(void)
{
    edict_t *p = h_armed("grenades", "grenades", 5);
    int a = h_ammo(p);
    int arm = h_arm_grenade(p);
    CHECK(arm >= 0, "fuse armed (frame %d)", arm);
    /* released in the weapon step 2.1 s after arming; throw on the next
     * step (2.2 s); remaining = 3.2 - 2.2 = 1 s */
    while (h_frames - 1 < arm + 21 * h_div - 1) h_tick(p, BUTTON_ATTACK, 1);
    edict_t *g = NULL;
    int thrown = -1;
    for (int i = 0; i < 5 * h_div && !g; i++) { h_tick(p, 0, 1); g = h_grenade(); if (g) thrown = h_frames - 1; }
    CHECK(g != NULL, "grenade thrown");
    if (!g) return;
    CHECK(thrown - arm == 22 * h_div, "thrown 2.2 s (%d frames) after arming, 1 s remaining (got %d)", 22 * h_div, thrown - arm);
    float x0 = g->s.origin[0];
    h_frame();
    float speed = (g->s.origin[0] - x0) * h_rate;
    CHECK(fabsf(speed - H_GRENADE_SPEED_1S) <= 1, "forward speed %.2f = %d +- 1", speed, H_GRENADE_SPEED_1S);
    int boom = -1, m = h_nmsgs;
    for (int i = 0; i < 3 * h_rate && boom < 0; i++) { h_frame(); if (h_explosions(m)) boom = h_frames - 1; }
    CHECK(boom - thrown == h_rate, "flying grenade explodes after the remaining 1 s: %d frames (got %d)", h_rate, boom - thrown);
    CHECK(a - h_ammo(p) == 1, "one grenade spent (got %d)", a - h_ammo(p));
}

/* Quick release: attack pressed for one tick, released before the fuse
 * arms. The throw comes one weapon step after arming: 3.1 s of fuse left,
 * the mod's own curve at 3.1 s, the grenade explodes 3.1 s after the throw.
 * A tap right after the throw (the gun is still in its throw frames) adds
 * nothing: one grenade, one ammo. */
static void sc_t10_grenade_quick(void)
{
    edict_t *p = h_armed("grenades", "grenades", 5);
    int a = h_ammo(p), m = h_nmsgs;
    h_tick(p, BUTTON_ATTACK, 1);
    edict_t *g = NULL;
    int thrown = -1;
    for (int i = 0; i < 3 * h_rate && !g; i++) { h_tick(p, 0, 1); g = h_grenade(); if (g) thrown = h_frames - 1; }
    CHECK(g != NULL, "grenade thrown");
    if (!g) return;
    h_tick(p, BUTTON_ATTACK, 1);
    float x0 = g->s.origin[0];
    h_frame();
    float speed = (g->s.origin[0] - x0) * h_rate;
    CHECK(fabsf(speed - H_GRENADE_SPEED_QUICK) <= 1, "quick release speed %.2f = %d +- 1", speed, H_GRENADE_SPEED_QUICK);
    int boom = -1;
    for (int i = 0; i < 5 * h_rate && boom < 0; i++) { h_tick(p, 0, 1); if (h_explosions(m)) boom = h_frames - 1; }
    CHECK(boom - thrown == 31 * h_div, "explodes 3.1 s after the throw: %d frames (got %d)", 31 * h_div, boom - thrown);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    CHECK(h_explosions(m) == 1 && a - h_ammo(p) == 1, "one grenade, one ammo (got %d explosions, %d spent)", h_explosions(m), a - h_ammo(p));
}


/* A released re-tap while a grenade is
 * cooking must not turn into a synthetic hold at the next weapon update.
 * The pending tap is consumed with gunframe 12 on that same update, and the
 * grenade flies 100 ms later, as at 10 Hz. */
static void sc_t10_grenade_pending_tap(void)
{
    edict_t *p = h_armed("grenades", "grenades", 5);
    int arm = h_arm_grenade(p);
    CHECK(arm >= 0, "grenade armed by real input");
    if (arm < 0) return;
    int a = h_ammo(p);

#ifdef HARNESS_OPENTDM
    /* Reach an automatic weapon call. At higher world rates the next
     * weapon update is still ahead when input arrives after RunFrame. */
    int previous_next = p->client->next_weapon_think;
    for (int i = 0; i < 3 * h_div && p->client->next_weapon_think == previous_next; i++) {
        int dt = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
        h_think(p, BUTTON_ATTACK, dt);
        h_frame();
    }
    CHECK(p->client->next_weapon_think > previous_next, "automatic weapon update occurred before the re-tap");
#endif

    int msec = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
    h_think(p, 0, 1);
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, msec - 2);
#ifndef HARNESS_OPENTDM
    /* FFA permits this first ClientThink call immediately; a second tap in
     * a later world frame, while weapon_thunk is set, creates pending. */
    h_frame();
    if (h_div > 1) {
        msec = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
        h_think(p, BUTTON_ATTACK, 1);
        h_think(p, 0, msec - 1);
    }
#endif
    CHECK(!(p->client->buttons & BUTTON_ATTACK), "last real input released attack");
    int queued = p->client->attack_pending;
    if (h_div > 1)
        CHECK(queued, "the re-tap waits for a permitted weapon update");

    int released = -1;
    for (int i = 0; i < 4 * h_div; i++) {
        msec = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
        h_think(p, 0, msec);
        h_frame();
#ifdef HARNESS_OPENTDM
        int gunframe = p->client->ps.gunframe;
#else
        int gunframe = p->client->weaponframe;
#endif
        if (queued ? !p->client->attack_pending : gunframe == 12) {
            released = h_frames - 1;
            printf("  info consumed frame=%d gunframe=%d real_buttons=%d fuse_frame=%d\n",
                   released, gunframe, p->client->buttons, p->client->grenade_framenum);
            CHECK(gunframe == 12, "released cooking grenade advances to throw frame 12 at the permitted update (got %d)", gunframe);
            break;
        }
    }
    CHECK(released >= 0, "a permitted weapon update occurred");

    edict_t *g = NULL;
    int thrown = -1;
    for (int i = 0; i < 4 * h_div && !g; i++) {
        msec = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
        h_think(p, 0, msec);
        h_frame();
        g = h_grenade();
        if (g) thrown = h_frames - 1;
    }
    printf("  info arm=%d consumed=%d thrown=%d expected_throw=%d delta=%d\n",
           arm, released, thrown, released + h_div, thrown - released);
    CHECK(g != NULL && thrown == released + h_div,
          "one flying grenade on the following 100 ms weapon update (got frame %d, expected %d)", thrown, released + h_div);
    CHECK(a - h_ammo(p) == 1, "exactly one grenade spent");
}

/* Death while holding an armed grenade: it goes off at once (timer 0) at
 * the first weapon processing after the death: one explosion, nothing in
 * flight. OpenTDM runs the dead player's weapon every world frame (the
 * death frame), OpenFFA on its 100 ms step (within h_div frames). The ammo
 * is not observable: the death clears the inventory before the fire. */
static void sc_t10_grenade_death(void)
{
    edict_t *p = h_armed("grenades", "grenades", 5);
    int m = h_nmsgs;
    h_frames_n(5 * h_rate);     /* past the 5 s kill limit */
    CHECK(h_arm_grenade(p) >= 0, "fuse armed");
    for (int i = 0; i < 5 * h_div; i++) h_tick(p, BUTTON_ATTACK, 1);
    CHECK(h_explosions(m) == 0, "still in hand");
    h_think(p, BUTTON_ATTACK, 1000 / h_rate);
    h_client_command(p, "kill");
    int k = h_frames, boom = -1;
    for (int i = 0; i < 2 * h_div && boom < 0; i++) { h_frame(); if (h_explosions(m)) boom = h_frames - 1; }
    CHECK(p->deadflag, "dead");
#ifdef HARNESS_OPENTDM
    CHECK(boom == k, "explodes in the frame of the death (frame %d, got %d)", k, boom);
#else
    CHECK(boom >= k && boom < k + h_div, "explodes at the first weapon step after the death: frames [%d, %d) (got %d)", k, k + h_div, boom);
#endif
    CHECK(h_grenade() == NULL, "no flying grenade");
    h_frames_n(4 * h_rate);
    CHECK(h_explosions(m) == 1, "one explosion (got %d)", h_explosions(m));
}

/* Weapon switch while holding: the hold goes on, the release throws the
 * grenade (frame 11 -> 12 -> throw), then the gun switches; one grenade,
 * one ammo, one explosion. */
static void sc_t10_grenade_switch(void)
{
    edict_t *p = h_armed("grenades", "grenades", 5);
    int *inv = &p->client->inventory[p->client->ammo_index], a = *inv, m = h_nmsgs;
    CHECK(h_arm_grenade(p) >= 0, "fuse armed");
    h_client_command(p, "use blaster");
    for (int i = 0; i < 10 * h_div; i++) h_tick(p, BUTTON_ATTACK, 1);
    CHECK(h_grenade() == NULL && h_explosions(m) == 0, "held 1 s after the switch: still in hand");
    int rel = h_frames;
    edict_t *g = NULL;
    for (int i = 0; i < 3 * h_div && !g; i++) { h_tick(p, 0, 1); g = h_grenade(); }
    /* the step after release leaves frame 11, the next one throws */
    CHECK(g != NULL && h_frames - 1 - rel < 2 * h_div, "thrown by the second weapon step after release: < %d frames (got %d)", 2 * h_div, h_frames - 1 - rel);
    for (int i = 0; i < 4 * h_rate; i++) h_tick(p, 0, 1);
    CHECK(h_explosions(m) == 1, "one explosion (got %d)", h_explosions(m));
    CHECK(a - *inv == 1, "one grenade spent (got %d)", a - *inv);
    int m2 = h_nmsgs;
    h_tick(p, BUTTON_ATTACK, 1);
    for (int i = 0; i < h_rate; i++) h_tick(p, 0, 1);
    CHECK(h_shots(p, MZ_BLASTER, m2) == 1, "then the blaster is up (got %d blaster shots)", h_shots(p, MZ_BLASTER, m2));
}

/* ---------------------------------------------------------------- T11 */

static int h_sum(const int *v, int from, int to)
{
    int s = 0;
    for (int i = from; i < to; i++) s += v[i];
    return s;
}

/* trigger_hurt brush away from the spawn point; dmg 1 so health counts hits */
static void h_hurt_scene(int spawnflags)
{
    h_scene_base();
    int m = h_add_inline(448, -64, 0, 576, 64, 128);
    h_ent("{\n\"classname\" \"trigger_hurt\"\n\"model\" \"*%d\"\n\"dmg\" \"1\"\n\"spawnflags\" \"%d\"\n}\n", m, spawnflags);
    h_start(h_ents);
}

/* damage taken as the player sees it: health plus armor (TDM spawns with
 * armor that absorbs part of the hit) */
static int h_hp(edict_t *p) { return h_health(p) + h_stat(p, STAT_ARMOR); }

#define H_MAXTICKS (10 * 60)    /* 10 s at the highest harness rate */
static int h_snd[H_MAXTICKS];   /* hurt sounds per tick of the last h_hurt_run */

/* step into the trigger, then `ticks` world ticks of `ncmds` usercmds;
 * hits[i] = health lost in tick i, h_snd[i] = hurt sounds in it; returns total */
static int h_hurt_run(edict_t *p, int ticks, int ncmds, int *hits, int *sounds)
{
    int electro = h_find(h_sounds, "world/electro.wav");
    int s = h_nsounds, total = 0;
    h_place(p, 512, 0, 24);
    for (int i = 0; i < ticks; i++) {
        int hp = h_hp(p), s0 = h_nsounds;
        h_tick(p, 0, ncmds);
        hits[i] = hp - h_hp(p);
        h_snd[i] = h_count_sounds(electro, s0);
        total += hits[i];
    }
    *sounds = h_count_sounds(electro, s);
    return total;
}

/* normal: first hit in the tick of first contact, then one every 0.1 s:
 * 10 hits in [t0, t0 + 1 s); the sound has its own 1 s cooldown: once in
 * every second of contact, whatever the contact phase */

/* leave the trigger and wait 2 s (the source debounce runs out) */
static void h_hurt_leave(edict_t *p)
{
    h_place(p, 0, 0, 24);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    h_give(p, "health", 100);
    h_frame();      /* the HUD health stat follows at the end of a frame */
}

static void sc_t11_hurt_normal(void)
{
    h_hurt_scene(0);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    int sounds, total;
    /* first contact in every phase of the 100 ms step */
    for (int k = 0; k < h_div; k++) {
        if (k) h_hurt_leave(p);
        h_phase(p, k);                          /* the contact tick has phase k */
        total = h_hurt_run(p, h_rate, 1, hits, &sounds);
        CHECK(hits[0] == 1, "phase %d/%d: first hit in the contact tick (%d)", k, h_div, hits[0]);
        CHECK(total == 10, "phase %d/%d: 10 hits in the first second (got %d)", k, h_div, total);
        CHECK(h_snd[0] == 1, "phase %d/%d: hurt sound on the first touch (%d)", k, h_div, h_snd[0]);
        CHECK(sounds == 1, "phase %d/%d: one hurt sound in the first second (got %d)", k, h_div, sounds);
    }
    /* the sound cooldown over 3 s of contact, in every phase */
    for (int k = 0; k < h_div; k++) {
        h_hurt_leave(p);
        h_phase(p, k);
        h_hurt_run(p, 3 * h_rate, 1, hits, &sounds);
        int s1 = h_sum(h_snd, 0, h_rate), s2 = h_sum(h_snd, h_rate, 2 * h_rate), s3 = h_sum(h_snd, 2 * h_rate, 3 * h_rate);
        CHECK(s1 == 1 && s2 == 1 && s3 == 1, "phase %d/%d: one hurt sound in each of 3 s of contact (got %d, %d, %d)",
              k, h_div, s1, s2, s3);
    }
    h_hurt_leave(p);
    total = h_hurt_run(p, h_rate, 2, hits, &sounds);
    CHECK(total == 10, "two usercmds per tick: still 10 hits per second (got %d)", total);
}

/* SLOW (16): one hit per second, each with its sound, in every contact phase */
static void sc_t11_hurt_slow(void)
{
    h_hurt_scene(16);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    int sounds;
    for (int k = 0; k < h_div; k++) {
        if (k) h_hurt_leave(p);
        h_phase(p, k);
        int total = h_hurt_run(p, 2 * h_rate, 1, hits, &sounds);
        CHECK(hits[0] == 1 && hits[h_rate] == 1, "phase %d/%d: SLOW hits at t0 and t0 + 1 s (%d, %d)",
              k, h_div, hits[0], hits[h_rate]);
        CHECK(total == 2, "phase %d/%d: SLOW: 2 hits in 2 s (got %d)", k, h_div, total);
        CHECK(sounds == 2, "phase %d/%d: SLOW: a sound with each hit (got %d)", k, h_div, sounds);
    }
}

/* TOGGLE (2): the real use callback switches the source off and on; back
 * on, the first contact hits at once, then 10 per second with one sound */
static void sc_t11_hurt_toggle(void)
{
    h_hurt_scene(2);
    edict_t *p = h_join(0, "p1");
    edict_t *t = NULL;
    for (int n = 1; n < ge->num_edicts; n++)
        if (EDICT(n)->inuse && EDICT(n)->use && !strcmp(EDICT(n)->classname, "trigger_hurt")) t = EDICT(n);
    CHECK(t != NULL, "toggleable trigger_hurt spawned");
    if (!t) return;
    static int hits[H_MAXTICKS];
    int sounds, total = h_hurt_run(p, h_rate, 1, hits, &sounds);
    CHECK(total == 10, "on: 10 hits in 1 s (got %d)", total);
    t->use(t, p, p);
    total = h_hurt_run(p, 2 * h_rate, 1, hits, &sounds);
    CHECK(total == 0 && sounds == 0, "off: no hit, no sound in 2 s (got %d, %d)", total, sounds);
    t->use(t, p, p);
    total = h_hurt_run(p, h_rate, 1, hits, &sounds);
    CHECK(hits[0] == 1, "on again: first hit at once (%d)", hits[0]);
    CHECK(total == 10 && sounds == 1, "on again: 10 hits and one sound in 1 s (got %d, %d)", total, sounds);
}

/* SILENT (4): damage without sound */
static void sc_t11_hurt_silent(void)
{
    h_hurt_scene(4);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    int sounds;
    int total = h_hurt_run(p, h_rate, 1, hits, &sounds);
    CHECK(total == 10 && sounds == 0, "SILENT: 10 hits, no sound (got %d, %d)", total, sounds);
}

/* NO_PROTECTION (8) hurts a god-mode player; the plain trigger does not */
static void sc_t11_hurt_no_protection(void)
{
    static int hits[H_MAXTICKS];
    int sounds;
    h_hurt_scene(0);
    edict_t *p = h_join(0, "p1");
    h_client_command(p, "god");
    int total = h_hurt_run(p, h_rate, 1, hits, &sounds);
    CHECK(total == 0, "god mode, plain trigger: no damage (got %d)", total);
}

static void sc_t11_hurt_no_protection_god(void)
{
    static int hits[H_MAXTICKS];
    int sounds;
    h_hurt_scene(8);
    edict_t *p = h_join(0, "p1");
    h_client_command(p, "god");
    int total = h_hurt_run(p, h_rate, 1, hits, &sounds);
    CHECK(total == 10, "god mode, NO_PROTECTION: 10 hits (got %d)", total);
}

/* One source, two players inside: the debounce is per source, so the
 * source deals one hit per world frame in total, and one sound per second. */
static void sc_t11_hurt_shared(void)
{
    h_hurt_scene(0);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2");
    int s = h_nsounds, sounds = -1;
    h_place(a, 512, 0, 24);
    h_think(a, 0, 1000 / h_rate);   /* the first touch */
    h_place(b, 512, 0, 24);
    h_think(b, 0, 1000 / h_rate);
    h_frame();
    int ha = h_hp(a), hb = h_hp(b);
    for (int i = 0; i < h_rate; i++) {
        if (i == h_rate - 1)    /* frames [first touch, + 1 s) */
            sounds = h_count_sounds(h_find(h_sounds, "world/electro.wav"), s);
        h_think(a, 0, 1000 / h_rate);
        h_think(b, 0, 1000 / h_rate);
        h_frame();
    }
    int da = ha - h_hp(a), db = hb - h_hp(b);
    CHECK(da + db == 10, "shared source: 10 hits per second over both players (got %d + %d)", da, db);
    CHECK(sounds == 1, "shared source: one hurt sound in the first second over both players (got %d)", sounds);
}

/* target_laser (START_ON, dmg 1) through a standing player: the beam
 * damages on its own 100 ms think, 10 hits in every second of contact
 * (not one per world tick) */
static void sc_t11_laser(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"target_laser\"\n\"origin\" \"200 0 24\"\n\"angle\" \"180\"\n\"spawnflags\" \"1\"\n\"dmg\" \"1\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);  /* the laser starts 1 s after spawn */
    for (int k = 0; k < 2; k++) {
        int hp = h_hp(p);
        for (int i = 0; i < h_rate; i++) h_tick(p, 0, 1);
        CHECK(hp - h_hp(p) == 10, "second %d in the beam: 10 hits (got %d)", k + 1, hp - h_hp(p));
    }
}

/* ---------------------------------------------------------------- T12 */

/* run `ticks` world ticks of `ncmds` usercmds; hits[i] = damage in tick i;
 * returns the first tick with a hit or -1 */
static int h_damage_run(edict_t *p, int ticks, int ncmds, int *hits)
{
    int first = -1;
    for (int i = 0; i < ticks; i++) {
        int hp = h_hp(p);
        h_tick(p, 0, ncmds);
        hits[i] = hp - h_hp(p);
        if (hits[i] && first < 0) first = i;
    }
    return first;
}

static edict_t *h_by_model(const char *model)
{
    int idx = h_find(h_models, model);
    for (int n = 1; idx > 0 && n < ge->num_edicts; n++)
        if (EDICT(n)->inuse && EDICT(n)->s.modelindex == idx) return EDICT(n);
    return NULL;
}

/* sliding door (+x) squeezing the player against a wall at x = 128.5 */
static void h_door_scene(int spawnflags)
{
    h_scene_base();
    h_add_world(128.5f, 900, 0, 200, 1100, 200);
    int m = h_add_inline(-200, 968, 0, 40, 1032, 128);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"0\"\n\"dmg\" \"1\"\n\"spawnflags\" \"%d\"\n}\n", m, spawnflags);
    h_start(h_ents);
}

/* blocked door: one immediate hit, then it reverses (closes) */
static void sc_t12_door_blocked_reverses(void)
{
    h_door_scene(0);
    edict_t *p = h_join(0, "p1");
    edict_t *door = h_by_model("*1");
    h_place(p, 96, 1000, 24);
    static int hits[H_MAXTICKS];
    int t0 = -1, n = 4 * h_rate;
    float xblock = 0, xafter = 0;
    for (int i = 0; i < n; i++) {
        int hp = h_hp(p);
        h_tick(p, 0, 1);
        hits[i] = hp - h_hp(p);
        if (hits[i] && t0 < 0) { t0 = i; xblock = door->s.origin[0]; }
        else if (t0 >= 0 && i == t0 + 1) xafter = door->s.origin[0];
    }
    CHECK(t0 >= 0, "door blocked by the player (tick %d)", t0);
    if (t0 < 0 || t0 + h_rate > n) return;
    CHECK(hits[t0] == 1, "one hit at the block (%d)", hits[t0]);
    CHECK(h_sum(hits, t0, t0 + h_rate) == 1, "one hit in [t0, t0 + 1 s) (got %d)", h_sum(hits, t0, t0 + h_rate));
    CHECK(xafter < xblock, "door reversed: x %.3f one tick later < %.3f at the block", xafter, xblock);
}

/* DOOR_CRUSHER (4): keeps pushing, a hit every world frame: 10 per second */
static void sc_t12_door_crusher(void)
{
    h_door_scene(4);
    edict_t *p = h_join(0, "p1");
    h_place(p, 96, 1000, 24);
    static int hits[H_MAXTICKS];
    int n = 4 * h_rate;
    int t0 = h_damage_run(p, n, 1, hits);
    CHECK(t0 >= 0, "crusher reached the player (tick %d)", t0);
    if (t0 < 0 || t0 + h_rate > n) return;
    CHECK(h_sum(hits, t0, t0 + h_rate) == 10, "crusher: 10 hits in [t0, t0 + 1 s) (got %d)", h_sum(hits, t0, t0 + h_rate));
}

/* door blocked by a non-player object (item): the object is destroyed
 * with an explosion */
static void sc_t12_door_debris(void)
{
    h_scene_base();
    h_add_world(128.5f, 900, 0, 200, 1100, 200);
    int m = h_add_inline(-200, 968, 0, 40, 1032, 128);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"0\"\n\"dmg\" \"1\"\n}\n", m);
    h_ent("{\n\"classname\" \"item_armor_shard\"\n\"origin\" \"96 1000 16\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    edict_t *item = h_by_model("models/items/armor/shard/tris.md2");
    CHECK(item != NULL, "armor shard spawned");
    if (!item) return;
    int n = NUM(item), msg = h_nmsgs;
    h_place(p, -100, 1070, 24);    /* in the door's trigger field, not in its path */
    for (int i = 0; i < 4 * h_rate && EDICT(n)->inuse && EDICT(n)->s.modelindex == item->s.modelindex; i++)
        h_tick(p, 0, 1);
    CHECK(!(EDICT(n)->inuse && EDICT(n)->s.modelindex == item->s.modelindex), "blocking object removed");
    CHECK(h_count_msgs(H_SVC_TEMP_ENTITY, TE_EXPLOSION1, msg) == 1, "one explosion (got %d)",
          h_count_msgs(H_SVC_TEMP_ENTITY, TE_EXPLOSION1, msg));
}

/* rising plat squeezing the player into a ceiling: one hit, then down
 * (the accelerating mover starts back one frame after the block) */
static void sc_t12_plat_blocked(void)
{
    h_scene_base();
    h_add_world(-64, 936, 100, 64, 1064, 200);
    int m = h_add_inline(-64, 936, -8, 64, 1064, 64);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"dmg\" \"1\"\n}\n", m);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    edict_t *plat = h_by_model("*1");
    h_place(p, 0, 1000, 24);
    static int hits[H_MAXTICKS];
    int t0 = -1;
    float zblock = 0, zafter = 0;
    for (int i = 0; i < 10 * h_rate; i++) {
        int hp = h_hp(p);
        h_tick(p, 0, 1);
        hits[i] = hp - h_hp(p);
        if (hits[i] && t0 < 0) zblock = zafter = plat->s.origin[2];
        if (hits[i] && t0 < 0) t0 = i;
        if (t0 >= 0 && i > t0 && plat->s.origin[2] < zafter) zafter = plat->s.origin[2];
        if (t0 >= 0 && i == t0 + h_rate) break;
    }
    CHECK(t0 >= 0, "plat blocked by the player (tick %d)", t0);
    if (t0 < 0) return;
    CHECK(h_sum(hits, t0, t0 + h_rate) == 1, "one hit in [t0, t0 + 1 s) (got %d)", h_sum(hits, t0, t0 + h_rate));
    CHECK(zafter < zblock, "plat reversed within 1 s: lowest z %.3f < %.3f at the block", zafter, zblock);
}

/* func_rotating with the player inside its (rotated) bounds: blocked
 * every world frame */
static void sc_t12_rotating_blocked(void)
{
    h_scene_base();
    int m = h_add_inline(-32, -32, -32, 32, 32, 32);
    h_ent("{\n\"classname\" \"func_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 1000 24\"\n\"dmg\" \"1\"\n\"spawnflags\" \"1\"\n}\n", m);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_place(p, 0, 1000, 24);
    static int hits[H_MAXTICKS];
    int t0 = h_damage_run(p, 2 * h_rate, 1, hits);
    CHECK(t0 >= 0 && t0 <= 1, "blocked at once (tick %d)", t0);
    if (t0 < 0) return;
    CHECK(h_sum(hits, t0, t0 + h_rate) == 10, "rotating_blocked: 10 hits in 1 s (got %d)", h_sum(hits, t0, t0 + h_rate));
}

/* TOUCH_PAIN (16): a hit on every touch from a usercmd; two ClientThink
 * in one world tick give two hits. Per-usercmd by contract, so at R Hz
 * one usercmd per tick gives R hits per second (unchanged by design). */
static void sc_t12_rotating_touch(void)
{
    h_scene_base();
    int m = h_add_inline(-32, -32, -32, 32, 32, 32);
    h_ent("{\n\"classname\" \"func_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 1000 24\"\n\"dmg\" \"1\"\n\"spawnflags\" \"17\"\n}\n", m);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    /* next to the rotated bounds (radius sqrt(3) * 32), within touch range */
    h_place(p, -(sqrtf(3.0f) * 32 + 16 + 0.5f), 1000, 24);
    static int hits[H_MAXTICKS];
    int t0 = h_damage_run(p, h_rate, 1, hits);
    CHECK(t0 >= 0 && t0 <= 1, "touching at once (tick %d)", t0);
    CHECK(h_sum(hits, 0, h_rate) == h_rate, "one usercmd per tick: %d hits in 1 s (got %d)", h_rate, h_sum(hits, 0, h_rate));
    h_give(p, "health", 200);
    h_frame();      /* the HUD health stat follows at the end of a frame */
    h_damage_run(p, h_rate, 2, hits);
    CHECK(h_sum(hits, 0, h_rate) == 2 * h_rate, "two usercmds per tick: %d hits in 1 s (got %d)", 2 * h_rate, h_sum(hits, 0, h_rate));
    for (int i = 0; i < h_rate; i++)
        if (hits[i] != 2) { CHECK(0, "two hits in tick %d (got %d)", i, hits[i]); break; }
}

/* TOUCH_PAIN as contact through the real Pmove: the player stands on a
 * spinning platform (the ground trace lists it as a touch entity), a hit
 * per usercmd as t12_rotating_touch (box collision only). Engine-only: the
 * box fixture (not the product collision) gives one extra hit per world
 * tick, R+R for R usercmds and 2R+R for 2R, and would fail the contract */
static void sc_t12_rotating_ride(void)
{
    h_scene_base();
    int m = h_add_inline(-64, -64, -16, 64, 64, 0);
    h_ent("{\n\"classname\" \"func_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 1000 16\"\n\"dmg\" \"1\"\n\"spawnflags\" \"17\"\n}\n", m);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_place(p, 0, 1000, 16 + 24 + 0.25f);
    static int hits[H_MAXTICKS];
    h_damage_run(p, h_rate / 2, 1, hits);    /* lands on the platform */
    h_give(p, "health", 200);
    h_frame();
    int t0 = h_damage_run(p, h_rate, 1, hits);
    CHECK(t0 == 0 && p->groundentity && p->groundentity->classname && !strcmp(p->groundentity->classname, "func_rotating"),
          "standing on the platform, touching at once (tick %d)", t0);
    CHECK(h_sum(hits, 0, h_rate) == h_rate, "one usercmd per tick: %d hits in 1 s (got %d)", h_rate, h_sum(hits, 0, h_rate));
    h_give(p, "health", 200);
    h_frame();
    h_damage_run(p, h_rate, 2, hits);
    CHECK(h_sum(hits, 0, h_rate) == 2 * h_rate, "two usercmds per tick: %d hits in 1 s (got %d)", 2 * h_rate, h_sum(hits, 0, h_rate));
}

/* func_train pushing the player into a wall: a hit every 0.5 s */
static void sc_t12_train_blocked(void)
{
    h_scene_base();
    h_add_world(16.5f, 900, 0, 100, 1100, 200);
    int m = h_add_inline(0, 0, 0, 64, 64, 64);
    h_ent("{\n\"classname\" \"func_train\"\n\"model\" \"*%d\"\n\"target\" \"c1\"\n\"dmg\" \"1\"\n\"spawnflags\" \"1\"\n}\n", m);
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c1\"\n\"target\" \"c2\"\n\"origin\" \"-200 968 0\"\n}\n");
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c2\"\n\"target\" \"c1\"\n\"origin\" \"200 968 0\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_place(p, 0, 1000, 24);
    static int hits[H_MAXTICKS];
    int n = 6 * h_rate;
    int t0 = h_damage_run(p, n, 1, hits);
    CHECK(t0 >= 0, "train blocked by the player (tick %d)", t0);
    if (t0 < 0 || t0 + 2 * h_rate > n) return;
    CHECK(h_sum(hits, t0, t0 + h_rate) == 2, "train: 2 hits in [t0, t0 + 1 s) (got %d)", h_sum(hits, t0, t0 + h_rate));
    CHECK(hits[t0] == 1 && hits[t0 + h_rate / 2] == 1, "hits at t0 and t0 + 0.5 s (%d, %d)", hits[t0], hits[t0 + h_rate / 2]);
}

/* func_door_secret opened by trigger_always, first move (-y) squeezing the
 * player into a wall: a hit every 0.5 s */
static void sc_t12_secret_blocked(void)
{
    h_scene_base();
    h_add_world(-100, 900, 0, 100, 966.5f, 200);
    int m = h_add_inline(-32, 1000, 0, 32, 1064, 128);
    h_ent("{\n\"classname\" \"func_door_secret\"\n\"model\" \"*%d\"\n\"targetname\" \"sd\"\n\"dmg\" \"1\"\n}\n", m);
    h_ent("{\n\"classname\" \"trigger_always\"\n\"target\" \"sd\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_place(p, 0, 1000 - 16 - 0.5f, 24);
    static int hits[H_MAXTICKS];
    int n = 10 * h_rate;
    int t0 = h_damage_run(p, n, 1, hits);
    CHECK(t0 >= 0, "secret door blocked by the player (tick %d)", t0);
    if (t0 < 0 || t0 + h_rate > n) return;
    CHECK(h_sum(hits, t0, t0 + h_rate) == 2, "secret door: 2 hits in [t0, t0 + 1 s) (got %d)", h_sum(hits, t0, t0 + h_rate));
    CHECK(hits[t0] == 1 && hits[t0 + h_rate / 2] == 1, "hits at t0 and t0 + 0.5 s (%d, %d)", hits[t0], hits[t0 + h_rate / 2]);
}

/* falling func_object landing on an idle player: one hit, then it rests
 * (a player sending usercmds: t12_falling_object_moving) */
static edict_t *h_falling_object_scene(void)
{
    h_scene_base();
    int m = h_add_inline(-16, 984, 1000, 16, 1016, 1032);
    h_ent("{\n\"classname\" \"func_object\"\n\"model\" \"*%d\"\n\"dmg\" \"1\"\n}\n", m);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_place(p, 0, 1000, 24);
    h_tick(p, 0, 1);
    return p;
}

static void sc_t12_falling_object(void)
{
    edict_t *p = h_falling_object_scene();
    static int hits[H_MAXTICKS];
    int t0 = h_damage_run(p, 5 * h_rate, 0, hits);
    CHECK(t0 >= 0, "object landed on the player (tick %d)", t0);
    CHECK(h_sum(hits, 0, 5 * h_rate) == 1, "idle player: one hit in 5 s (got %d)", h_sum(hits, 0, 5 * h_rate));
}

/* The same resting object over a player that keeps sending usercmds: each
 * ClientThink relinks the player (linkcount), G_RunFrame drops the object's
 * groundentity and it re-impacts once per world frame (world-driven
 * continuous source): 10 hits in [t0, t0 + 1 s) as at 10 Hz. */
static void sc_t12_falling_object_moving(void)
{
    edict_t *p = h_falling_object_scene();
    static int hits[H_MAXTICKS];
    h_damage_run(p, 5 * h_rate, 0, hits);
    int t0 = h_damage_run(p, 2 * h_rate, 1, hits);
    CHECK(t0 >= 0 && t0 <= 1, "object re-impacts a player sending usercmds at once (tick %d)", t0);
    if (t0 < 0) return;
    CHECK(h_sum(hits, t0, t0 + h_rate) == 10, "player sending usercmds: 10 hits in [t0, t0 + 1 s) (got %d)",
          h_sum(hits, t0, t0 + h_rate));
}

/* rotating_blocked with the first contact in every phase of the 100 ms
 * step: the first hit at once, 10 in [t0, t0 + 1 s) */
static void sc_t12_crush_phases(void)
{
    h_scene_base();
    int m = h_add_inline(-32, -32, -32, 32, 32, 32);
    h_ent("{\n\"classname\" \"func_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 1000 24\"\n\"dmg\" \"1\"\n\"spawnflags\" \"1\"\n}\n", m);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    for (int k = 0; k < h_div; k++) {
        h_place(p, 0, 0, 24);
        h_give(p, "health", 100);
        for (int i = 0; i < h_rate; i++) h_tick(p, 0, 1);
        h_phase(p, k);
        h_place(p, 0, 1000, 24);
        int t0 = h_damage_run(p, 2 * h_rate, 1, hits);
        CHECK(t0 == 0, "phase %d/%d: first hit in the contact tick (tick %d)", k, h_div, t0);
        if (t0 < 0) continue;
        CHECK(h_sum(hits, t0, t0 + h_rate) == 10, "phase %d/%d: 10 hits in [t0, t0 + 1 s) (got %d)",
              k, h_div, h_sum(hits, t0, t0 + h_rate));
    }
}

/* health plus armor right now (the HUD stats of h_hp follow only at the
 * end of a frame; the direct callbacks below are inside one) */
static int h_hp_now(edict_t *p)
{
    int a = ArmorIndex(p);
    return p->health + (a ? p->client->inventory[a] : 0);
}

static edict_t *h_by_class(const char *classname, int nth)
{
    for (int n = 1; n < ge->num_edicts; n++)
        if (EDICT(n)->inuse && !strcmp(EDICT(n)->classname, classname) && !nth--) return EDICT(n);
    return NULL;
}

/* The mod's blocked callbacks called within one world tick, as the
 * physics of several sources would: the limiter is per source/victim pair.
 * Two callbacks of one pair in a tick hit once; another victim and another
 * source hit at once; within 0.1 s the pair does not hit again; a freed
 * source does not pass its cooldown to a new entity in the same slot. */
static void sc_t12_crush_pairs(void)
{
    h_scene_base();
    int m1 = h_add_inline(-32, -32, -32, 32, 32, 32), m2 = h_add_inline(-32, -32, -32, 32, 32, 32);
    h_ent("{\n\"classname\" \"func_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 1000 24\"\n\"dmg\" \"1\"\n\"spawnflags\" \"1\"\n}\n", m1);
    h_ent("{\n\"classname\" \"func_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 -1000 24\"\n\"dmg\" \"1\"\n\"spawnflags\" \"1\"\n}\n", m2);
    h_start(h_ents);
    edict_t *a = h_join(0, "p1");
    h_place(a, 200, 0, 24);         /* off the spawn point before p2 spawns there */
    h_tick(a, 0, 1);
    edict_t *b = h_join(1, "p2");
    edict_t *r1 = h_by_class("func_rotating", 0), *r2 = h_by_class("func_rotating", 1);
    CHECK(r1 && r2 && r1->blocked && r2->blocked, "two rotating sources spawned");
    if (!r1 || !r2 || !r1->blocked) return;
    int hp = h_hp_now(a);
    r1->blocked(r1, a);
    r1->blocked(r1, a);
    CHECK(hp - h_hp_now(a) == 1, "one source, one victim, two callbacks in a tick: one hit (got %d)", hp - h_hp_now(a));
    hp = h_hp_now(b);
    r1->blocked(r1, b);
    CHECK(hp - h_hp_now(b) == 1, "the same source, another victim: hit at once (got %d)", hp - h_hp_now(b));
    hp = h_hp_now(a);
    r2->blocked(r2, a);
    CHECK(hp - h_hp_now(a) == 1, "another source, the same victim: hit at once (got %d)", hp - h_hp_now(a));
    /* a new entity in the freed slot of r2 (the reuse policy allows it in
     * the first seconds of a level) is a new source */
    void (*blocked)(edict_t *, edict_t *) = r2->blocked;
    int slot = NUM(r2);
    G_FreeEdict(r2);
    edict_t *n = G_Spawn();
    CHECK(NUM(n) == slot, "the new entity reuses the freed slot %d (got %d)", slot, NUM(n));
    n->classname = "func_rotating";
    n->dmg = 1;
    n->blocked = blocked;
    hp = h_hp_now(a);
    n->blocked(n, a);
    CHECK(hp - h_hp_now(a) == 1, "a new entity in a freed slot: hit at once, no inherited cooldown (got %d)",
          hp - h_hp_now(a));
    h_frame();
    hp = h_hp_now(a);
    r1->blocked(r1, a);
    int exp = h_div == 1;           /* the next tick is t0 + 0.1 s only at 10 Hz */
    CHECK(hp - h_hp_now(a) == exp, "the pair one tick later: %d hit(s) (got %d)", exp, hp - h_hp_now(a));
}

/* More than MAX_CRUSH pairs active within 0.1 s. MAX_CRUSH filler
 * pairs keep the table full (G_CrushAllowed every tick, as the physics
 * would); further pairs limit per victim: the first hit at once, then at
 * most one per 0.1 s for all sources together; a pair that gets a table slot
 * right after an overflow hit waits for the rest of the victim's 0.1 s; a
 * reused edict and a new map start with an immediate hit. */
static edict_t *h_cfill[MAX_CRUSH];

static void h_crush_spawn_fill(void)
{
    for (int i = 0; i < MAX_CRUSH; i++) h_cfill[i] = G_Spawn();
}

static int h_crush_fill(void)
{
    int active = 0;
    for (int i = 0; i < MAX_CRUSH; i++)
        if (h_cfill[i]->inuse) G_CrushAllowed(h_cfill[i], EDICT(0));
    for (int i = 0; i < MAX_CRUSH; i++) active += level.crush[i].framenum > level.framenum;
    return active;
}

/* N ticks with the table kept full; S1 (and S2) on V: hits, first hit tick,
 * smallest gap between hits */
static int h_crush_run(edict_t *s1, edict_t *s2, edict_t *v, int n, int *first, int *gap)
{
    int hits = 0, last = -1, full = 1;
    *first = -1;
    *gap = 1 << 30;
    for (int t = 0; t < n; t++) {
        full &= h_crush_fill() == MAX_CRUSH;
        for (int k = 0; k < (s2 ? 2 : 1); k++) {
            if (!G_CrushAllowed(k ? s2 : s1, v)) continue;
            hits++;
            if (*first < 0) *first = t;
            if (last >= 0 && t - last < *gap) *gap = t - last;
            last = t;
        }
        h_frame();
    }
    CHECK(full, "the table stays full (%d pairs)", MAX_CRUSH);
    return hits;
}

static void sc_t12_crush_overflow(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    edict_t *s1 = G_Spawn(), *s2 = G_Spawn(), *v[4];
    for (int i = 0; i < 4; i++) v[i] = G_Spawn();
    h_crush_spawn_fill();
    int first, gap, hits;

    /* reused edict (first, while the freed slot is reusable at once) */
    hits = h_crush_run(s1, NULL, v[0], 1, &first, &gap);
    CHECK(hits == 1, "overflow: the first hit at once (%d)", hits);
    int slot = NUM(v[0]);
    G_FreeEdict(v[0]);
    edict_t *n = G_Spawn();
    CHECK(NUM(n) == slot, "the new entity reuses the freed slot %d (got %d)", slot, NUM(n));
    hits = h_crush_run(s1, NULL, n, 1, &first, &gap);
    CHECK(hits == 1, "a reused edict: hit at once, no inherited cooldown (%d)", hits);

    /* one source, then two sources on one victim: 10 hits in 1 s */
    hits = h_crush_run(s1, NULL, v[1], h_rate, &first, &gap);
    CHECK(first == 0 && hits == 10 && gap >= h_div, "one source: first hit tick %d, %d hits in 1 s (10), gap %d ticks (>= %d)",
          first, hits, gap, h_div);
    hits = h_crush_run(s1, s2, v[2], h_rate, &first, &gap);
    CHECK(first == 0 && hits == 10 && gap >= h_div, "two sources: first hit tick %d, %d hits in 1 s (10), gap %d ticks (>= %d)",
          first, hits, gap, h_div);

    /* fallback -> table: an overflow hit, then a free slot the next tick */
    hits = h_crush_run(s1, NULL, v[3], 1, &first, &gap);
    CHECK(hits == 1, "overflow hit before the transition (%d)", hits);
    G_FreeEdict(h_cfill[0]);
    int next = -1, in_table = 0;
    for (int t = 0; t < 2 * h_div && next < 0; t++) {
        h_crush_fill();
        if (G_CrushAllowed(s1, v[3])) next = t + 1;
        h_frame();
    }
    for (int i = 0; i < MAX_CRUSH; i++) in_table |= level.crush[i].self == s1 && level.crush[i].other == v[3];
    CHECK(next == h_div && in_table, "fallback -> table: the next hit %d ticks later (%d), from the table (%d)",
          next, h_div, in_table);

    /* new map: a client hit by the overflow starts with an immediate hit */
    h_cfill[0] = G_Spawn();
    hits = h_crush_run(s1, NULL, p, 1, &first, &gap);
    CHECK(hits == 1, "overflow hit on the client before the new map (%d)", hits);
#ifdef HARNESS_ENGINE
    he_set_game(ge);    /* SV_ClearWorld before SpawnEntities, as SV_SpawnServer */
#endif
    ge->SpawnEntities("harness", h_ents, "");
    ge->ClientBegin(p);
    h_frame();
    s1 = G_Spawn();
    h_crush_spawn_fill();
    hits = h_crush_run(s1, NULL, p, 1, &first, &gap);
    CHECK(hits == 1, "new map: the overflow hits the client at once (%d)", hits);

    /* table -> fallback -> table: A->W enters the last free slot; on the tick
     * before its entry expires B hits W through the overflow; A's next hit
     * comes 0.1 s after B's, not on the tick its entry expires */
    edict_t *a = G_Spawn(), *b = G_Spawn(), *w = G_Spawn();
    G_FreeEdict(h_cfill[1]);
    int t0 = -1, tb = -1, ta = -1, full = 1;
    for (int t = 0; t < 6 * h_div && ta < 0; t++) {
        int active = h_crush_fill();
        if (t0 < 0 && active < MAX_CRUSH && G_CrushAllowed(a, w)) t0 = level.framenum;
        if (t0 >= 0 && tb < 0 && level.framenum == t0 + h_div - 1) {
            full &= h_crush_fill() == MAX_CRUSH;
            if (G_CrushAllowed(b, w)) tb = level.framenum;
        } else if (tb >= 0 && G_CrushAllowed(a, w)) {
            ta = level.framenum;
        }
        h_frame();
    }
    CHECK(t0 >= 0 && tb == t0 + h_div - 1 && full, "A->W in the full table at frame %d, B hits W through the overflow at %d",
          t0, tb);
    CHECK(ta - tb == h_div, "table -> fallback -> table: A hits W %d ticks after B (%d)", ta - tb, h_div);
}

/* A door blocked again before its next damage is due still reverses
 * (the damage limiter does not gate the physical reaction): blocked while
 * opening it closes; blocked again as soon as it moves back (within 0.1 s
 * at 60 Hz) it takes no damage but opens again. */
static void sc_t12_crush_reverse(void)
{
    h_door_scene(0);
    edict_t *p = h_join(0, "p1");
    edict_t *door = h_by_model("*1");
    h_place(p, -100, 1070, 24);     /* in the door's trigger field, not in its path */
    float x0 = door->s.origin[0];
    for (int i = 0; i < 4 && door->s.origin[0] == x0; i++) h_tick(p, 0, 1);
    CHECK(door->s.origin[0] > x0, "door opening (+x): %.3f > %.3f", door->s.origin[0], x0);
    h_place(p, -400, 1070, 24);     /* out of the trigger field: it would reopen the door */
    h_tick(p, 0, 1);
    int hp = h_hp_now(p);
    door->blocked(door, p);
    CHECK(hp - h_hp_now(p) == 1, "block: one hit (got %d)", hp - h_hp_now(p));
    float xs = door->s.origin[0];
    int k = 0;
    while (k < 2 * h_rate && door->s.origin[0] == xs) { h_tick(p, 0, 0); k++; }
    CHECK(door->s.origin[0] < xs, "block: the door closes (x %.3f < %.3f after %d ticks)", door->s.origin[0], xs, k);
    hp = h_hp_now(p);
    door->blocked(door, p);
    int exp = k >= h_div;           /* hit again only from t0 + 0.1 s on */
    CHECK(hp - h_hp_now(p) == exp, "blocked again %d ticks later: %d hit(s) (got %d)", k, exp, hp - h_hp_now(p));
    xs = door->s.origin[0];
    for (int i = 0; i < 2 * h_rate && door->s.origin[0] <= xs; i++) h_tick(p, 0, 0);
    CHECK(door->s.origin[0] > xs, "blocked again: the door opens again (x %.3f > %.3f)", door->s.origin[0], xs);
}

/* ---------------------------------------------------------------- T13/T14 */

/* T13/T14 run only on the engine collision fixture (H_ENGINE). A mover's
 * path, one entry per world tick: the position relative to the anchor (the
 * origin at h_path_begin), the velocity the pusher applies in that tick
 * (set by the previous think) and the ideal position (its integral). */
#define H_VZERO(v) (!(v)[0] && !(v)[1] && !(v)[2])
#define H_EPS 1e-3f

typedef struct {
    edict_t *e;
    vec3_t o0;
    int n, f0;
    float pos[H_MAXTICKS][3], vel[H_MAXTICKS][3];
    double ideal[H_MAXTICKS][3];
} hpath_t;

static void h_path_begin(hpath_t *t, edict_t *e)
{
    t->e = e;
    t->n = 0;
    t->f0 = h_frames;
    VectorCopy(e->s.origin, t->o0);
}

/* one world tick, with a usercmd of `ply` before it when given */
static void h_path_tick(hpath_t *t, edict_t *ply)
{
    int i = t->n++;
    if (i >= H_MAXTICKS) imp_error("path longer than %d ticks", H_MAXTICKS);
    VectorCopy(t->e->velocity, t->vel[i]);
    if (ply) h_tick_sched(ply, 0);
    else h_frame();
    for (int k = 0; k < 3; k++) {
        t->pos[i][k] = t->e->s.origin[k] - t->o0[k];
        t->ideal[i][k] = (i ? t->ideal[i - 1][k] : 0) + (double)t->vel[i][k] / h_rate;
    }
}

/* the next commanded movement from tick `from`: ticks [*a, *b]; 0 when
 * there is none or it does not end within the record */
static int h_path_seg(const hpath_t *t, int from, int *a, int *b)
{
    for (*a = from; *a < t->n && H_VZERO(t->vel[*a]); (*a)++) {}
    for (*b = *a; *b + 1 < t->n && !H_VZERO(t->vel[*b + 1]); (*b)++) {}
    return *b + 1 < t->n;
}

/* sounds `idx` of entity `e` in frames [f0, f1) */
static int h_sounds_in(const edict_t *e, int idx, int f0, int f1)
{
    int c = 0;
    for (int i = 0; i < h_nsounds; i++)
        c += h_sounds_log[i].entnum == NUM(e) && h_sounds_log[i].soundindex == idx &&
             h_sounds_log[i].frame >= f0 && h_sounds_log[i].frame < f1;
    return c;
}

/* One movement [a, b], anchored at the position before a: after each tick
 * within 1/8 per axis of the ideal path; endpoint within 1/8 per axis of
 * `end` (relative to the path anchor); no step longer than vmax / R + 1/8
 * per axis (no large final jump); one endfunc: sound `endsnd` once from a
 * to the next movement. Returns the duration in ticks. */
static int h_path_check(const char *what, const hpath_t *t, int a, int b, const vec3_t end, const vec3_t vmax, int endsnd)
{
    float maxerr = 0, enderr = 0, excess = -1e9f, step = 0, lim = 0;
    int worst = -1, next, dummy;
    for (int i = a; i <= b; i++)
        for (int k = 0; k < 3; k++) {
            double base = a ? t->pos[a - 1][k] - t->ideal[a - 1][k] : 0;
            float err = (float)fabs(t->pos[i][k] - t->ideal[i][k] - base);
            float s = fabsf(t->pos[i][k] - (i ? t->pos[i - 1][k] : 0));
            if (err > maxerr) { maxerr = err; worst = i - a + 1; }
            if ((vmax[k] > 0 || s > 0) && s - vmax[k] / h_rate > excess) { excess = s - vmax[k] / h_rate; step = s; lim = vmax[k] / h_rate + 0.125f; }
        }
    for (int k = 0; k < 3; k++) enderr = fmaxf(enderr, fabsf(t->pos[b][k] - end[k]));
    if (h_verbose)
        for (int i = a; i <= b; i++)
            printf("    %s[%d] = %.4f %.4f %.4f  v %.3f %.3f %.3f\n", what, i - a, t->pos[i][0], t->pos[i][1], t->pos[i][2],
                   t->vel[i][0], t->vel[i][1], t->vel[i][2]);
    CHECK(maxerr <= 0.125f + H_EPS, "%s: path error after each step <= 1/8 (max %.4f at step %d)", what, maxerr, worst);
    CHECK(enderr <= 0.125f + H_EPS, "%s: endpoint within 1/8 per axis (off by %.4f)", what, enderr);
    CHECK(excess <= 0.125f + H_EPS, "%s: no large final jump: largest step %.4f <= %.4f", what, step, lim);
    h_path_seg(t, b + 1, &next, &dummy);
    int ends = h_sounds_in(t->e, endsnd, t->f0 + a, t->f0 + next);
    CHECK(ends == 1, "%s: one endfunc (got %d)", what, ends);
    return b - a + 1;
}

/* linear duration from the first movement: dist / speed +- (1/R + 0.125 / speed) */
static void h_linear_duration(const char *what, int ticks, float dist, float speed)
{
    float dur = (float)ticks / h_rate, tol = 1.0f / h_rate + 0.125f / speed;
    CHECK(fabsf(dur - dist / speed) <= tol + 1e-4f, "%s: duration %.4f s = %.4f s +- %.4f", what, dur, dist / speed, tol);
}

/* Linear func_door (accel = decel = speed; key 50, doubled in deathmatch:
 * 100 units/s) of sx * sy units moving along `angle`, opened by
 * trigger_always: the open path, the 3 s wait and the return path (a new
 * path, anchored at the top), each checked by h_path_check, with the
 * linear duration. */
static void h_t13_door(int angle, float sx, float sy, int lip)
{
    h_scene_base();
    int m = h_add_inline(0, 2000, 0, sx, 2000 + sy, 128);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"%d\"\n\"lip\" \"%d\"\n\"speed\" \"50\"\n\"targetname\" \"d\"\n}\n",
          m, angle, lip);
    h_ent("{\n\"classname\" \"trigger_always\"\n\"target\" \"d\"\n}\n");
    h_start(h_ents);
    edict_t *door = h_by_model("*1");
    CHECK(door != NULL, "door spawned");
    if (!door) return;
    const float speed = 100;
    static hpath_t t;
    h_path_begin(&t, door);
    for (int i = 0; i < 7 * h_rate; i++) h_path_tick(&t, NULL);
    vec3_t open, home = { 0, 0, 0 }, vmax;
    VectorSubtract(door->pos2, door->pos1, open);
    for (int k = 0; k < 3; k++) vmax[k] = fabsf(door->movedir[k]) * speed;
    float dist = VectorLength(open);
    int a, b;
    if (!h_path_seg(&t, 0, &a, &b)) { CHECK(0, "door opened and stopped"); return; }
    h_linear_duration("open", h_path_check("open", &t, a, b, open, vmax, door->moveinfo.sound_end), dist, speed);
    if (!h_path_seg(&t, b + 1, &a, &b)) { CHECK(0, "door closed again"); return; }
    h_linear_duration("return", h_path_check("return", &t, a, b, home, vmax, door->moveinfo.sound_end), dist, speed);
}

/* 100 units at 100 units/s, both directions: +x, then back */
static void sc_t13_linear_door(void) { h_t13_door(0, 108, 64, 8); }
/* -x, then back */
static void sc_t13_door_negative(void) { h_t13_door(180, 108, 64, 8); }
/* 45 degrees: 0.7071 * (108 + 108) - 8 = 144.7 units, off the 1/8 grid
 * per step (at 10 Hz too) */
static void sc_t13_door_diagonal(void) { h_t13_door(45, 108, 108, 8); }
/* 1 unit: shorter than one step at any rate */
static void sc_t13_door_short(void) { h_t13_door(0, 9, 64, 8); }

/* Team push transaction: two crusher func_door teams (+x, 100 units at 98
 * units/s: a 1/8 remainder per step at 10 and 60 Hz) opened in the same
 * tick. Team A's first part (master, moved first) carries a rider, its
 * second part is blocked by a player against a wall. While blocked nothing
 * of team A moves: positions and carry are rolled back. After unblock team
 * A replays team B (same x geometry, no obstacle) exactly, delayed by the
 * blocked ticks, and reaches the endpoint with one endfunc. */
static void sc_t13_team_block(void)
{
    h_scene_base();
    h_add_world(192.5f, 2150, 0, 300, 2300, 200);      /* the wall in front of A2 */
    static const float y[4] = { 2000, 2200, 3000, 3200 };
    for (int i = 0; i < 4; i++) {
        int m = h_add_inline(0, y[i], 0, 108, y[i] + 64, i & 1 ? 64 : 16);
        h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"0\"\n\"speed\" \"49\"\n\"dmg\" \"1\"\n"
              "\"spawnflags\" \"4\"\n\"team\" \"t%c\"\n\"targetname\" \"t%c\"\n}\n", m, "ab"[i / 2], "ab"[i / 2]);
    }
    h_start(h_ents);
    edict_t *a1 = h_by_model("*1"), *a2 = h_by_model("*2"), *b1 = h_by_model("*3"), *b2 = h_by_model("*4");
    CHECK(a1 && a2 && b1 && b2 && a1->teamchain == a2 && b1->teamchain == b2, "two teams of two doors");
    if (!(a1 && a2 && b1 && b2)) return;
    edict_t *p1 = h_join(0, "p1");
    h_place(p1, 54, 2032, 40);
    h_tick(p1, 0, 1);
    edict_t *p2 = h_join(1, "p2");     /* after p1 left the shared spawn point */
    h_place(p2, 160, 2232, 24);
    h_tick(p2, 0, 1);
    h_think(p1, 0, 1000 / h_rate);
    h_tick(p2, 0, 1);
    CHECK(p1->groundentity == a1, "the rider stands on A1");
    a1->use(a1, p1, p1);
    b1->use(b1, p1, p1);
    static float ax1[H_MAXTICKS], ax2[H_MAXTICKS], bx1[H_MAXTICKS], bx2[H_MAXTICKS], ride[H_MAXTICKS];
    static int isblk[H_MAXTICKS];
    static hpath_t t;
    int n = 3 * h_rate, blocked = 0, unblock = h_rate / 4 + 1, rollback_bad = -1, carry2_bad = -1, sa = -1, sb = -1;
    vec3_t carry0 = { 0, 0, 0 }, carry2 = { 0, 0, 0 };
    float ride0 = p1->s.origin[0] - a1->s.origin[0];
    h_path_begin(&t, b1);
    for (int i = 0; i < n; i++) {
        vec3_t c, c2;
        float x1 = a1->s.origin[0], x2 = a2->s.origin[0], px = p1->s.origin[0];
        VectorCopy(a1->push_carry, c);
        VectorCopy(a2->push_carry, c2);
        if (blocked == unblock) h_place(p2, 160, 2500, 24);    /* out of A2's way */
        h_think(p2, 0, 1000 / h_rate);
        h_path_tick(&t, p1);
        ax1[i] = a1->s.origin[0]; ax2[i] = a2->s.origin[0]; bx1[i] = b1->s.origin[0]; bx2[i] = b2->s.origin[0];
        ride[i] = p1->s.origin[0] - a1->s.origin[0];
        if (h_verbose)
            printf("    team[%d] A %.4f %.4f B %.4f %.4f rider %.4f %.4f %.4f ground %d carry %.3f\n", i, ax1[i], ax2[i], bx1[i], bx2[i],
                   p1->s.origin[0], p1->s.origin[1], p1->s.origin[2], p1->groundentity ? NUM(p1->groundentity) : -1, a1->push_carry[0]);
        if (sa < 0 && ax1[i] != x1) sa = i;
        if (sb < 0 && bx1[i] != t.o0[0]) sb = i;
        isblk[i] = sa >= 0 && ax1[i] == x1 && !H_VZERO(a1->velocity);
        if (isblk[i]) {
            if (!blocked) { VectorCopy(c, carry0); VectorCopy(c2, carry2); }
            if (rollback_bad < 0 && (ax2[i] != x2 || p1->s.origin[0] != px || !VectorCompare(a1->push_carry, carry0))) rollback_bad = i;
            if (carry2_bad < 0 && !VectorCompare(a2->push_carry, carry2)) carry2_bad = i;
            blocked++;
        }
    }
    CHECK(sa >= 0 && sa == sb, "A1 and B1 start in the same tick (%d, %d)", sa, sb);
    CHECK(blocked == unblock, "A blocked by p2 for %d ticks (got %d)", unblock, blocked);
    CHECK(!VectorCompare(carry0, vec3_origin) && !VectorCompare(carry2, vec3_origin), "nonzero carry of A1 and A2 at the block (%.3f, %.3f)",
          carry0[0], carry2[0]);
    CHECK(rollback_bad < 0, "blocked tick %d: A2, the rider and A1's carry unchanged", rollback_bad);
    CHECK(carry2_bad < 0, "blocked tick %d: A2's carry unchanged (rolled back with the team)", carry2_bad);
    float maxride = 0;
    for (int i = 0; i < n; i++) maxride = fmaxf(maxride, fabsf(ride[i] - ride0));
    CHECK(maxride <= H_EPS, "the rider moves with A1 (offset change %.4f)", maxride);
    /* team A without its blocked ticks == team B */
    int j = 0, bad = -1;
    for (int i = 0; i < n; i++) {
        if (isblk[i]) continue;
        if (bad < 0 && (ax1[i] != bx1[j] || ax2[i] != bx2[j])) bad = i;
        j++;
    }
    CHECK(bad < 0, "after unblock team A replays team B (first difference at tick %d)", bad);
    int a, b;
    vec3_t end = { 100, 0, 0 }, vmax = { 98, 0, 0 };
    if (!h_path_seg(&t, 0, &a, &b)) { CHECK(0, "B1 stopped"); return; }
    h_linear_duration("B1", h_path_check("B1", &t, a, b, end, vmax, b1->moveinfo.sound_end), 100, 98);
    CHECK(fabsf(ax1[n - 1] - t.o0[0] - 100) <= 0.125f, "A1 endpoint %.4f within 1/8 of 100", ax1[n - 1] - t.o0[0]);
    CHECK(h_sounds_in(a1, a1->moveinfo.sound_end, t.f0, t.f0 + n) == 1, "A: one endfunc (got %d)",
          h_sounds_in(a1, a1->moveinfo.sound_end, t.f0, t.f0 + n));
}

/* Carry lifecycle: a door (98 units/s) blocked by a player reverses; the
 * reversed path starts without the old carry (reset in Move_Calc); a freed
 * entity (reused by G_Spawn) has none (G_FreeEdict clears the edict, as
 * SpawnEntities on a map change and the OpenTDM restart do). */
static void sc_t13_carry_reset(void)
{
    h_scene_base();
    h_add_world(128.5f, 1900, 0, 200, 2100, 200);
    int m = h_add_inline(-200, 1968, 0, 40, 2032, 128);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"0\"\n\"speed\" \"49\"\n\"dmg\" \"1\"\n\"targetname\" \"d\"\n}\n", m);
    h_start(h_ents);
    edict_t *door = h_by_model("*1"), *p = h_join(0, "p1");
    h_place(p, 96, 2000, 24);
    h_tick(p, 0, 1);
    door->use(door, p, p);
    int rev = -1, carried = 0;
    for (int i = 0; i < 4 * h_rate && rev < 0; i++) {
        float v = door->velocity[0];
        carried |= !VectorCompare(door->push_carry, vec3_origin);
        h_tick(p, 0, 1);
        if (v > 0 && door->velocity[0] < 0) rev = i;
    }
    CHECK(carried, "nonzero carry while opening");
    CHECK(rev >= 0, "blocked door reversed (tick %d)", rev);
    CHECK(VectorCompare(door->push_carry, vec3_origin), "reversed: new path without the old carry (%.3f)", door->push_carry[0]);
    for (int i = 0; i < 4 && VectorCompare(door->push_carry, vec3_origin); i++) h_tick(p, 0, 1);
    CHECK(!VectorCompare(door->push_carry, vec3_origin), "nonzero carry while closing");
    G_FreeEdict(door);
    CHECK(VectorCompare(door->push_carry, vec3_origin), "freed entity: no carry for its reuse");
}

/* Carry on a train teleport: a func_train (98 units/s) arrives at c2
 * with a remainder (OpenFFA ~0.4 units; OpenTDM's final step leaves only
 * float noise, ~1e-4, hence the exact zero below), then c3 (path_corner
 * TELEPORT) puts it at c3 and its path to c4 starts without the old carry
 * (Move_Calc) */
static void sc_t13_train_teleport(void)
{
    h_scene_base();
    int m = h_add_inline(0, 0, 0, 64, 64, 64);
    h_ent("{\n\"classname\" \"func_train\"\n\"model\" \"*%d\"\n\"target\" \"c1\"\n\"speed\" \"98\"\n}\n", m);
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c1\"\n\"target\" \"c2\"\n\"origin\" \"0 2000 0\"\n}\n");
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c2\"\n\"target\" \"c3\"\n\"origin\" \"110 2000 0\"\n}\n");
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c3\"\n\"target\" \"c4\"\n\"spawnflags\" \"1\"\n\"origin\" \"1000 2000 0\"\n}\n");
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c4\"\n\"origin\" \"1101 2000 0\"\n}\n");
    h_start(h_ents);
    edict_t *train = h_by_model("*1");
    CHECK(train != NULL, "train spawned");
    if (!train) return;
    int tele = -1;
    vec3_t before = { 0, 0, 0 };
    for (int i = 0; i < 4 * h_rate && tele < 0; i++) {
        float x = train->s.origin[0];
        VectorCopy(train->push_carry, before);
        h_frame();
        if (train->s.origin[0] - x > 500) tele = i;
    }
    CHECK(tele >= 0, "train teleported to c3 (tick %d, x %.3f)", tele, train->s.origin[0]);
    CHECK(!VectorCompare(before, vec3_origin), "nonzero carry before the teleport (%.3f)", before[0]);
    CHECK(VectorCompare(train->push_carry, vec3_origin) && train->s.origin[0] == 1000 && !H_VZERO(train->velocity),
          "teleport tick: at c3 (x %.3f), moving to c4 without the old carry (%g)", train->s.origin[0], train->push_carry[0]);
}

/* Carry on a map change (SpawnEntities) and the OpenTDM match restart
 * (TDM_ResetLevel: the entity string again over the same edicts): the door
 * moving with a remainder starts its new map at rest without it */
static void sc_t13_carry_map(void)
{
    h_scene_base();
    int m = h_add_inline(0, 2000, 0, 108, 2064, 128);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"0\"\n\"speed\" \"49\"\n\"targetname\" \"d\"\n}\n", m);
    h_ent("{\n\"classname\" \"trigger_always\"\n\"target\" \"d\"\n}\n");
    h_start(h_ents);
    static const char *const how[2] = { "map change", "OpenTDM restart" };
#ifdef HARNESS_OPENTDM
    const int ways = 2;
#else
    const int ways = 1;
#endif
    for (int w = 0; w < ways; w++) {
        edict_t *door = h_by_model("*1");
        CHECK(door != NULL, "door spawned");
        if (!door) return;
        float x0 = door->s.origin[0];
        for (int i = 0; i < 2 * h_rate && VectorCompare(door->push_carry, vec3_origin); i++) h_frame();
        h_frames_n(h_rate / 2);
        CHECK(!VectorCompare(door->push_carry, vec3_origin) && door->s.origin[0] != x0, "door moving with a carry (%.3f, x %+.3f)",
              door->push_carry[0], door->s.origin[0] - x0);
        int n = NUM(door);
#ifdef HARNESS_OPENTDM
        if (w) TDM_ResetLevel();
        else
#endif
            ge->SpawnEntities("harness", h_ents, "");
        door = h_by_model("*1");
        CHECK(door && NUM(door) == n && VectorCompare(door->push_carry, vec3_origin) && door->s.origin[0] == x0,
              "%s: the door (edict %d) at rest without the old carry (%.3f, x %+.3f)", how[w], door ? NUM(door) : -1,
              door ? door->push_carry[0] : 0, door ? door->s.origin[0] - x0 : 0);
    }
}

/* T14: func_plat (defaults: 200 units/s, 500 units/s^2 in both mods) at
 * the bottom, raised by its rider touching the trigger: height 64 (TDM: a
 * nonzero final remainder) or a zero path (lip = model height: the zero
 * remainder). The rider stays on it; checked by h_path_check against the
 * velocity profile; speed within 200 units/s; one endfunc before the 3 s
 * wait. Duration from the first movement (the dispatch tick is not part
 * of it): OpenTDM keeps its 100 ms controller, every speed held for 0.1 s
 * including the final remainder, 7 intervals as at 10 Hz; OpenFFA (dt
 * units per tick) within one 100 ms step of its 10 Hz duration. */
#define H_T14_TICKS10 7     /* the 10 Hz duration of both mods on master */

static void h_t14_plat(int zero)
{
    h_scene_base();
    int m = h_add_inline(-64, 1936, 0, 64, 2064, 64);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"%s\" \"64\"\n}\n", m, zero ? "lip" : "height");
    h_start(h_ents);
    edict_t *plat = h_by_model("*1"), *p = h_join(0, "p1");
    CHECK(plat != NULL, "plat spawned");
    if (!plat) return;
    h_place(p, 0, 2000, plat->absmax[2] + 24);
    h_tick(p, 0, 1);
    static hpath_t t;
    float ride0 = 0, maxride = 0, vmaxz = 0;
    int land = -1;
    h_path_begin(&t, plat);
    for (int i = 0; i < 2 * h_rate; i++) {
        h_path_tick(&t, p);
        /* the rider is placed a little above the plat and falls onto it; when
         * it lands depends on the rate and the command schedule (from 1 to 6
         * ticks) and is not under test. From that tick on its offset is fixed. */
        if (land < 0 && p->groundentity == plat) {
            land = i;
            ride0 = p->s.origin[2] - plat->s.origin[2];
        }
        if (land >= 0) maxride = fmaxf(maxride, fabsf(p->s.origin[2] - plat->s.origin[2] - ride0));
        if (h_verbose) printf("    rider[%d] = %.4f (raw %.4f, plat %.4f, ground %d)\n", i, p->s.origin[2] - plat->s.origin[2] - ride0,
                              p->s.origin[2] - plat->s.origin[2], plat->s.origin[2], p->groundentity == plat);
        vmaxz = fmaxf(vmaxz, fabsf(plat->velocity[2]));
    }
    int ends = h_sounds_in(plat, plat->moveinfo.sound_end, t.f0, t.f0 + t.n);
    if (zero) {
        int moved = 0;
        for (int i = 0; i < t.n; i++) moved |= !H_VZERO(t.pos[i]);
        CHECK(!moved, "zero path: no movement");
        CHECK(ends == 1, "zero path: one endfunc (got %d)", ends);
        return;
    }
    CHECK(vmaxz <= 200 + H_EPS, "speed within 200 units/s (max %.3f)", vmaxz);
    int a, b;
    vec3_t end = { 0, 0, 64 }, vmax = { 0, 0, 200 };
    if (!h_path_seg(&t, 0, &a, &b)) { CHECK(0, "plat rose and stopped"); return; }
    CHECK(land >= 0 && land < b, "the rider lands on the plat while it rises (tick %d, the rise ends at %d)", land, b);
    CHECK(maxride <= H_EPS, "from then on the rider moves with the plat (offset change %.4f)", maxride);
    int d = h_path_check("plat", &t, a, b, end, vmax, plat->moveinfo.sound_end);
#ifdef HARNESS_OPENTDM
    CHECK(d == H_T14_TICKS10 * h_div, "duration %d ticks = %d controller intervals of 100 ms", d, H_T14_TICKS10);
    int held = 1;
    for (int i = a; i <= b; i++)
        held &= VectorCompare(t.vel[i], t.vel[a + (i - a) / h_div * h_div]);
    CHECK(held, "each speed (the final remainder too) held for one 100 ms interval");
#else
    CHECK(abs(d - H_T14_TICKS10 * h_div) <= h_div, "duration %.3f s within 0.1 s of the 10 Hz %.1f s",
          (float)d / h_rate, H_T14_TICKS10 / 10.0f);
#endif
}

static void sc_t14_plat(void) { h_t14_plat(0); }
static void sc_t14_plat_zero(void) { h_t14_plat(1); }

/* T14 rider on a func_plat going down, the rider's
 * usercmds by HARNESS_SCHED. After each world tick from the departure: the
 * rider's ground is the plat, its height relative to the plat within 1/8 of
 * the settled one, no EV_FALL* and no health lost; the plat plays one start
 * and one end sound per leg, its loop sound is off while it stands, and the
 * durations of the legs of one direction differ by at most one tick.
 * cycles 0: a targeted plat at the top, used once, goes down 512 units;
 * cycles n: a PLAT_LOW_TRIGGER plat (the rider at
 * the top stays out of the trigger) carries its rider up and down n times. */
#define H_STATE_TOP 0
#define H_STATE_BOTTOM 1
#define H_STATE_UP 2
#define H_STATE_DOWN 3

static void h_t14_ride(int cycles)
{
    h_scene_base();
    if (cycles) {
        int m = h_add_inline(-64, 1936, 128, 64, 2064, 192);
        h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"128\"\n\"spawnflags\" \"1\"\n}\n", m);
    } else {
        int m = h_add_inline(-64, 1936, 512, 64, 2064, 576);
        h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"512\"\n\"targetname\" \"plat\"\n}\n", m);
    }
    h_start(h_ents);
    edict_t *plat = h_by_model("*1"), *p = h_join(0, "rider");
    CHECK(plat != NULL, "plat spawned");
    if (!plat) return;
    h_place(p, 0, 2000, plat->absmax[2] - 1 + 24);       /* on the top face (absmax has 1 unit slop) */
    /* the low trigger starts the plat in the placing usercmd: its first leg
     * begins before tick 0 */
    int f0 = h_frames;
    for (int i = 0; i < (cycles ? 1 : h_rate / 2); i++) h_tick(p, 0, 1);
    CHECK(p->groundentity == plat, "the rider settled on the plat");
    if (!cycles) plat->use(plat, p, p);
    float ride0 = p->s.origin[2] - plat->s.origin[2], maxride = 0;
    int hp0 = h_hp(p), falls0 = h_falls, lost = 0, first_lost = -1, loud = 0;
    int state = plat->moveinfo.state, legs = state == H_STATE_UP || state == H_STATE_DOWN, start = -1, dur[2][16], nd[2] = { 0, 0 };
    int want = cycles ? 2 * cycles : 1;
    for (int i = 0; i < 60 * h_rate; i++) {
        h_tick_sched(p, 0);
        int st = plat->moveinfo.state;
        if (st != state && (st == H_STATE_UP || st == H_STATE_DOWN)) { legs++; start = i; }
        if (st != state && (st == H_STATE_TOP || st == H_STATE_BOTTOM) && nd[st] < 16) dur[st][nd[st]++] = i - start;
        state = st;
        float off = fabsf(p->s.origin[2] - plat->s.origin[2] - ride0);
        maxride = fmaxf(maxride, off);
        if (p->groundentity != plat) { lost++; if (first_lost < 0) first_lost = i; }
        loud += (st == H_STATE_TOP || st == H_STATE_BOTTOM) && plat->s.sound;
        if (h_verbose)
            printf("    ride[%d] state %d plat %.3f off %.3f ground %d hp %d falls %d\n", i, st, plat->s.origin[2], off,
                   p->groundentity ? NUM(p->groundentity) : -1, h_hp(p), h_falls - falls0);
        if (legs == want && st == H_STATE_BOTTOM) break;
    }
    int starts = h_sounds_in(plat, plat->moveinfo.sound_start, f0, h_frames);
    int ends = h_sounds_in(plat, plat->moveinfo.sound_end, f0, h_frames);
    CHECK(legs == want && state == H_STATE_BOTTOM, "%d legs, the plat stopped at the bottom (got %d, state %d)", want, legs, state);
    CHECK(!lost, "the rider's ground is the plat after every tick (%d ticks without, first %d)", lost, first_lost);
    CHECK(maxride <= 0.125f + H_EPS, "the rider's height on the plat within 1/8 (max %.4f)", maxride);
    CHECK(h_hp(p) == hp0 && h_falls == falls0, "no damage, no fall event (health %d -> %d, %d events)", hp0, h_hp(p), h_falls - falls0);
    CHECK(starts == legs && ends == legs, "one start and one end sound per leg (%d legs: %d, %d)", legs, starts, ends);
    CHECK(!loud, "no loop sound while the plat stands (%d ticks)", loud);
    for (int k = 0; k < 2; k++) {
        int lo = 1 << 30, hi = -1;
        for (int j = 0; j < nd[k]; j++) { lo = dur[k][j] < lo ? dur[k][j] : lo; hi = dur[k][j] > hi ? dur[k][j] : hi; }
        if (nd[k] > 1)
            CHECK(hi - lo <= 1, "%s legs: durations within one tick (%d..%d ticks over %d legs)", k ? "down" : "up", lo, hi, nd[k]);
    }
}

static void sc_t14_plat_down(void) { h_t14_ride(0); }
static void sc_t14_plat_cycles(void) { h_t14_ride(5); }

/* T13 rollback of a team push with an idle rider:
 * a two-part team going down; A carries p1, B
 * is blocked by p2 against a world box, so the team rolls back every tick;
 * p1 sends no usercmd at all (no fresh ClientThink refreshes its ground).
 * p1 keeps the team leader as ground through the rollback and, after p2
 * leaves, through the move down; no damage and no fall event for p1 */
static void sc_t13_team_down_idle(void)
{
    h_scene_base();
    h_add_world(-64, 2150, 400, 172, 2300, 456);
    for (int i = 0; i < 2; i++) {
        float y = 2000 + 200 * i;
        int m = h_add_inline(0, y, 512, 108, y + 64, 620);
        h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"-2\"\n\"speed\" \"49\"\n\"dmg\" \"1\"\n"
              "\"spawnflags\" \"4\"\n\"team\" \"down\"\n\"targetname\" \"down\"\n}\n", m);
    }
    h_start(h_ents);
    edict_t *a = h_by_model("*1"), *b = h_by_model("*2");
    CHECK(a && b && a->teamchain == b, "two downward doors form one team");
    if (!(a && b)) return;
    edict_t *p1 = h_join(0, "rider");
    h_place(p1, 54, 2032, 645);
    h_tick(p1, 0, 1);
    edict_t *p2 = h_join(1, "blocker");
    h_place(p2, 54, 2232, 480);
    h_tick(p2, 0, 1);
    for (int i = 0; i < h_rate / 2; i++) {
        h_think(p1, 0, 1000 / h_rate);
        h_tick(p2, 0, 1);
    }
    CHECK(p1->groundentity == a, "p1 settled on the team leader");
    float az = a->s.origin[2], bz = b->s.origin[2];
    float ride0 = p1->s.origin[2] - az, maxride = 0;
    int lost = 0, blocked_moves = 0, unblock = h_rate / 2, hp0 = h_hp(p1), falls0 = h_falls;
    a->use(a, p1, p1);
    for (int i = 0; i < 2 * h_rate; i++) {
        if (i == unblock) {
            h_place(p2, 400, 2232, 24);
            h_think(p2, 0, 1000 / h_rate);
        }
        h_frame();
        lost += p1->groundentity != a;
        maxride = fmaxf(maxride, fabsf(p1->s.origin[2] - a->s.origin[2] - ride0));
        if (i < unblock && (a->s.origin[2] != az || b->s.origin[2] != bz)) blocked_moves++;
        if (h_verbose)
            printf("    team-down[%d] a %.3f b %.3f rider %.3f ground %d hp %d\n", i, a->s.origin[2], b->s.origin[2],
                   p1->s.origin[2], p1->groundentity ? NUM(p1->groundentity) : -1, h_hp(p1));
    }
    CHECK(!blocked_moves, "the blocked team rolls back completely (%d moved ticks)", blocked_moves);
    CHECK(!lost && maxride <= 0.125f + H_EPS, "the idle rider keeps its ground through rollback and unblock (%d ticks without, height %.4f)",
          lost, maxride);
    CHECK(h_hp(p1) == hp0 && h_falls == falls0, "no damage, no fall event for the rider (health %d -> %d, %d events)",
          hp0, h_hp(p1), h_falls - falls0);
    CHECK(fabsf(a->s.origin[2] - az + 100) <= 0.125f && fabsf(b->s.origin[2] - bz + 100) <= 0.125f,
          "both doors reach the destination 100 units down (%.3f, %.3f)", a->s.origin[2] - az, b->s.origin[2] - bz);
}

/* ---------------------------------------------------------------- T15 */

#ifdef HARNESS_OPENTDM
static const char *h_plain(const char *s)
{
    static char b[128];
    int i;
    for (i = 0; s[i] && i < 127; i++) b[i] = s[i] & 0x7f;
    b[i] = 0;
    return b;
}

/* a TDM configstring as a client sees it at the end of frame f */
static const char *h_cs_at(int num, int f)
{
    const char *t = NULL;
    for (int i = 0; i < h_ncstrings && h_cstrings[i].frame <= f; i++)
        if (h_cstrings[i].num == num) t = h_cstrings[i].text;
    return t ? h_plain(t) : "(none)";
}
static const char *h_timer_at(int f) { return h_cs_at(CS_TDM_TIMELIMIT_STRING, f); }

/* the HUD text of `frames` remaining: rounded up to whole seconds, a
 * non-positive value shows zero */
static const char *h_hud_text(int frames)
{
    static char b[16];
    int secs = frames > 0 ? (frames + h_rate - 1) / h_rate : 0;
    snprintf(b, sizeof(b), "%2d:%.2d", secs / 60, secs % 60);
    return b;
}

/* two players in the two teams, past the team command rate limit */
static void h_tdm_pair(edict_t **a, edict_t **b)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_start(h_ents);
    *a = h_join(0, "p1");
    *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);
}

/* both teams ready; runs the countdown and returns 1 once the match plays */
static int h_tdm_start(edict_t *a, edict_t *b)
{
    h_client_command(a, "ready");
    h_client_command(b, "ready");
    for (int i = 0; i < 30 * h_rate && tdm_match_status != MM_PLAYING; i++) h_frame();
    CHECK(tdm_match_status == MM_PLAYING, "match started (status %d)", tdm_match_status);
    return tdm_match_status == MM_PLAYING;
}

/* announcements "N\n" (N = 1..5) from bprint index bp: the HUD string
 * `num` shows N on the announce frame, N + 1 one frame before and N one
 * frame after (n-1/n/n+1 of N * rate frames); returns the count */
static int h_announced(int bp, int num, const char *what)
{
    int seen = 0;
    for (int i = bp; i < h_nbprints; i++) {
        int nsec;
        char tail;
        if (sscanf(h_bprints[i].text, "%d%c", &nsec, &tail) != 2 || tail != '\n' || nsec < 1 || nsec > 5) continue;
        int f = h_bprints[i].frame;
        char want[16], prev[16];
        snprintf(want, sizeof(want), "%2d:%.2d", 0, nsec);
        snprintf(prev, sizeof(prev), "%2d:%.2d", 0, nsec + 1);
        seen++;
        const char *got = h_cs_at(num, f);
        CHECK(!strcmp(got, want), "%s announce \"%d\": HUD timer \"%s\" (got \"%s\")", what, nsec, want, got);
        got = h_cs_at(num, f - 1);
        CHECK(!strcmp(got, prev), "%s announce \"%d\", one frame before: \"%s\" (got \"%s\")", what, nsec, prev, got);
        got = h_cs_at(num, f + 1);
        CHECK(!strcmp(got, want), "%s announce \"%d\", one frame after: \"%s\" (got \"%s\")", what, nsec, want, got);
    }
    return seen;
}

/* Match countdown: both teams ready -> countdown; on the frame where the
 * mod announces "N" (N = 5..1 seconds left, an exact frame count) the
 * HUD timer configstring shows exactly 0:0N (positive countdown rounded
 * up, exact on whole seconds), N + 1 one frame before, N one frame after. */
static void sc_t15_countdown_hud(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_start(h_ents);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);     /* past the team command rate limit */
    int bp = h_nbprints;
    h_client_command(a, "ready");
    h_client_command(b, "ready");
    int started = 0;
    for (int i = bp; i < h_nbprints; i++) started |= strstr(h_bprints[i].text, "Starting countdown") != NULL;
    CHECK(started, "both teams ready: countdown started");
    if (!started) return;
    h_frames_n(20 * h_rate);
    int seen = h_announced(bp, CS_TDM_TIMELIMIT_STRING, "countdown");
    CHECK(seen == 5, "5 countdown announcements (got %d)", seen);
}

/* A whole short match (62 s, no overtime): every frame the HUD timer is
 * the remaining time rounded up (n-1/n/n+1 around 60 s, 5 s and 3 s
 * spelled out), "1 minute remaining" on the 60 s frame, zero at the end
 * and after it (never negative); the forced screenshot reaches each team
 * player once, 0.5 s after the intermission began. */
static void sc_t15_match_hud(void)
{
    h_set_override("g_match_time", "62");
    h_set_override("g_tie_mode", "0");
    h_set_override("g_force_screenshot", "1");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    int bp = h_nbprints, tx = h_ntexts, wrong = 0, minute = -1, end = -1, inter = -1;
    static const int marks[] = { 60, 5, 3 };
    for (int i = 0; i < 80 * h_rate && end < 0; i++) {
        int r = level.match_end_framenum - level.framenum;  /* as the frame sees it */
        h_frame();
        int f = h_frames - 1;
        if (tdm_match_status != MM_PLAYING) {
            end = f;
            if (level.match_score_end_framenum) inter = f;
            break;
        }
        const char *got = h_timer_at(f);
        for (int k = 0; k < 3; k++)
            if (r >= marks[k] * h_rate - 1 && r <= marks[k] * h_rate + 1)
                CHECK(!strcmp(got, h_hud_text(r)), "%d s %+d frame(s) left: HUD \"%s\" (got \"%s\")",
                      marks[k], r - marks[k] * h_rate, h_hud_text(r), got);
        if (strcmp(got, h_hud_text(r)) && !wrong++)
            printf("  info first wrong HUD frame: %d left, \"%s\" (want \"%s\")\n", r, got, h_hud_text(r));
        if (r == 60 * h_rate) minute = f;
    }
    CHECK(end >= 0, "match ended (status %d)", tdm_match_status);
    CHECK(!wrong, "HUD timer = remaining time rounded up on every frame (%d wrong)", wrong);
    int said = -1;
    for (int i = bp; i < h_nbprints; i++)
        if (strstr(h_bprints[i].text, "1 minute remaining")) said = h_bprints[i].frame;
    CHECK(said >= 0 && said == minute, "\"1 minute remaining\" on the 60 s frame %d (got %d)", minute, said);
    CHECK(!strcmp(h_timer_at(end), " 0:00"), "match end: HUD \" 0:00\" (got \"%s\")", h_timer_at(end));
    for (int i = 0; i < 3 * h_rate; i++) {
        h_frame();
        if (inter < 0 && level.match_score_end_framenum) inter = h_frames - 1;
    }
    CHECK(!strcmp(h_timer_at(h_frames - 1), " 0:00"), "after the end: HUD \" 0:00\" (got \"%s\")", h_timer_at(h_frames - 1));
    CHECK(inter >= 0, "intermission began");
    edict_t *pl[2] = { a, b };
    for (int k = 0; k < 2; k++) {
        const htext_t *t = h_text_to(pl[k], H_SVC_STUFFTEXT, "screenshot", tx);
        int n = 0;
        for (int i = tx; i < h_ntexts; i++)
            n += h_texts[i].entnum == NUM(pl[k]) && h_texts[i].svc == H_SVC_STUFFTEXT && strstr(h_texts[i].text, "screenshot");
        CHECK(n == 1, "p%d: one screenshot (got %d)", k + 1, n);
        CHECK(t && t->frame - inter == h_rate / 2, "p%d: screenshot 0.5 s = %d frames after the intermission began (got %d)",
              k + 1, h_rate / 2, t ? t->frame - inter : -1);
    }
}

/* Timeout: the timeout HUD string counts the pause down (rounded up,
 * every frame), then the 10.4 s resume countdown with its "N"
 * announcements (n-1/n/n+1); zero once play resumes. */
static void sc_t15_timeout_hud(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    h_frames_n(h_rate);
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "timeout (status %d)", tdm_match_status);
    int wrong = 0, bp = h_nbprints;
    for (int i = 0; i < 3 * h_rate + 1; i++) {
        int r = level.timeout_end_framenum - level.realframenum;
        h_frame();
        const char *got = h_cs_at(CS_TDM_TIMEOUT_STRING, h_frames - 1);
        if (strcmp(got, h_hud_text(r)) && !wrong++)
            printf("  info first wrong pause frame: %d left, \"%s\" (want \"%s\")\n", r, got, h_hud_text(r));
    }
    h_client_command(a, "time");
    CHECK(level.match_resume_framenum, "resume countdown");
    for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) {
        int r = level.match_resume_framenum - level.realframenum;
        h_frame();
        if (tdm_match_status != MM_TIMEOUT) break;
        const char *got = h_cs_at(CS_TDM_TIMEOUT_STRING, h_frames - 1);
        if (strcmp(got, h_hud_text(r)) && !wrong++)
            printf("  info first wrong resume frame: %d left, \"%s\" (want \"%s\")\n", r, got, h_hud_text(r));
    }
    CHECK(tdm_match_status == MM_PLAYING, "resumed (status %d)", tdm_match_status);
    CHECK(!wrong, "timeout HUD = remaining time rounded up on every frame (%d wrong)", wrong);
    int seen = h_announced(bp, CS_TDM_TIMEOUT_STRING, "resume");
    CHECK(seen == 5, "5 resume announcements (got %d)", seen);
    const char *got = h_cs_at(CS_TDM_TIMEOUT_STRING, h_frames - 1);
    CHECK(!strcmp(got, " 0:00"), "play resumed: timeout HUD \" 0:00\" (got \"%s\")", got);
}

/* `players`: a player in the game for exactly 60 s shows 1:00, one frame
 * less 0:59, 90 s 1:30 (the time column in minutes:seconds). */
static void sc_t15_players_time(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    static const struct { int secs, less; const char *want; } at[] = {
        { 60, 1, "  0:59" }, { 60, 0, "  1:00" }, { 90, 0, "  1:30" },
    };
    for (int k = 0; k < 3; k++) {
        int ticks = at[k].secs * h_rate - at[k].less;
        while (level.framenum - a->client->resp.enterframe < ticks) h_frame();
        CHECK(level.framenum - a->client->resp.enterframe == ticks, "in the game %d frames", ticks);
        int tx = h_ntexts;
        h_client_command(a, "players");
        const htext_t *t = h_text_to(a, 0, "id    time", tx);
        const char *line = t ? strstr(t->text, "\n 0 ") : NULL;
        char want[32];
        snprintf(want, sizeof(want), " 0  %s  ", at[k].want);
        CHECK(line && !strncmp(line + 1, want, strlen(want)), "players after %d frames: \"%s\" (got \"%.20s\")",
              ticks, want, line ? line + 1 : "(none)");
    }
}
#endif

static const struct { const char *name; void (*run)(void); } h_scenarios[] = {
    { "smoke", sc_smoke },
    { "t01_rate_refuse", sc_t01_rate_refuse },
    { "t04_cmd_schedule", sc_t04_cmd_schedule },
    { "t09_machinegun_held", sc_t09_machinegun_held },
    { "t09_hyperblaster_held", sc_t09_hyperblaster_held },
    { "t09_rocket_held", sc_t09_rocket_held },
    { "t09_rocket_tap", sc_t09_rocket_tap },
    { "t09_rocket_tick_tap", sc_t09_rocket_tick_tap },
    { "t09_machinegun_tick_tap", sc_t09_machinegun_tick_tap },
    { "t09_hyperblaster_tick_tap", sc_t09_hyperblaster_tick_tap },
#ifndef HARNESS_OPENTDM
    { "t09_rocket_second_tap", sc_t09_rocket_second_tap },
    { "t09_machinegun_second_tap", sc_t09_machinegun_second_tap },
    { "t09_hyperblaster_second_tap", sc_t09_hyperblaster_second_tap },
#endif
    { "t09_rocket_tap_spam", sc_t09_rocket_tap_spam },
    { "t09_machinegun_tap_spam", sc_t09_machinegun_tap_spam },
    { "t09_rocket_noammo", sc_t09_rocket_noammo },
    { "t09_lifecycle_none", sc_t09_lifecycle_none },
    { "t09_lifecycle_death", sc_t09_lifecycle_death },
    { "t09_lifecycle_spectator", sc_t09_lifecycle_spectator },
    { "t09_lifecycle_map", sc_t09_lifecycle_map },
    { "t09_lifecycle_switch", sc_t09_lifecycle_switch },
    { "t09_lifecycle_noammo", sc_t09_lifecycle_noammo },
#ifdef HARNESS_OPENTDM
    { "t09_lifecycle_countdown", sc_t09_lifecycle_countdown },
    { "t09_lifecycle_timeout", sc_t09_lifecycle_timeout },
    { "t09_lifecycle_readyattn", sc_t09_lifecycle_readyattn },
    { "t09_lifecycle_votehold", sc_t09_lifecycle_votehold },
#endif
    { "t10_grenade_held_explodes", sc_t10_grenade_held_explodes },
    { "t10_grenade_release_1s", sc_t10_grenade_release_1s },
    { "t10_grenade_quick", sc_t10_grenade_quick },
    { "t10_grenade_pending_tap", sc_t10_grenade_pending_tap },
    { "t10_grenade_death", sc_t10_grenade_death },
    { "t10_grenade_switch", sc_t10_grenade_switch },
    { "t11_hurt_normal", sc_t11_hurt_normal },
    { "t11_hurt_slow", sc_t11_hurt_slow },
    { "t11_hurt_toggle", sc_t11_hurt_toggle },
    { "t11_hurt_silent", sc_t11_hurt_silent },
    { "t11_hurt_god_plain", sc_t11_hurt_no_protection },
    { "t11_hurt_god_no_protection", sc_t11_hurt_no_protection_god },
    { "t11_hurt_shared", sc_t11_hurt_shared },
    { "t11_laser", sc_t11_laser },
    { "t12_door_blocked_reverses", sc_t12_door_blocked_reverses },
    { "t12_door_crusher", sc_t12_door_crusher },
    { "t12_door_debris", sc_t12_door_debris },
    { "t12_plat_blocked", sc_t12_plat_blocked },
    { "t12_rotating_blocked", sc_t12_rotating_blocked },
    { "t12_rotating_touch", sc_t12_rotating_touch },
    { "t12_rotating_ride", sc_t12_rotating_ride },
    { "t12_train_blocked", sc_t12_train_blocked },
    { "t12_secret_blocked", sc_t12_secret_blocked },
    { "t12_falling_object", sc_t12_falling_object },
    { "t12_falling_object_moving", sc_t12_falling_object_moving },
    { "t12_crush_phases", sc_t12_crush_phases },
    { "t12_crush_pairs", sc_t12_crush_pairs },
    { "t12_crush_reverse", sc_t12_crush_reverse },
    { "t12_crush_overflow", sc_t12_crush_overflow },
    { "t13_linear_door", sc_t13_linear_door },
    { "t13_door_negative", sc_t13_door_negative },
    { "t13_door_diagonal", sc_t13_door_diagonal },
    { "t13_door_short", sc_t13_door_short },
    { "t13_team_block", sc_t13_team_block },
    { "t13_carry_reset", sc_t13_carry_reset },
    { "t13_train_teleport", sc_t13_train_teleport },
    { "t13_carry_map", sc_t13_carry_map },
    { "t14_plat", sc_t14_plat },
    { "t14_plat_zero", sc_t14_plat_zero },
    { "t14_plat_down", sc_t14_plat_down },
    { "t14_plat_cycles", sc_t14_plat_cycles },
    { "t13_team_down_idle", sc_t13_team_down_idle },
#ifdef HARNESS_OPENTDM
    { "t15_countdown_hud", sc_t15_countdown_hud },
    { "t15_match_hud", sc_t15_match_hud },
    { "t15_timeout_hud", sc_t15_timeout_hud },
    { "t15_players_time", sc_t15_players_time },
#endif
    { NULL, NULL }
};
