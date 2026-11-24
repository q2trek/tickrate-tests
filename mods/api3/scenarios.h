/*
 * T09-T15 against the production module. Expected numbers are the
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

/* The usercmd schedules (h_tick_sched): over any second (h_rate ticks)
 * the commands last 1000 ms, at 60 Hz every tick command 16 or 17 ms; the
 * world ticks without a ClientThink are the ones the schedule leaves out */
/* The engine rounds sv_fps down to a multiple of 10 before the game starts
 * (engine/scheduler t01_rate_round); a module that still sees 25 or 0 refuses
 * it instead of running a world clock the engine does not keep */
static sigjmp_buf h_fpe_jmp;
static void h_fpe(int sig) { (void)sig; siglongjmp(h_fpe_jmp, 1); }

static void h_rate_refuse(const char *value)
{
    jmp_buf outer;
    memcpy(outer, h_abort, sizeof(outer));
    imp_cvar("sv_fps", value, 0);
    imp_cvar("sv_features", va("%d", GMF_VARIABLE_FPS), 0);
    int refused = 0, crashed = 0;
    signal(SIGFPE, h_fpe);  /* a module that takes 0 divides by it */
    if (sigsetjmp(h_fpe_jmp, 1))
        crashed = 1;
    else if (setjmp(h_abort))
        refused = 1;
    else {
        h_scene_base();
        h_start(h_ents);
    }
    signal(SIGFPE, SIG_DFL);
    memcpy(h_abort, outer, sizeof(outer));
#ifdef HARNESS_OPENTDM
    const char *want = "Invalid server FPS";
#else
    const char *want = va("Invalid value '%s' for 'sv_fps' cvar", value);
#endif
    CHECK(refused && !strcmp(h_errmsg, want), "sv_fps %s: the module refuses to start (error '%s')",
          value, refused ? h_errmsg : crashed ? "SIGFPE" : "none");
}

static void sc_t01_rate_refuse(void) { h_rate_refuse("25"); }
static void sc_t01_rate_zero(void) { h_rate_refuse("0"); }

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

static void sc_w_timing_window(void)
{
    edict_t *p = h_armed("rocket launcher", "rockets", 20);
    while (h_frames % h_div != 1 % h_div)
        h_frame();
    p->client->quad_framenum = level.framenum + (h_div - h_frames % h_div) % h_div;
    int messages = h_nmsgs;
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, 1);
    CHECK(h_shots(p, MZ_ROCKET, messages) == 1, "first rocket fires in the command");
    int to_grid = (h_div - h_frames % h_div) % h_div;
    edict_t *rocket = G_Find(NULL, FOFS(classname), "rocket");
    CHECK(rocket && rocket->dmg >= 100 && rocket->dmg < 120,
          "rocket sees quad expire at the window frame");
    CHECK(rocket && rocket->nextthink == level.framenum + to_grid + 8000 * h_rate / 650,
          "rocket lifetime starts at the window frame");
    h_frames_n(to_grid + 1);
    for (int i = 0; i < 2 * h_div; i++) {
#ifdef HARNESS_OPENTDM
        int previous = p->client->ps.gunframe;
#else
        int previous = p->client->weaponframe;
#endif
        int step = h_frames % h_div == 0;
        h_frame();
#ifdef HARNESS_OPENTDM
        int current = p->client->ps.gunframe;
#else
        int current = p->client->weaponframe;
#endif
        CHECK(current == previous + step, "automatic reload advances only on the grid, tick %d (frame %d -> %d)",
              i, previous, current);
    }
}

static void sc_w_special_windowclock(void)
{
    edict_t *p = h_armed("grenade launcher", "grenades", 20);
    for (int phase = 0; phase < h_div; phase++) {
        while (h_frames % h_div != phase)
            h_frame();
        int frame = level.framenum;
        int window = frame + (h_div - phase) % h_div;
        int messages = h_nmsgs;
        h_think(p, BUTTON_ATTACK, 1);
        edict_t *grenade = G_Find(NULL, FOFS(classname), "grenade");
        CHECK(grenade && h_shots(p, MZ_GRENADE, messages) == 1,
              "phase %d: grenade and muzzleflash in the command", phase);
        CHECK(grenade && grenade->nextthink == window + 25 * h_div,
              "phase %d: fuse deadline starts at the window frame (%d, expected %d)",
              phase, grenade ? grenade->nextthink : -1, window + 25 * h_div);
        CHECK(level.framenum == frame, "phase %d: weapon restores the world frame", phase);
        h_think(p, 0, 1);
        h_frames_n(3 * h_rate);
    }

    h_use(p, "grenades");
    while (h_frames % h_div != 1 % h_div)
        h_frame();
    p->client->weaponstate = WEAPON_FIRING;
#ifdef HARNESS_OPENTDM
    p->client->ps.gunframe = 11;
#else
    p->client->weaponframe = 11;
#endif
    int fuse = level.framenum + (h_div - h_frames % h_div) % h_div + 32 * h_div;
    h_think(p, BUTTON_ATTACK, 1);
    CHECK(p->client->grenade_framenum == fuse,
          "hand grenade fuse arms from the window frame (got %d, expected %d)",
          p->client->grenade_framenum, fuse);
}

static void sc_w_second_short_tap(void)
{
    edict_t *p = h_armed("machinegun", "bullets", 50);
    for (int pattern = 0; pattern < 3; pattern++) {
        h_frames_n(h_rate);
        while (h_frames % h_div != 1 % h_div)
            h_frame();
        int messages = h_nmsgs, ammo = h_ammo(p);
        h_think(p, BUTTON_ATTACK, 1);
        h_think(p, 0, 1);
        CHECK(h_shots(p, MZ_MACHINEGUN, messages) == 1, "first tap fires once in the command");
        int remaining = (h_div - h_frames % h_div) % h_div + 1;
        if (pattern == 1)
            h_frames_n(remaining);   /* second tap in the following window */
        h_think(p, BUTTON_ATTACK, 1);
        if (pattern != 2)
            h_think(p, 0, 1);
        if (pattern != 1) {
            h_frames_n(remaining);
            CHECK(h_shots(p, MZ_MACHINEGUN, messages) == 1, "same window: only the first weapon step fired");
        }
        h_frames_n(h_div);
#ifdef HARNESS_OPENTDM
        int expected = pattern == 0 ? 1 : 2;
#else
        int expected = 2;
#endif
        CHECK(h_shots(p, MZ_MACHINEGUN, messages) == expected,
              "pattern %d: the second tap has its verdict at the next weapon step", pattern);
        h_think(p, 0, 1);
        h_frames_n(2 * h_div);
        CHECK(h_shots(p, MZ_MACHINEGUN, messages) == expected && ammo - h_ammo(p) == expected,
              "%s: expected %d shots and bullets (got %d, %d)",
              (const char *[]){ "second short tap in the same window", "tap in the adjacent window", "held second tap" }[pattern],
              expected, h_shots(p, MZ_MACHINEGUN, messages), ammo - h_ammo(p));
    }
}

#ifndef HARNESS_OPENTDM
static void sc_w_second_tap_while_firing(void)
{
    edict_t *p = h_armed("machinegun", "bullets", 50);
    while (h_frames % h_div != 1 % h_div)
        h_frame();
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, 1);
    h_frames_n(h_div);
    CHECK(p->client->weaponstate == WEAPON_FIRING, "machinegun is still in its firing cycle");

    int messages = h_nmsgs;
    const int *bullets = &p->client->inventory[p->client->ammo_index];
    int ammo = *bullets;
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, 1);
    CHECK(h_shots(p, MZ_MACHINEGUN, messages) == 1 && *bullets == ammo - 1,
          "first tap from WEAPON_FIRING fires once in the command");
    if (h_div > 1)
        h_frame();  /* the next tap arrives on another world frame of the same window */
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, 1);
    while (h_frames % h_div)
        h_frame();
    h_frame();
    CHECK(h_shots(p, MZ_MACHINEGUN, messages) == 1, "one firing step in the input window");
    h_frames_n(h_div);
    CHECK(h_shots(p, MZ_MACHINEGUN, messages) == 1 && *bullets == ammo - 1,
          "released tap after a WEAPON_FIRING shot adds no flash or ammo expenditure on the next step");
}

static void sc_w_respawn_latch(void)
{
    edict_t *p = h_armed("rocket launcher", "rockets", 20);
    h_frames_n(5 * h_rate);
    while (h_frames % h_div != 1 % h_div)
        h_frame();
    h_think(p, BUTTON_USE, 1);
    h_think(p, 0, 1);
    while (h_frames % h_div) {
        h_frame();
        CHECK(p->client->latched_buttons & BUTTON_USE, "live button latch survives a non-grid frame");
    }
    h_frame();
    CHECK(!p->client->latched_buttons, "live button latch clears on the grid frame");

    h_client_command(p, "kill");
    CHECK(p->deadflag, "player is dead before the respawn input");
    do {
        h_frame();
    } while (level.framenum <= p->client->respawn_framenum || h_frames % h_div != 1 % h_div);
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, 1);
    while (h_frames % h_div) {
        h_frame();
        CHECK(p->deadflag, "respawn from a released tap waits for the grid frame");
    }
    h_frame();
    CHECK(!p->deadflag && p->health > 0, "latched attack respawns on the grid frame");
}
#endif

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
/* Deliver both taps in one packet before RunFrame at every phase. */
static void h_batch_taps(const char *weapon, const char *ammo, int mz)
{
    edict_t *p = h_armed(weapon, ammo, 50);
#ifdef HARNESS_OPENTDM
    int expected = 1;   /* the second press is released before the next step */
#else
    int expected = 2;   /* the second press is pending */
#endif
    for (int k = 0; k < h_div; k++) {
        h_phase(p, k);
        int m = h_nmsgs, a = h_ammo(p);
        h_think(p, BUTTON_ATTACK, 20);
        h_think(p, 0, 20);
        h_think(p, BUTTON_ATTACK, 20);
        h_think(p, 0, 20);
        int inpack = h_shots(p, mz, m);
        h_frame();
        for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
        int shots = h_shots(p, mz, m), used = a - h_ammo(p);
        printf("  info %s batch taps phase %d/%d: %d in the batch, %d shots, %d ammo\n", weapon, k, h_div, inpack, shots, used);
        CHECK(inpack == 1 && shots == expected && used == expected,
              "%s phase %d/%d: one shot in the batch, %d total shots and ammo (got %d, %d, %d)",
              weapon, k, h_div, expected, inpack, shots, used);
    }
}
static void sc_t09_machinegun_batch_taps(void) { h_batch_taps("machinegun", "bullets", MZ_MACHINEGUN); }
static void sc_t09_hyperblaster_batch_taps(void) { h_batch_taps("hyperblaster", "cells", MZ_HYPERBLASTER); }

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

/* Machinegun sub-frame tap: the known defect (live buttons are read at the
 * world frame) is recorded, not asserted. */
static void sc_t09_machinegun_tap_observed(void)
{
    edict_t *p = h_armed("machinegun", "bullets", 50);
    int m = h_nmsgs, a = h_ammo(p);
    h_think(p, BUTTON_ATTACK, 500 / h_rate);
    h_think(p, 0, 500 / h_rate);
    h_frame();
    for (int i = 0; i < h_rate; i++) h_tick(p, 0, 1);
    printf("  info machinegun sub-frame tap: %d shots, %d bullets (observed, not asserted)\n",
           h_shots(p, MZ_MACHINEGUN, m), a - h_ammo(p));
}

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

/* Pending press losing its context (6.3). A press that the weapon has not
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
        h_think(p, BUTTON_ATTACK, 10);
        h_think(p, 0, 10);
        h_frames_n(h_rate);
        h_client_command(p, "time");
        for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) h_frame();
        CHECK(tdm_match_status == MM_PLAYING, "resumed (status %d)", tdm_match_status);
        h_frames_n(h_rate);   /* no new command clears stale input before resumed weapon steps */
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

/* The game uses the coming world frame while it runs the entity loop. */
static int h_grid_wait(void)
{
#ifdef HARNESS_OPENTDM
    int frame = level.realframenum;
#else
    int frame = h_frames;
#endif
    return (h_div - frame % h_div) % h_div;
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
    int window = thrown + (h_grid_wait() + 1) % h_div;
    CHECK(thrown - arm == 22 * h_div, "thrown 2.2 s (%d frames) after arming, 1 s remaining (got %d)", 22 * h_div, thrown - arm);
    float x0 = g->s.origin[0];
    h_frames_n(h_grid_wait() + 1);
    float speed = (g->s.origin[0] - x0) * 10;
    CHECK(fabsf(speed - H_GRENADE_SPEED_1S) <= 1, "forward speed %.2f = %d +- 1", speed, H_GRENADE_SPEED_1S);
    int boom = -1, m = h_nmsgs;
    for (int i = 0; i < 3 * h_rate && boom < 0; i++) { h_frame(); if (h_explosions(m)) boom = h_frames - 1; }
    CHECK(boom - window == h_rate, "flying grenade explodes after the remaining 1 s: %d frames (got %d)", h_rate, boom - window);
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
    int window = thrown + (h_grid_wait() + 1) % h_div;
    h_tick(p, BUTTON_ATTACK, 1);
    float x0 = g->s.origin[0];
    h_frames_n(h_grid_wait() + 1);
    float speed = (g->s.origin[0] - x0) * 10;
    CHECK(fabsf(speed - H_GRENADE_SPEED_QUICK) <= 1, "quick release speed %.2f = %d +- 1", speed, H_GRENADE_SPEED_QUICK);
    int boom = -1;
    for (int i = 0; i < 5 * h_rate && boom < 0; i++) { h_tick(p, 0, 1); if (h_explosions(m)) boom = h_frames - 1; }
    CHECK(boom - window == 31 * h_div, "explodes 3.1 s after the throw: %d frames (got %d)", 31 * h_div, boom - window);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    CHECK(h_explosions(m) == 1 && a - h_ammo(p) == 1, "one grenade, one ammo (got %d explosions, %d spent)", h_explosions(m), a - h_ammo(p));
}


/* D14 (from the external review): a released re-tap while a grenade is
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

    int msec = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
    h_think(p, 0, 1);
    h_think(p, BUTTON_ATTACK, 1);
#ifdef HARNESS_OPENTDM
    CHECK(p->client->weapon_thunk, "the held attack has already used the window's weapon step");
    h_think(p, 0, 1);
    h_think(p, BUTTON_ATTACK, 1);
    h_think(p, 0, msec - 4);
#else
    h_think(p, 0, msec - 2);
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
#ifdef HARNESS_OPENTDM
    /* The current window already used its command step. */
    int expected_release = h_frames + (h_div - h_frames % h_div) % h_div + h_div;
#else
    int queued = p->client->attack_pending;
    if (h_div > 1)
        CHECK(queued, "the re-tap waits for a permitted weapon update");
#endif

    int released = -1;
    for (int i = 0; i < 4 * h_div; i++) {
        msec = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
        h_think(p, 0, msec);
        h_frame();
#ifdef HARNESS_OPENTDM
        int gunframe = p->client->ps.gunframe;
        if (gunframe == 12) {
            CHECK(h_frames - 1 == expected_release,
                  "grenade release advances at the first unused grid step (got %d, expected %d)",
                  h_frames - 1, expected_release);
#else
        int gunframe = p->client->weaponframe;
        if (queued ? !p->client->attack_pending : gunframe == 12) {
#endif
            released = h_frames - 1;
            printf("  info consumed frame=%d gunframe=%d real_buttons=%d fuse_frame=%d\n",
                   released, gunframe, p->client->buttons, p->client->grenade_framenum);
#ifndef HARNESS_OPENTDM
            CHECK(gunframe == 12, "released cooking grenade advances to throw frame 12 at the permitted update (got %d)", gunframe);
#endif
            break;
        }
    }
    CHECK(released >= 0, "a permitted weapon update occurred");
#ifdef HARNESS_OPENTDM
    int expected_throw = expected_release + h_div;
#else
    int expected_throw = released + h_div;
#endif

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
           arm, released, thrown, expected_throw, thrown - released);
    CHECK(g != NULL && thrown == expected_throw,
          "one flying grenade on the following 100 ms weapon update (got frame %d, expected %d)", thrown, expected_throw);
    CHECK(a - h_ammo(p) == 1, "exactly one grenade spent");
}

/* Death while holding an armed grenade: it goes off at once (timer 0) at
 * the first weapon processing after the death: one explosion, nothing in
 * flight. The death clears the inventory before the fire. */
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
#ifdef HARNESS_OPENTDM
    int death_step = k + (h_div - level.realframenum % h_div) % h_div;
#endif
    for (int i = 0; i < 2 * h_div && boom < 0; i++) { h_frame(); if (h_explosions(m)) boom = h_frames - 1; }
    CHECK(p->deadflag, "dead");
#ifdef HARNESS_OPENTDM
    CHECK(boom == death_step, "explodes on the first grid step after death (frame %d, got %d)", death_step, boom);
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

/* leave the trigger and wait 2 s (the source debounce runs out) */
static void h_hurt_leave(edict_t *p)
{
    h_place(p, 0, 0, 24);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(p, 0, 1);
    h_give(p, "health", 100);
    h_frame();      /* the HUD health stat follows at the end of a frame */
}

static void h_hurt_phase(edict_t *p, int phase)
{
    h_phase(p, phase);
}

static void sc_t11_hurt_normal(void)
{
    h_hurt_scene(0);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    int sounds;
    int second_phase = G_WindowFrame(h_rate) - h_rate;
    for (int k = 0; k < h_div; k++) {
        if (k) h_hurt_leave(p);
        h_hurt_phase(p, k);
        int first = level.framenum;
        int ticks = G_WindowFrame(first) + 9 * h_div - first + 1;
        int total = h_hurt_run(p, ticks, 2, hits, &sounds);
        CHECK(total == 10, "phase %d: ten hits in ten contact windows (got %d)", k, total);
        for (int i = 0; i < ticks; i++) {
            int window = G_WindowFrame(first + i);
            int hit = !i || window != G_WindowFrame(first + i - 1);
            CHECK(hits[i] == hit, "phase %d tick %d: first contact in window hits (%d, want %d)",
                  k, i, hits[i], hit);
            CHECK(h_snd[i] == (hit && window % h_rate == second_phase),
                  "phase %d tick %d: sound only on a whole-second hit (%d)", k, i, h_snd[i]);
        }
    }

    h_hurt_leave(p);
    h_hurt_phase(p, 0);
    CHECK(h_hurt_run(p, 1, 2, hits, &sounds) == 1, "last commands in a window share one hit");
    h_place(p, 0, 0, 24);
    for (int i = 0; i < h_div; i++)
        h_tick(p, 0, 1);
    CHECK(h_hurt_run(p, 1, 2, hits, &sounds) == 1, "reentry after one empty window hits immediately");
}

static void sc_t11_hurt_slow(void)
{
    h_hurt_scene(16);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    int sounds;
    int second_phase = G_WindowFrame(h_rate) - h_rate;
    for (int k = 0; k < h_div; k++) {
        if (k) h_hurt_leave(p);
        h_hurt_phase(p, k);
        int first = level.framenum;
        int window = G_WindowFrame(first);
        int second = window + 9 * h_div + 1 - first;
        int total = h_hurt_run(p, second + 1, 1, hits, &sounds);
        CHECK(hits[0] == 1 && hits[second] == 1 && total == 2,
              "phase %d: SLOW hits on contact and at the next allowed window (%d, %d, total %d)",
              k, hits[0], hits[second], total);
        CHECK(sounds == (window % h_rate == second_phase ? 2 : 0),
              "phase %d: SLOW sounds only on whole-second hits (got %d)", k, sounds);
    }
}

static void sc_t11_hurt_sound_grid(void)
{
    h_hurt_scene(16);
    edict_t *p = h_join(0, "p1");
    static int hits[H_MAXTICKS];
    int sounds;
    int off_second = (G_WindowFrame(h_rate) + 1) % h_rate;
    while (level.framenum % h_rate != off_second)
        h_frame();
    int total = h_hurt_run(p, 2 * h_rate, 1, hits, &sounds);
    CHECK(total == 2 && sounds == 0, "off-second SLOW contact: two hits and zero sounds (got %d, %d)",
          total, sounds);
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
    int sounds;
    h_hurt_phase(p, 1 % h_div);
    int total = h_hurt_run(p, h_rate, 1, hits, &sounds);
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
    h_hurt_phase(p, 1 % h_div);
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
    h_hurt_phase(p, 1 % h_div);
    int total = h_hurt_run(p, h_rate, 1, hits, &sounds);
    CHECK(total == 10, "god mode, NO_PROTECTION: 10 hits (got %d)", total);
}

/* One source, two players inside: the debounce is per source, so the
 * source deals one hit per 100 ms window in total. */
static void sc_t11_hurt_shared(void)
{
    h_hurt_scene(0);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2");
    h_hurt_phase(a, 0);
    h_place(a, 512, 0, 24);
    h_think(a, 0, 1000 / h_rate);
    h_place(b, 512, 0, 24);
    h_think(b, 0, 1000 / h_rate);
    h_frame();
    int ha = h_hp(a), hb = h_hp(b);
    for (int i = 0; i < h_rate; i++) {
        h_think(a, 0, 1000 / h_rate);
        h_think(b, 0, 1000 / h_rate);
        h_frame();
    }
    int da = ha - h_hp(a), db = hb - h_hp(b);
    CHECK(da + db == 10, "shared source: 10 hits per second over both players (got %d + %d)", da, db);
}

/* Damage with knockback but no direction (hurt and crush pass a zero
 * dir): no push, the velocity stays what it was. */
static void sc_t11_knockback_zero_dir(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    vec3_t dir = { 0, 0, 0 }, v = { 10, 20, 30 };
    VectorCopy(v, p->velocity);
    T_Damage(p, g_edicts, g_edicts, dir, p->s.origin, dir, 10, 10, 0, MOD_TRIGGER_HURT);
    CHECK(VectorCompare(p->velocity, v), "velocity unchanged (%g %g %g)", p->velocity[0], p->velocity[1], p->velocity[2]);
    CHECK(p->health < 100, "the damage was dealt (health %d)", p->health);
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
    if (h_div > 1) {
        while (!FRAMESYNC) h_frame();
        h_frame();  /* The first relink must be checked outside a keyframe. */
    }
    h_place(p, 1, 1000, 24);
    int t0 = h_damage_run(p, 2 * h_rate, 1, hits);
    CHECK(t0 == 0, "object re-impacts the player on the first relink (tick %d)", t0);
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

/* The production blocked callbacks called within one world tick, as the
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

/* D14: more than MAX_CRUSH pairs active within 0.1 s. MAX_CRUSH filler
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

#ifndef HARNESS_OPENTDM
    /* level reset (`sv reset`, or the end of a map that rotates to itself):
     * the frame counter restarts, a client hit by the overflow starts with an
     * immediate hit */
    h_cfill[1] = G_Spawn();
    h_frames_n(h_rate);
    hits = h_crush_run(s1, NULL, p, 1, &first, &gap);
    CHECK(hits == 1, "overflow hit on the client before the reset (%d)", hits);
    G_ResetLevel();
    h_frame();
    s1 = G_Spawn();
    h_crush_spawn_fill();
    hits = h_crush_run(s1, NULL, p, 1, &first, &gap);
    CHECK(hits == 1, "level reset: the overflow hits the client at once (%d)", hits);
#endif
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
#define H_T14_TICKS10 7     /* the 10 Hz duration of both mods before task 8 */

static void h_t14_plat(int zero)
{
    h_scene_base();
    int m = h_add_inline(-64, 1936, 8, 64, 2064, 72);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"%s\" \"64\"\n}\n", m, zero ? "lip" : "height");
    h_start(h_ents);
    edict_t *plat = h_by_model("*1"), *p = h_join(0, "p1");
    CHECK(plat != NULL, "plat spawned");
    if (!plat) return;
    h_place(p, 0, 2000, plat->absmax[2] - 1 + 24 + 0.125f);
    h_tick(p, 0, 1);
    if (!zero)
        CHECK(p->groundentity == plat, "plat rider is grounded before departure");
    static hpath_t t;
    float ride0 = p->s.origin[2] - plat->s.origin[2], maxride = 0, vmaxz = 0;
    h_path_begin(&t, plat);
    for (int i = 0; i < 2 * h_rate; i++) {
        h_path_tick(&t, p);
        maxride = fmaxf(maxride, fabsf(p->s.origin[2] - plat->s.origin[2] - ride0));
        if (h_verbose) printf("    rider[%d] = %.4f (plat %.4f)\n", i, p->s.origin[2] - plat->s.origin[2] - ride0, plat->s.origin[2]);
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
    CHECK(maxride <= H_EPS, "the rider moves with the plat (offset change %.4f)", maxride);
    CHECK(vmaxz <= 200 + H_EPS, "speed within 200 units/s (max %.3f)", vmaxz);
    int a, b;
    vec3_t end = { 0, 0, 64 }, vmax = { 0, 0, 200 };
    if (!h_path_seg(&t, 0, &a, &b)) { CHECK(0, "plat rose and stopped"); return; }
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

/* T14 rider on a func_plat going down (task 14.2, design D12), the rider's
 * usercmds by HARNESS_SCHED. After each world tick from the departure: the
 * rider's ground is the plat, its height relative to the plat within 1/8 of
 * the settled one, no EV_FALL* and no health lost; the plat plays one start
 * and one end sound per leg, its loop sound is off while it stands, and the
 * durations of the legs of one direction differ by at most one tick.
 * cycles 0: a targeted plat at the top, used once, goes down 512 units (the
 * external review scene); cycles n: a PLAT_LOW_TRIGGER plat (the rider at
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

/* T13 rollback of a team push with an idle rider (task 14.2, design D12):
 * a two-part team going down (the external review scene); A carries p1, B
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

/* misc_viper_bomb dropped (used) high above the floor: it rolls 100
 * degrees per second while it falls (10 per 100 ms frame originally). */
static void sc_t13_viper_bomb_roll(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"misc_viper_bomb\"\n\"origin\" \"0 0 2000\"\n}\n");
    h_start(h_ents);
    edict_t *bomb = h_by_class("misc_viper_bomb", 0);
    edict_t *viper = G_Spawn();     /* the bomb takes its flight from the viper */
    viper->classname = "misc_viper";
    VectorSet(viper->moveinfo.dir, 1, 0, 0);
    viper->moveinfo.speed = 100;
    CHECK(bomb && bomb->use, "bomb spawned");
    if (!bomb || !bomb->use) return;
    bomb->use(bomb, g_edicts, g_edicts);
    h_frame();
    float r0 = bomb->s.angles[2];
    h_frames_n(h_rate);
    float roll = bomb->s.angles[2] - r0;
    CHECK(bomb->inuse && bomb->s.origin[2] > 1000, "still falling (z %.0f)", bomb->s.origin[2]);
    CHECK(fabsf(roll - 100) < 0.01f, "rolls 100 degrees in 1 s (got %.3f)", roll);
}

/* BFG ball fired from (0,0,24) along +X at 400 units/s into a wall at
 * X=90, a player without armor at (-20,150,24) next to its path, the
 * owner out of the way; in every phase of the 100 ms step. At 10 Hz: two
 * laser hits of 5 in flight (frames 1 and 2, the ball touches the wall in
 * frame 3), then the secondary explosion 100 ms after the touch from the
 * touch point moved back by 100 ms of flight (x = 90 - 40 = 50):
 * 200 * (1 - sqrt(|(50,0,24) - (-20,150,28)| / 1000)) = 118 damage.
 * The movement, lasers and explosion all use the grid. */
typedef struct { int lasers, touch, explode, boom; } hbfg_t;
static hbfg_t h_bfg_shot(edict_t *owner, edict_t *target)
{
    hbfg_t r = { 0, -1, -1, 0 };
    vec3_t start = { 0, 0, 24 }, dir = { 1, 0, 0 };
    target->health = 1000;
    for (int ai; (ai = ArmorIndex(target)); ) target->client->inventory[ai] = 0;
    fire_bfg(owner, start, dir, 200, 400, 1000);
    edict_t *bfg = h_by_class("bfg blast", 0);
    if (!bfg) return r;
    for (int f = 1; f <= 2 * h_rate && r.explode < 0; f++) {
        int hp = target->health, flying = bfg->touch != NULL;
        h_frame();
        int d = hp - target->health;
        if (flying && !bfg->touch) r.touch = f;
        if (bfg->inuse && bfg->s.frame >= 1) { r.explode = f; r.boom = d; }
        else r.lasers += d / 5;
    }
    return r;
}

static void sc_t11_bfg_wall(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"-300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_add_world(90, -512, 0, 128, 512, 512);
    h_start(h_ents);
    edict_t *owner = h_join(0, "p1"), *target = h_join(1, "p2");
    VectorSet(owner->s.origin, -300, 0, 24);
    imp_linkentity(owner);
    VectorSet(target->s.origin, -20, 150, 24);
    imp_linkentity(target);
    for (int k = 0; k < h_div; k++) {
        while (h_frames % h_div != k) h_frame();
        int first_step = h_grid_wait() + 1;
        hbfg_t r = h_bfg_shot(owner, target);
        int delay = r.explode - r.touch;
        printf("  info phase %d/%d: %d laser hits, touch in frame %d, explosion %d frames later, %d damage\n",
               k, h_div, r.lasers, r.touch, delay, r.boom);
        CHECK(r.touch > 0 && r.explode > 0, "phase %d/%d: the ball touches the wall and explodes", k, h_div);
        CHECK(r.boom == 118, "phase %d/%d: secondary explosion 118 damage, as at 10 Hz (got %d)", k, h_div, r.boom);
        CHECK(r.touch == first_step + 2 * h_div,
              "phase %d/%d: touches the wall on the third grid step (got frame %d, expected %d)",
              k, h_div, r.touch, first_step + 2 * h_div);
        CHECK(r.lasers == 2, "phase %d/%d: 2 laser hits in flight, as at 10 Hz (got %d)", k, h_div, r.lasers);
        CHECK(delay == h_div, "phase %d/%d: explosion 100 ms (%d frames) after the touch (got %d)", k, h_div, h_div, delay);
        h_frames_n(h_rate);
    }
}

/* ---------------------------------------------------------------- T15 */

/* Two active players with distinct spawn points, past the team command limit. */
static void h_tdm_pair(edict_t **a, edict_t **b)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n\"angle\" \"0\"\n}\n");
    h_start(h_ents);
    *a = h_join(0, "p1");
    *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);
}

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

/* Fire released (or a grenade still held) during a timeout. Attack held
 * into the pause, then usercmds during it: released (machinegun, a cooking
 * grenade) or still held (grenade); after resume no usercmd at all and a
 * window of 3 weapon steps (the extra 10 Hz shot comes on the second). The
 * pause starts in every phase of the weapon step. Released: no shot after
 * resume, the grenade is thrown in the window and does not go off in the
 * hand. Held: the grenade stays cooking in the window, then a release
 * throws it. */
enum { H_TO_MG, H_TO_RELEASE, H_TO_HOLD };
static void h_timeout_fire(int kind)
{
    h_set_override("g_timeout_limit", "0");     /* one timeout per phase */
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    /* nobody dies: a respawn (random spawn point) could telefrag the other */
    a->flags |= FL_GODMODE;
    b->flags |= FL_GODMODE;
    for (int k = 0; k < h_div; k++) {
        if (kind == H_TO_MG) {
            h_give(a, "machinegun", 0);
            h_give(a, "bullets", 100);
            h_use(a, "machinegun");
            for (int i = 0; i < h_rate / 2; i++) h_tick(a, BUTTON_ATTACK, 1);
        } else {
            h_give(a, "grenades", 5);
            h_use(a, "grenades");
            CHECK(h_arm_grenade(a) >= 0, "phase %d: grenade armed", k);
        }
        /* the next weapon step k world frames ahead */
        for (int i = 0; i < h_div && (h_div - level.realframenum % h_div) % h_div != k; i++)
            h_tick(a, BUTTON_ATTACK, 1);
        CHECK((h_div - level.realframenum % h_div) % h_div == k, "phase %d/%d of the weapon step", k, h_div);
        int ammo = h_ammo(a), m0 = h_nmsgs;
        h_client_command(a, "time");
        CHECK(tdm_match_status == MM_TIMEOUT, "phase %d: timeout (status %d)", k, tdm_match_status);
        for (int i = 0; i < h_rate; i++) h_tick(a, kind == H_TO_HOLD ? BUTTON_ATTACK : 0, 1);
        h_client_command(a, "time");
        for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) h_frame();
        CHECK(tdm_match_status == MM_PLAYING, "phase %d: resumed (status %d)", k, tdm_match_status);
        h_frames_n(3 * h_div);
        edict_t *g = h_grenade();
        if (kind == H_TO_MG) {
            int shots = h_shots(a, MZ_MACHINEGUN, m0);
            CHECK(shots == 0 && h_ammo(a) == ammo, "phase %d/%d: released in the pause, no shot after resume (got %d, %d bullets spent)",
                  k, h_div, shots, ammo - h_ammo(a));
        } else if (kind == H_TO_RELEASE) {
            CHECK(g && !h_explosions(m0) && h_ammo(a) == ammo - 1,
                  "phase %d/%d: released in the pause, thrown in 3 steps after resume (grenade %s, %d explosions, %d spent)",
                  k, h_div, g ? "flying" : "none", h_explosions(m0), ammo - h_ammo(a));
        } else {
            CHECK(!g && !h_explosions(m0) && h_ammo(a) == ammo, "phase %d/%d: held through the pause, still cooking (grenade %s, %d explosions)",
                  k, h_div, g ? "flying" : "none", h_explosions(m0));
            for (int i = 0; i < 3 * h_div && !g; i++) { h_tick(a, 0, 1); g = h_grenade(); }
            CHECK(g != NULL, "phase %d/%d: then released, thrown", k, h_div);
        }
        for (int i = 0; i < 5 * h_rate; i++) h_tick(a, 0, 1);  /* the grenade goes off */
    }
}
static void sc_t09_timeout_release_mg(void) { h_timeout_fire(H_TO_MG); }
static void sc_t10_timeout_release_grenade(void) { h_timeout_fire(H_TO_RELEASE); }
static void sc_t10_timeout_hold_grenade(void) { h_timeout_fire(H_TO_HOLD); }
#endif

/* ---------------------------------------------------------------- T16 */

/* the status sample after the last frame (abort the scenario on failure) */
enum { H_PLAT_GRENADE, H_PLAT_HAND_GRENADE, H_PLAT_ITEM };

static edict_t *h_plat_object(edict_t *player, edict_t *plat, int kind)
{
    vec3_t start = { 8, 2000, plat->absmax[2] - 1 + 4 }, dir = { 1, 0, 0 };
#ifdef HARNESS_OPENTDM
    float fuse = 30;
#else
    int fuse = 30 * h_rate;
#endif
    edict_t *obj;
    if (kind == H_PLAT_ITEM) {
        obj = Drop_Item(player, FindItem("Shells"));
    } else if (kind == H_PLAT_GRENADE) {
        fire_grenade(player, start, dir, 120, 0, fuse, 160);
        obj = h_by_class("grenade", 0);
    } else {
        fire_grenade2(player, start, dir, 125, 0, fuse, 165, false);
        obj = h_by_class("hgrenade", 0);
    }
    if (!obj) imp_error("platform object did not spawn");
    VectorCopy(start, obj->s.origin);
    obj->s.origin[2] -= obj->mins[2];
    VectorCopy(obj->s.origin, obj->old_origin);
    VectorClear(obj->velocity);
    gi.linkentity(obj);
    return obj;
}

static void h_plat_object_ride(edict_t *plat, edict_t *obj, const char *name, int before, int linear)
{
    struct { int ticks, sounds, expected, first, last; } leg[2] = { 0 };
    float offset = obj->s.origin[2] - plat->s.origin[2], drift = 0;
    float last_grid_z = plat->s.origin[2];
    int item = !strcmp(name, "item"), lost = 0;
    for (int i = 0; i < 12 * h_rate; i++) {
        float z = plat->s.origin[2];
        int frame = h_frames, sound0 = h_nsounds;
        h_frame();
        float after = plat->s.origin[2];
        float rider_z = before ? z : after;
        int grid = frame % h_div == 0;
        int impact = grid && !item && rider_z != last_grid_z;
        if (grid)
            last_grid_z = rider_z;
        float dz = after - z;
        if (dz) {
            int down = dz < 0;
            leg[down].ticks++;
            leg[down].expected += impact;
            for (int j = sound0; j < h_nsounds; j++) {
                if (h_sounds_log[j].entnum != NUM(obj))
                    continue;
                if (leg[down].sounds++ == 0)
                    leg[down].first = frame;
                leg[down].last = frame;
            }
        }
        lost += obj->groundentity != plat;
        drift = fmaxf(drift, fabsf(obj->s.origin[2] - plat->s.origin[2] - offset));
        if (leg[1].ticks && plat->moveinfo.state == H_STATE_BOTTOM) break;
    }
    CHECK(!lost && drift <= 0.125f + H_EPS, "%s: %d ticks without platform support, max height change %.4f",
          name, lost, drift);
    for (int down = 0; down < 2; down++) {
        /* At 10 Hz a full leg has 16 impact sounds, or 15 when the object runs first. */
        int baseline = item ? 0 : 16 - before;
        int sounds = leg[down].sounds, ticks = leg[down].ticks;
        int wanted = linear || h_div == 1 ? baseline : leg[down].expected;
        CHECK(linear ? ticks == 16 * h_div : abs(ticks - 16 * h_div) <= h_div,
              "%s %s: platform travel lasts %d ticks (10 Hz: 16 windows)", name, down ? "down" : "up", ticks);
        CHECK(sounds == wanted && sounds <= ticks / h_div + 1,
              "%s %s: %d sounds, grid expected %d, frames %d..%d (10 Hz reference %d)",
              name, down ? "down" : "up", sounds, wanted, leg[down].first, leg[down].last, baseline);
    }
    CHECK(plat->moveinfo.state == H_STATE_BOTTOM, "%s: platform returned to bottom", name);
}

static void h_plat_object_lose_support(edict_t *plat, edict_t *obj, int phase, int grenade)
{
    /* Move the support sideways without carrying its rider. */
    if (!grenade && h_div > 1 && !h_grid_wait())
        h_frame();
    float z = obj->s.origin[2];
    plat->s.origin[0] += 256;
    gi.linkentity(plat);
    int falling = 0;
    int limit = grenade ? h_div : 1;
    for (int i = 0; i < limit && !falling; i++) {
        h_frame();
        falling = !obj->groundentity && obj->s.origin[2] < z;
    }
    CHECK(falling, "a support moved away drops its %s within %d ticks",
          grenade ? "grenade" : "item", limit);

    plat->s.origin[0] -= 256;
    gi.linkentity(plat);
    obj->s.origin[2] = plat->absmax[2] - 1 + 4 - obj->mins[2];
    VectorClear(obj->velocity);
    gi.linkentity(obj);
    h_frames_n(h_rate);
#ifdef HARNESS_OPENTDM
    while (level.realframenum % h_div != phase) h_frame();
#else
    while (h_frames % h_div != phase) h_frame();
#endif
    CHECK(obj->groundentity == plat, "rider settled again before support removal");
    z = obj->s.origin[2];
    G_FreeEdict(plat);
    h_frame();
    CHECK(!obj->groundentity, "removing support clears the rider's ground in the same tick (phase %d)", phase);
    for (int i = 1; i < limit && obj->s.origin[2] == z; i++)
        h_frame();
    CHECK(obj->s.origin[2] < z, "removing support drops the rider within %d ticks (phase %d)", limit, phase);
}

static void h_plat_object_scene(int linear)
{
    h_scene_base();
    int model = h_add_inline(-64, 1936, 256, 64, 2064, 320);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"256\"\n%s}\n", model,
          linear ? "\"speed\" \"1600\"\n\"accel\" \"1600\"\n\"decel\" \"1600\"\n" : "");
    h_start(h_ents);
    static const char *const names[] = { "grenade", "hand grenade", "item" };
    for (int kind = H_PLAT_GRENADE; kind <= H_PLAT_ITEM; kind++) {
        for (int before = 0; before < 2; before++) {
            if (kind || before) {
                while (h_frames % h_div)
                    h_frame();
#ifdef HARNESS_ENGINE
                he_set_game(ge);   /* Clear the outgoing map's collision links before reusing edicts. */
#endif
                h_frames = 0; // A map load restarts the engine frame counter.
                ge->SpawnEntities("harness", h_ents, "");
            }
            edict_t *plat = h_by_model("*1"), *player = h_join(0, "p1");
            edict_t *trigger = NULL;
            for (int n = 1; n < ge->num_edicts; n++) {
                edict_t *ent = EDICT(n);
                if (ent->inuse && ent->solid == SOLID_TRIGGER && ent->enemy == plat) trigger = ent;
            }
            if (!plat || !trigger) imp_error("platform or trigger did not spawn");
            if (before) {
                /* Reuse the slot of the unused single-player spawn point. */
                G_FreeEdict(h_by_class("info_player_start", 0));
                h_frames_n(3 * h_rate);
            }
            edict_t *obj = h_plat_object(player, plat, kind);
            CHECK((NUM(obj) < NUM(plat)) == before, "%s: object %d, platform %d", names[kind], NUM(obj), NUM(plat));
            h_frames_n(h_rate);
            CHECK(obj->groundentity == plat, "%s settled on platform", names[kind]);
            /* Move_Begin runs one tick after activation; the first push is a tick later. */
            if (linear)
                h_frames_n((h_grid_wait() + 2 * h_div - 2) % h_div);
            trigger->touch(trigger, player, NULL, NULL);
            h_plat_object_ride(plat, obj, names[kind], before, linear);
            h_plat_object_lose_support(plat, obj, (kind * 2 + before) % h_div, kind != H_PLAT_ITEM);
        }
    }
}

static void sc_t24_plat_object(void) { h_plat_object_scene(0); }
static void sc_t24_plat_object_linear(void) { h_plat_object_scene(1); }

#ifdef HARNESS_ENGINE
static void sc_t24_plat_real_debris(void)
{
    h_scene_base();
    int model = h_add_inline(-64, 1936, 256, 64, 2064, 320);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"256\"\n\"targetname\" \"lower\"\n}\n", model);
    h_start(h_ents);
    edict_t *plat = h_by_model("*1"), *player = h_join(0, "p1");
    if (!plat || !plat->use) imp_error("targeted platform did not spawn");
    char gib_model[] = "models/objects/gibs/sm_meat/tris.md2";
    vec3_t origin = { 8, 2000, 324 };
    edict_t *obj = NULL;
#ifdef HARNESS_OPENTDM
    ThrowDebris(player, gib_model, 1, origin);
    obj = h_by_class("debris", 0);
#else
    int index = gi.modelindex(gib_model);
    ThrowGib(player, index, 100, GIB_METALLIC);
    for (int i = game.maxclients + 1; i < ge->num_edicts; i++)
        if (EDICT(i)->inuse && EDICT(i)->s.modelindex == index && (EDICT(i)->s.effects & EF_GIB))
            obj = EDICT(i);
#endif
    if (!obj) imp_error("real debris did not spawn");
    VectorCopy(origin, obj->s.origin);
    VectorClear(obj->velocity);
    gi.linkentity(obj);
    h_frames_n(h_rate);
    CHECK(obj->solid == SOLID_NOT && !obj->area.prev && obj->groundentity == plat,
          "real non-solid debris settled without a collision area link");
    float offset = obj->s.origin[2] - plat->s.origin[2];
    plat->use(plat, player, player);
    float drift = 0;
    int moves = 0, lost = 0;
    for (int i = 0; i < h_rate; i++) {
        float z = plat->s.origin[2];
        h_frame();
        moves += plat->s.origin[2] < z;
        lost += obj->groundentity != plat;
        drift = fmaxf(drift, fabsf(obj->s.origin[2] - plat->s.origin[2] - offset));
    }
    CHECK(moves > 0, "real debris fixture platform descended");
    if (h_div > 1)
        CHECK(!lost && drift <= 0.01f, "real debris rides the descender (lost %d, drift %.4f)", lost, drift);
    else
        CHECK(drift > 0.01f, "10 Hz preserves the original non-solid debris ride");
}
#endif

static int h_late_support_thinks;

static void h_late_support_think(edict_t *ent)
{
    h_late_support_thinks++;
}

static void sc_t24_plat_late_support(void)
{
    h_scene_base();
    int model = h_add_inline(-32, 1968, 128, 32, 2032, 192);
    h_ent("{\n\"classname\" \"func_wall\"\n\"model\" \"*%d\"\n\"targetname\" \"support\"\n\"spawnflags\" \"7\"\n}\n", model);
    h_ent("{\n\"classname\" \"func_timer\"\n\"target\" \"support\"\n\"wait\" \"60\"\n}\n");
    h_start(h_ents);
    edict_t *player = h_join(0, "p1"), *wall = h_by_model("*1"), *timer = h_by_class("func_timer", 0);
    G_FreeEdict(h_by_class("info_player_start", 0));
    edict_t *item = Drop_Item(player, FindItem("Shells"));
    if (!item || !timer || !wall || NUM(item) >= NUM(timer)) imp_error("late support fixture needs item before timer");
    VectorSet(item->s.origin, 0, 2000, 196 - item->mins[2]);
    VectorClear(item->velocity);
    gi.linkentity(item);
    h_frames_n(h_rate);
    CHECK(item->groundentity == wall, "item settled on the toggleable wall");
    while (h_frames % h_div != h_div - 1) h_frame();
    timer->nextthink = level.framenum + 1;
    item->nextthink = timer->nextthink;
    item->think = h_late_support_think;
    h_late_support_thinks = 0;
    h_frame();
    float z = item->s.origin[2];
    h_frame();
    CHECK(wall->solid == SOLID_NOT, "upper slot timer switched off the wall");
    CHECK(h_late_support_thinks == 1, "lower slot item thinks just once during support removal");
    if (h_div > 1) {
        CHECK(!item->groundentity && item->s.origin[2] < z,
              "lower slot item starts falling in the tick of late wall removal");
    } else {
        CHECK(item->groundentity == wall && item->s.origin[2] == z,
              "10 Hz preserves the original late support tick");
        h_frame();
        CHECK(!item->groundentity && item->s.origin[2] < z, "10 Hz item falls on its next turn");
    }
}

static edict_t *h_plat_fuse_support;
static int h_plat_blast_frame;
static float h_plat_blast_offset;
static void (*h_plat_grenade_explode)(edict_t *);

static void h_plat_fuse_explode(edict_t *grenade)
{
    h_plat_blast_frame = h_frames;
    h_plat_blast_offset = grenade->s.origin[2] - h_plat_fuse_support->s.origin[2];
    h_plat_grenade_explode(grenade);
}

static void sc_t24_plat_fuse(void)
{
    h_scene_base();
    int model = h_add_inline(-64, 1936, 256, 64, 2064, 320);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"256\"\n\"targetname\" \"lower\"\n}\n", model);
    h_start(h_ents);
    edict_t *plat = h_by_model("*1"), *player = h_join(0, "p1");
    if (!plat || !plat->use) imp_error("targeted platform did not spawn");
    edict_t *grenade = h_plat_object(player, plat, H_PLAT_GRENADE);
    h_frames_n(h_rate);
    CHECK(grenade->groundentity == plat, "timed grenade settled on the platform");
    float offset = grenade->s.origin[2] - plat->s.origin[2];
    while (h_frames % h_div) h_frame();
    h_plat_fuse_support = plat;
    h_plat_grenade_explode = grenade->think;
    grenade->think = h_plat_fuse_explode;
    grenade->nextthink = level.framenum + h_rate;
    int deadline = h_frames + h_rate, messages = h_nmsgs;
    h_plat_blast_frame = -1;
    plat->use(plat, player, player);
    h_frames_n(h_rate + 1);
    CHECK(plat->moveinfo.state == H_STATE_DOWN && plat->s.origin[2] < 0, "grenade fuse ran during real platform descent");
    CHECK(h_plat_blast_frame == deadline && h_explosions(messages) == 1,
          "riding grenade explodes at its grid deadline (got %d, expected %d)", h_plat_blast_frame, deadline);
    CHECK(fabsf(h_plat_blast_offset - offset) <= 0.01f,
          "riding grenade explodes at its carried height (offset %.4f, expected %.4f)", h_plat_blast_offset, offset);
}

static edict_t *h_stationary_support;
static int h_stationary_contacts;
static void (*h_stationary_touch)(edict_t *, edict_t *, cplane_t *, csurface_t *);

static void h_count_stationary_contact(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    h_stationary_contacts += other == h_stationary_support;
    h_stationary_touch(self, other, plane, surf);
}

static void h_check_stationary_support(edict_t *pusher, edict_t *grenade, int windows)
{
    CHECK(grenade->groundentity == pusher, "grenade rests on the pusher before stationary windows");
    h_stationary_support = pusher;
    h_stationary_contacts = 0;
    h_stationary_touch = grenade->touch;
    grenade->touch = h_count_stationary_contact;
    vec3_t origin;
    VectorCopy(pusher->s.origin, origin);
    int lost = 0, moved = 0, links = pusher->linkcount;
    for (int i = 0; i < windows * h_div; i++) {
        h_frame();
        moved += !VectorCompare(pusher->s.origin, origin);
        lost += grenade->groundentity != pusher;
    }
    grenade->touch = h_stationary_touch;
    CHECK(!moved, "pusher stays in place throughout %d grid windows", windows);
    if (h_div == 1) {
        CHECK(pusher->linkcount - links == windows, "10 Hz still relinks a stationary push");
    } else {
        CHECK(!lost && !h_stationary_contacts,
              "stationary push keeps grenade support without contact (lost %d, contacts %d)",
              lost, h_stationary_contacts);
    }
}

static void sc_t24_plat_zero_push(void)
{
    h_scene_base();
    int model = h_add_inline(-64, 1936, 256, 64, 2064, 320);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"256\"\n}\n", model);
    h_start(h_ents);
    edict_t *player = h_join(0, "p1"), *plat = h_by_model("*1");
    edict_t *grenade = h_plat_object(player, plat, H_PLAT_GRENADE);
    h_frames_n(h_rate);
    h_frames_n(h_grid_wait());
    /* Two windows of travel add up to less than half a position quantum. */
    plat->velocity[0] = 0.25f;
    h_check_stationary_support(plat, grenade, 2);
}

#ifdef HARNESS_OPENTDM
static void sc_t24_door_final_push(void)
{
    h_scene_base();
    int model = h_add_inline(-64, 1936, 256, 64, 2064, 320);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"speed\" \"90\"\n"
          "\"targetname\" \"open\"\n\"wait\" \"-1\"\n}\n", model);
    h_start(h_ents);
    edict_t *player = h_join(0, "p1"), *door = h_by_model("*1");
    edict_t *grenade = h_plat_object(player, door, H_PLAT_GRENADE);
    h_frames_n(h_rate);
    h_frames_n((h_grid_wait() + 1) % h_div);
    door->use(door, player, player);
    /* Run the last travel frame on the grid so the grenade has seen the real move. */
    h_frames_n(42);
    CHECK(door->s.origin[0] == 120 && door->velocity[0] != 0 &&
          fabsf(door->velocity[0]) < 0.01f, "door arrives on the grid with a tiny final push pending");
    CHECK(grenade->groundentity_linkcount == door->linkcount, "grenade has seen the door's last real move");
    h_check_stationary_support(door, grenade, 1);
    CHECK(door->moveinfo.state == H_STATE_TOP, "door finishes its move after the final push");
}

static edict_t *h_view_player(void)
{
    edict_t *a, *b;
    h_set_override("g_timeout_limit", "0");
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return NULL;
    h_place(a, 200, 0, 24);
    h_tick(a, 0, 1);
    h_place(b, 0, 0, 24);
    h_tick(b, 0, 1);
    h_frames_n(h_rate);
    return a;
}

static void h_view_pause(edict_t *p)
{
    h_client_command(p, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "timeout started (status %d)", tdm_match_status);
}

static void h_view_resume(edict_t *p)
{
    h_client_command(p, "time");
    for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) h_frame();
    CHECK(tdm_match_status == MM_PLAYING, "match resumed (status %d)", tdm_match_status);
}

static void h_view_sync(void)
{
    while (!FRAMESYNC) h_frame();
}

static int h_view_neutral(edict_t *p)
{
    player_state_t *ps = &p->client->ps;
    return VectorCompare(ps->kick_angles, vec3_origin) && ps->viewoffset[2] == p->viewheight;
}

static void sc_t24_view_timeout_fall(void)
{
    edict_t *p = h_view_player();
    if (!p) return;
    player_state_t *ps = &p->client->ps;
    h_place(p, 200, 0, 24 + 512);
    for (int i = 0; i < 3 * h_rate && !ps->kick_angles[PITCH]; i++) h_tick(p, 0, 1);
    CHECK(p->groundentity && ps->kick_angles[PITCH] > 0 && ps->viewoffset[2] < p->viewheight,
          "landed with fall pitch %.4f and view height %.4f", ps->kick_angles[PITCH], ps->viewoffset[2]);
    h_view_pause(p);
    h_frames_n(3 * h_div);
    CHECK(h_view_neutral(p), "fall recovers in 0.3 s of timeout (pitch %.4f, height %.4f, want 0 and %d)",
          ps->kick_angles[PITCH], ps->viewoffset[2], p->viewheight);
    int tilted = 0;
    h_client_command(p, "time");
    for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) {
        h_frame();
        tilted += !h_view_neutral(p);
    }
    CHECK(tdm_match_status == MM_PLAYING && !tilted, "view stays neutral until resume (%d tilted ticks)", tilted);
    for (int i = 0; i < h_rate; i++) {
        h_tick(p, 0, 1);
        tilted += !h_view_neutral(p);
    }
    CHECK(!tilted, "fall kick does not return after resume (%d tilted ticks since recovery)", tilted);
}

static void h_view_damage(edict_t *p)
{
    edict_t *attacker = EDICT(2);
    vec3_t dir = { -1, 0, 0 }, point, normal = { 0, 0, 0 }, velocity;
    VectorCopy(p->s.origin, point);
    point[0] += 100;
    point[1] += 100;
    int health = p->health;
    VectorCopy(p->velocity, velocity);
    T_Damage(p, attacker, attacker, dir, point, normal, 10, 20, DAMAGE_NO_KNOCKBACK, MOD_RAILGUN);
    CHECK(p->health == health - 10 && VectorCompare(velocity, p->velocity),
          "damage hit for 10 health without changing velocity");
}

/* Wait for the projectile: the BFG muzzleflash is the start of the windup. */
static int h_view_bfg(edict_t *p)
{
    h_give(p, "bfg10k", 0);
    h_give(p, "cells", 100);
    h_use(p, "bfg10k");
    h_view_sync();
    int ammo = h_ammo(p);
    int fired = 0;
    for (int i = 0; i < 3 * h_rate && !fired; i++) {
        init_genrand(1); /* Pin the random BFG roll before each possible firing frame. */
        h_tick(p, i == 0 ? BUTTON_ATTACK : 0, 1);
        fired = h_by_class("bfg blast", 0) != NULL && h_ammo(p) == ammo - 50;
    }
    CHECK(fired, "BFG projectile fired");
    return fired ? level.realframenum - 1 : -1;
}

static void h_view_timeout_kick(edict_t *p, const char *event, int delay)
{
    h_view_pause(p);
    float *angles = p->client->ps.kick_angles;
    float previous = fabsf(angles[PITCH]);
    int onset = previous > 0 ? 0 : -1, rises = 0, late = 0;
    for (int i = 1; i <= h_rate; i++) {
        h_frame();
        float pitch = fabsf(angles[PITCH]);
        if (pitch > 0 && onset < 0) onset = i;
        rises += pitch > previous;
        late += i >= delay + 5 * h_div && !VectorCompare(angles, vec3_origin);
        previous = pitch;
    }
    CHECK(onset == delay && rises == (delay != 0), "%s kick appears once at tick %d (onset %d, %d increases)",
          event, delay, onset, rises);
    CHECK(!late && tdm_match_status == MM_TIMEOUT, "%s kick recovers within 0.5 s of onset in timeout (%d late ticks)",
          event, late);
    h_view_resume(p);
    CHECK(h_view_neutral(p), "%s kick stays neutral at resume", event);
}

static void sc_t24_view_timeout_damage(void)
{
    edict_t *p = h_view_player();
    if (!p) return;
    h_view_sync();
    if (h_div > 1) h_frame();   /* Enter timeout before the next damage feedback sample. */
    h_view_damage(p);
    if (h_div == 1) h_frame();
    h_view_timeout_kick(p, "damage", h_div > 1 ? h_div : 0);

    if (h_view_bfg(p) < 0) return;
    for (int i = 0; i < h_div && !p->client->ps.kick_angles[PITCH]; i++) h_frame();
    h_view_timeout_kick(p, "BFG", 0);
}

/* Pitch and roll at the 100 ms presentation samples from the pre-fix box
 * harness. Fall height was sampled every world tick, including at 60 Hz. */
static const float h_view_fall_angles[5][2] = {
    { 12.5f, 0 }, { 8.333334f, 0 }, { 4.166667f, 0 },
};
static const float h_view_fall_height60[] = {
    17, 17.277779f, 17.555555f, 17.833334f, 18.111111f, 18.388889f,
    18.666666f, 18.944445f, 19.222221f, 19.5f, 19.777779f, 20.055555f,
    20.333334f, 20.611111f, 20.888889f, 21.166666f, 21.444445f, 21.722221f,
};
static const float h_view_damage_angles[2][5][2] = {
    {
        { -4.7140455f, -4.7140455f }, { -3.7712364f, -3.7712364f }, { -2.8284273f, -2.8284273f },
        { -1.8856182f, -1.8856182f }, { -0.9428091f, -0.9428091f },
    }, {
        /* The second hit is at 80 health, after the timeout. */
        { -5.3033018f, -5.3033018f }, { -4.2426414f, -4.2426414f }, { -3.1819813f, -3.1819813f },
        { -2.1213207f, -2.1213207f }, { -1.0606604f, -1.0606604f },
    },
};
static const float h_view_bfg_angles[5][2] = {
    { -40, 6.6723518f }, { -32, 5.3378816f }, { -24, 4.0034113f },
    { -16, 2.6689408f }, { -8, 1.3344704f },
};

static void h_view_check_trace(edict_t *p, const char *event, const float angles[5][2])
{
    int wrong = 0;
    for (int i = 0; i <= 6 * h_div; i++) {
        player_state_t *ps = &p->client->ps;
        int sample = i / h_div;
        const float *want = sample < 5 ? angles[sample] : vec3_origin;
        float z = 22;
        if (angles == h_view_fall_angles && i < 3 * h_div)
            z = h_view_fall_height60[i * 6 / h_div];
        if (fabsf(ps->kick_angles[PITCH] - want[0]) > 0.00001f || ps->kick_angles[YAW] ||
            fabsf(ps->kick_angles[ROLL] - want[1]) > 0.00001f || fabsf(ps->viewoffset[2] - z) > 0.00001f) {
            if (!wrong++)
                printf("  info %s tick %d: pitch %.6f roll %.6f height %.6f (want %.6f %.6f %.6f)\n",
                       event, i, ps->kick_angles[PITCH], ps->kick_angles[ROLL], ps->viewoffset[2], want[0], want[1], z);
        }
        h_frame();
    }
    CHECK(!wrong, "%s camera matches the pre-fix trace (%d mismatched ticks)", event, wrong);
}

static void h_view_check_bfg(edict_t *p, int shot)
{
    int wrong = 0;
    for (int i = 0; i <= 6 * h_div; i++) {
        const player_state_t *ps = &p->client->ps;
        int sample = (level.realframenum - 1) / h_div * h_div;
        int elapsed = sample - shot;
        float fraction = elapsed < 0 ? 0 : fmaxf(0, 1.f - (float)elapsed / (5 * h_div));
        float pitch = h_view_bfg_angles[0][0] * fraction;
        float roll = h_view_bfg_angles[0][1] * fraction;
        if (fabsf(ps->kick_angles[PITCH] - pitch) > 0.00001f || ps->kick_angles[YAW] ||
            fabsf(ps->kick_angles[ROLL] - roll) > 0.00001f || fabsf(ps->viewoffset[2] - 22) > 0.00001f) {
            if (!wrong++)
                printf("  info BFG sample %d, shot %d: pitch %.6f roll %.6f (want %.6f %.6f)\n",
                       sample, shot, ps->kick_angles[PITCH], ps->kick_angles[ROLL], pitch, roll);
        }
        h_frame();
    }
    CHECK(!wrong, "BFG camera takes the first grid sample after the shot and decays for 0.5 s (%d mismatched ticks)",
          wrong);
}

static void sc_t24_view_no_timeout(void)
{
    /* The pre-fix trace below is recorded for these two rates only. */
    if (h_rate != 10 && h_rate != 60) {
        printf("  info pre-fix camera trace exists for 10 and 60 Hz only, rate %d not compared\n", h_rate);
        return;
    }
    edict_t *p = h_view_player();
    if (!p) return;
    for (int after = 0; after < 2; after++) {
        if (after) {
            h_view_pause(p);
            h_frames_n(h_rate);
            h_view_resume(p);
            h_frames_n(h_rate);
            CHECK(level.realframenum > level.framenum, "match clock is behind the world clock after resume");
        }
        /* The box Pmove cannot fall: feed the landing velocity transition
         * to the real server frame, on the floor and at a presentation tick. */
        h_view_sync();
        p->client->oldvelocity[2] = -500;
        h_frame();
        h_view_check_trace(p, "fall", h_view_fall_angles);
        h_view_sync();
        h_view_damage(p);
        h_frame();
        h_view_check_trace(p, "damage", h_view_damage_angles[after]);
        int shot = h_view_bfg(p);
        if (shot < 0) return;
        CHECK(shot % h_div == 0, "BFG fires on a grid frame (frame %d)", shot);
        if (h_div == 1)
            h_view_check_trace(p, "BFG", h_view_bfg_angles);
        else
            h_view_check_bfg(p, shot);
        edict_t *blast = h_by_class("bfg blast", 0);
        if (blast) G_FreeEdict(blast);
    }
}

static void sc_t24_view_frozen(void)
{
    edict_t *p = h_view_player();
    if (!p) return;
    h_give(p, "quad damage", 0);
    h_client_command(p, "use quad damage");
    h_view_sync();
    VectorSet(p->velocity, 180, 0, 0);
    h_frame();
    player_state_t *ps = &p->client->ps;
    vec3_t kick, gun, offset;
    CHECK(ps->kick_angles[PITCH] != 0 && ps->gunangles[PITCH] != 0 && p->client->bobtime > 0 &&
          p->client->quad_framenum > (unsigned)level.framenum && p->air_finished_framenum > (unsigned)level.framenum,
          "moving player has active speed tilt, bob, gun angle, quad and air");
    p->waterlevel = 3;
    p->watertype = CONTENTS_LAVA | CONTENTS_WATER;
    h_view_pause(p);
    h_view_sync();
    VectorCopy(ps->kick_angles, kick);
    VectorCopy(ps->gunangles, gun);
    VectorCopy(ps->viewoffset, offset);
    float bob = p->client->bobtime;
    int frame = level.framenum, health = p->health;
    unsigned quad = p->client->quad_framenum, air = p->air_finished_framenum;
    h_frame();
    CHECK(VectorCompare(ps->kick_angles, kick) && VectorCompare(ps->gunangles, gun) &&
          VectorCompare(ps->viewoffset, offset) && p->client->bobtime == bob,
          "view stays fixed on the first frozen frame");
    CHECK(level.framenum == frame && p->client->quad_framenum == quad &&
          p->air_finished_framenum == air && p->health == health,
          "lava, air and powerup stay frozen on the first frozen frame");
    int view_changed = 0, world_changed = 0;
    for (int i = 0; i < h_rate; i++) {
        h_frame();
        view_changed += !VectorCompare(ps->kick_angles, kick) || !VectorCompare(ps->gunangles, gun) ||
                        !VectorCompare(ps->viewoffset, offset) || p->client->bobtime != bob;
        world_changed += level.framenum != frame || p->client->quad_framenum != quad ||
                         p->air_finished_framenum != air || p->health != health;
    }
    CHECK(!view_changed, "speed tilt, bob and weapon angle stay frozen (%d changed ticks)", view_changed);
    CHECK(!world_changed && tdm_match_status == MM_TIMEOUT, "powerup, air and environmental damage stay frozen (%d changed ticks)",
          world_changed);
    h_view_resume(p);
    h_frames_n(h_div);
    CHECK(p->health < health, "lava damages the player after resume");
}
#endif

static void h_check_crush_log(int start, const char *expected)
{
    const char prefix[] = "Crush limiter: ";
    int lines = 0, matches = 0;
    for (int i = start; i < h_ndprints; i++) {
        const char *text = h_dprints[i].text;
        lines += !strncmp(text, prefix, sizeof(prefix) - 1);
        matches += expected && !strcmp(text, expected);
    }
    CHECK(lines == (expected != NULL) && matches == lines, "crush log: %d lines, %d expected matches for %s",
          lines, matches, expected ? expected : "no overflow");
}

static void sc_t24_crush_overflow_count(void)
{
    const char first_line[] = "Crush limiter: table full (64 pairs), limiting per victim\n";
    h_scene_base();
    h_start(h_ents);
    edict_t *s1 = G_Spawn(), *s2 = G_Spawn(), *victim = G_Spawn();
    h_crush_spawn_fill();
    CHECK(h_crush_fill() == 64 && level.crush_overflows == 0, "64 active pairs fit without overflow");
    h_check_crush_log(0, NULL);

    int start = h_ndprints, first, gap;
    int hits = h_crush_run(s1, s2, victim, 3 * h_div, &first, &gap);
    CHECK(first == 0 && hits == 3 && gap == h_div && level.crush_overflows == 3,
          "fallback allows 3 hits at 100 ms intervals and counts them (hits %d, total %llu)", hits, level.crush_overflows);
    h_check_crush_log(start, first_line);

    start = h_ndprints;
    ge->SpawnEntities("next", h_ents, "");
    CHECK(level.crush_overflows == 0, "overflow count resets at map change");
    h_check_crush_log(start, "Crush limiter: overflow total on harness = 3\n");

    start = h_ndprints;
    s1 = G_Spawn();
    victim = G_Spawn();
    h_crush_spawn_fill();
    CHECK(h_crush_fill() == 64 && G_CrushAllowed(s1, victim) && level.crush_overflows == 1,
          "first fallback hit on the new map starts a new count");
    h_check_crush_log(start, first_line);

    start = h_ndprints;
    h_shutdown();
    CHECK(level.crush_overflows == 0, "overflow count resets at shutdown");
    h_check_crush_log(start, "Crush limiter: overflow total on next = 1\n");
    h_start(h_ents);
    CHECK(level.crush_overflows == 0, "next map starts without a prior count");
    h_shutdown();
    h_check_crush_log(start, "Crush limiter: overflow total on next = 1\n");
}

/* ---------------------------------------------------------------- T17 */

#ifdef HARNESS_OPENTDM
/* `ghost` (join code) is rate limited for 200 ms, as at 10 Hz: a second
 * command in the same frame and one 100 ms later are ignored, one 200 ms
 * after the first is processed */
static void sc_t01_fixed_engine(void)
{
    h_scene_base();
    h_start(h_ents);
    game.server_features &= ~GMF_VARIABLE_FPS;
    imp_cvar_set("sv_fps", "60");
    TDM_SetFrameTime();
    CHECK(SERVER_FPS == 10 && fabsf(FRAMETIME - 0.1f) < 1e-6f,
          "fixed engine ignores sv_fps 60: 10 Hz, 0.1 s (got %d, %.6f)", SERVER_FPS, FRAMETIME);
}

static void sc_t17_ghost_ratelimit(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    edict_t *s = h_connect(2, "spec");      /* a spectator: ghost works only from spectator mode */
    if (!h_tdm_start(a, b)) return;
    static const struct { int frames; int limited; } at[] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 0 } };
    int f0 = h_frames;
    for (int k = 0; k < 4; k++) {
        while (h_frames - f0 < at[k].frames * h_rate / 10) h_frame();
        int tx = h_ntexts;
        h_client_command(s, "ghost 12345");
        int limited = h_text_to(s, 0, "rate limiting", tx) != NULL;
        int done = h_text_to(s, 0, "No client information found", tx) != NULL;
        CHECK(limited == at[k].limited && done == !at[k].limited, "ghost %d ms after the first: %s (got %s)",
              at[k].frames * 100, at[k].limited ? "rate limited" : "processed",
              limited ? "rate limited" : done ? "processed" : "nothing");
    }
}

/* g_ping_handicap: the displayed ping jitters as at 10 Hz on every
 * scoreboard refresh (3 s): +3 every 4th refresh, -2 two refreshes later,
 * the plain value between */
static void sc_t17_ping_handicap(void)
{
    h_set_override("g_ping_handicap", "50");
    h_scene_base();
    h_start(h_ents);
    int stride = 3 * h_rate;
    for (int n = 0; n < 8;) {
        h_frame();
        if (level.framenum % stride) continue;
        int k = level.framenum / stride, want = k % 4 == 0 ? 53 : k % 4 == 2 ? 48 : 50;
        int got = TDM_PingHandicap(0);
        CHECK(got == want, "refresh %d (frame %d): handicap ping %d (got %d)", k, level.framenum, want, got);
        n++;
    }
}
#endif

/* ---------------------------------------------------------------- T18 */

/* Player presentation (p_view.c) at the 10 Hz contract: feedback that
 * stock Q2 computes once per world frame is computed once per 100 ms at
 * any rate, on the real frame counter (it keeps running in a timeout).
 * The world is driven with bare h_frame() where the box Pmove would reset
 * the field under test (ground, water level, view angles). */
/* idle frames until the next frame has real frame phase k of the 100 ms
 * window (ClientEndServerFrame sees realframenum before its increment) */
static void h_real_phase(int k)
{
#ifdef HARNESS_OPENTDM
    while (level.realframenum % h_div != k) h_frame();
#else
    while (h_frames % h_div != k) h_frame();
#endif
}

static void h_hit(edict_t *p, int damage)
{
    vec3_t dir = { 0, 0, 0 };
    T_Damage(p, g_edicts, g_edicts, dir, p->s.origin, dir, damage, 0, 0, MOD_TRIGGER_HURT);
}

#ifdef HARNESS_OPENTDM
/* One hit, in every phase of the window: the HUD flash lasts one 100 ms
 * window (h_div frames), not one world frame. */
static void sc_t18_flash_window(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    for (int k = 0; k < h_div; k++) {
        h_give(p, "health", 100);
        h_frames_n(h_rate);
        h_real_phase(k);
        h_hit(p, 10);
        int n = 0;
        for (int i = 0; i < 3 * h_div; i++) {
            h_frame();
            n += h_stat(p, STAT_FLASHES) != 0;
        }
        CHECK(n == h_div, "hit in phase %d/%d: flash for %d frames (got %d)", k, h_div, h_div, n);
    }
}

/* Six 1-point hits inside one 100 ms window (all in one frame at 10 Hz,
 * spread over the window's frames at 60): one feedback batch, the blend
 * peaks at the 10 Hz value 0.2 (the minimum visible alpha). */
static void sc_t18_blend_batches(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    h_real_phase(1 % h_div);    /* the window ends with the last hit's frame */
    float peak = 0;
    for (int f = 0, i = 0; f < 3 * h_div; f++) {
        for (; i < 6 && i * h_div / 6 == f; i++) h_hit(p, 1);
        h_frame();
        if (p->client->ps.blend[3] > peak) peak = p->client->ps.blend[3];
    }
    CHECK(fabsf(peak - 0.2f) < 0.01f, "six hits in one window: peak blend alpha 0.2 (got %.3f)", peak);
}

/* Turning at 90 deg/s for 0.5 s: the gun lags by 0.2 of the view turn of
 * one 100 ms window, 0.2 * 9 = 1.8 deg yaw, at any rate. */
static void sc_t18_gun_sway(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    float yaw = p->client->ps.viewangles[YAW], peak = 0;
    for (int i = 0; i < h_rate / 2; i++) {
        yaw += 90.0f / h_rate;
        p->client->ps.viewangles[YAW] = p->client->v_angle[YAW] = yaw;
        h_frame();
        if (fabsf(p->client->ps.gunangles[YAW]) > peak) peak = fabsf(p->client->ps.gunangles[YAW]);
    }
    CHECK(fabsf(peak - 1.8f) < 0.05f, "turning 90 deg/s: peak gun yaw 1.8 (got %.3f)", peak);
}

/* Running, then airborne for 0.5 s: the bob cycle stops in the air. */
static void sc_t18_bob_air(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    VectorSet(p->velocity, 300, 0, 0);
    h_frames_n(h_rate / 2);
    float run = p->client->bobtime;
    CHECK(run > 0, "running: the bob cycle advances (bobtime %.3f)", run);
    p->groundentity = NULL;
    h_frame();
    float air = p->client->bobtime;
    for (int i = 0; i < h_rate / 2; i++) {
        p->groundentity = NULL;
        h_frame();
    }
    CHECK(air == run && p->client->bobtime == run, "airborne: bobtime stays %.4f (got %.4f, then %.4f)",
          run, air, p->client->bobtime);
}

/* In lava for 2 s: one burn sound per second (the 1 s debounce follows
 * both burn sounds), whatever burn1/burn2 the dice pick. */
static void sc_t18_lava_sound(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    int s = h_nsounds;
    for (int i = 0; i < 2 * h_rate; i++) {
        p->waterlevel = 1;
        p->watertype = CONTENTS_LAVA;
        h_frame();
    }
    int b1 = h_find(h_sounds, "player/burn1.wav"), b2 = h_find(h_sounds, "player/burn2.wav");
    int burns = (b1 > 0 ? h_count_sounds(b1, s) : 0) + (b2 > 0 ? h_count_sounds(b2, s) : 0);
    CHECK(p->health > 0, "alive in lava (health %d)", p->health);
    CHECK(burns == 2, "2 s in lava: 2 burn sounds (got %d)", burns);
}

#endif

/* sound calls since log index `from` whose name starts with `prefix` */
static int h_count_prefix(const char *prefix, int from)
{
    int c = 0;
    for (int i = from; i < h_nsounds; i++) {
        const char *n = h_sounds[h_sounds_log[i].soundindex];
        c += n && !strncmp(n, prefix, strlen(prefix));
    }
    return c;
}

#ifdef HARNESS_OPENTDM
/* A hard landing (fall damage), in every phase of the window: the fall
 * sound, and no normal pain sound with it (the feedback of its window). */
static void sc_t18_fall_pain(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    for (int k = 0; k < h_div; k++) {
        h_give(p, "health", 100);
        h_frames_n(h_rate);
        h_real_phase(k);
        int s = h_nsounds, hp = h_hp(p);
        p->velocity[2] = -600;      /* landed: delta 600, 36 > 30 */
        h_frames_n(2 * h_div);
        VectorClear(p->velocity);
        VectorClear(p->client->oldvelocity);
        int fall = h_count_prefix("*fall", s), pain = h_count_prefix("*pain", s);
        CHECK(h_hp(p) < hp && fall == 1, "phase %d/%d: fall damage and its sound (health+armor %d -> %d, %d sounds)",
              k, h_div, hp, h_hp(p), fall);
        for (int i = s; i < h_nsounds; i++) {
            const char *sound = h_sounds[h_sounds_log[i].soundindex];
            if (sound && !strncmp(sound, "*fall", 5))
                CHECK(h_sounds_log[i].frame % h_div == 0, "phase %d: fall sound is on the grid (at %d)",
                      k, h_sounds_log[i].frame);
        }
        CHECK(pain == 0, "phase %d/%d: no pain sound with the fall (got %d)", k, h_div, pain);
    }
}

/* Drowning for 4 s past the 12 s of air: a gurp sound with each hit, no
 * normal pain sound with them. */
static void sc_t18_drown_pain(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    int s = h_nsounds;
    for (int i = 0; i < 16 * h_rate; i++) {
        p->waterlevel = 3;
        p->watertype = CONTENTS_WATER;
        h_frame();
    }
    int gurp = h_count_prefix("*gurp", s), pain = h_count_prefix("*pain", s);
    CHECK(gurp >= 3, "drowning: gurp sounds (got %d)", gurp);
    CHECK(pain == 0, "drowning: no pain sound (got %d)", pain);
}

static void h_check_powerup_warning_resume(int sound, int mark, int resume)
{
    int count = 0, frame = -1;
    for (int i = mark; i < h_nsounds; i++) {
        if (h_sounds_log[i].soundindex == sound) {
            count++;
            frame = h_sounds_log[i].frame;
        }
    }
    CHECK(count == 1 && frame == resume,
          "powerup warns once on the first unfrozen grid frame (got %d warnings at %d, want frame %d)",
          count, frame, resume);
}

/* The warning becomes eligible on the same grid frame as the timeout. */
static void sc_t18_powerup_warn_timeout(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    h_give(a, "quad damage", 0);
    h_client_command(a, "use quad damage");
    h_real_phase(0);
    h_frame();
    CHECK(a->s.effects & EF_QUAD, "quad active");
    while (a->client->quad_framenum - level.framenum > 3 * h_rate + (h_div > 1)) h_frame();
    int s = h_nsounds;
    int sound = imp_soundindex("items/damage2.wav");
    h_view_pause(a);
    h_frames_n(h_rate);
    CHECK(h_count_sounds(sound, s) == 0, "no warning while the powerup clock is frozen");
    h_view_resume(a);
    int resume = h_frames - 1;
    h_frames_n(h_rate);
    h_check_powerup_warning_resume(sound, s, resume);
}

/* Lava and slime stay frozen, then tick at the resumed world grid. */
static void sc_t18_environment_resume(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    h_view_pause(a);
    for (int step = 0; step <= h_div && !level.timeout_frozen; step++)
        h_frame();
    CHECK(tdm_match_status == MM_TIMEOUT && level.timeout_frozen, "world is frozen");
    a->waterlevel = 1;
    a->watertype = CONTENTS_LAVA;
    int hp = h_hp_now(a), frozen = level.framenum;
    h_frames_n(h_div + 1);
    CHECK(level.framenum == frozen && h_hp_now(a) == hp, "no world damage while paused");
    a->waterlevel = a->watertype = 0;
    h_client_command(a, "time");
    for (int i = 0; i < 15 * h_rate && tdm_match_status == MM_TIMEOUT; i++) h_frame();
    CHECK(tdm_match_status == MM_PLAYING, "resumed");
    if (h_div > 1)
        CHECK(level.framenum % h_div == level.realframenum % h_div, "game and real phases agree");
    for (int liquid = 0; liquid < 2; liquid++) {
        h_give(a, "health", 100);
        int total = 0, wrong = 0;
        for (int i = 0; i < h_rate; i++) {
            a->waterlevel = 1;
            a->watertype = liquid ? CONTENTS_SLIME : CONTENTS_LAVA;
            int before = h_hp_now(a);
            int want = level.realframenum % h_div == 0 ? (liquid ? 1 : 3) : 0;
            h_frame();
            int damage = before - h_hp_now(a);
            total += damage;
            wrong += damage != want;
        }
        CHECK(!wrong && total == (liquid ? 10 : 30),
              "%s damage on real grid (%d wrong frames, total %d)",
              liquid ? "slime" : "lava", wrong, total);
    }
}

/* A rocket kick shown on a 100 ms window, then a timeout freezes the game
 * clock on the next frame (not a window start at 60 Hz): the view kick
 * still returns to zero within 0.2 s of real frames. */
static void sc_t18_kick_timeout(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    a->flags |= FL_GODMODE;
    b->flags |= FL_GODMODE;
    h_give(a, "rocket launcher", 0);
    h_give(a, "rockets", 10);
    h_use(a, "rocket launcher");
    h_tick(a, BUTTON_ATTACK, 1);
    for (int i = 0; i < 3 * h_div && !a->client->ps.kick_angles[PITCH]; i++) h_tick(a, 0, 1);
    CHECK(a->client->ps.kick_angles[PITCH] != 0, "rocket: view kick shown (%.2f)", a->client->ps.kick_angles[PITCH]);
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "timeout (status %d)", tdm_match_status);
    int zero = -1;
    for (int i = 0; i < 2 * h_div && zero < 0; i++) {
        h_frame();
        if (VectorCompare(a->client->ps.kick_angles, vec3_origin)) zero = i;
    }
    CHECK(zero >= 0, "timeout: view kick back to zero within 0.2 s (still %.2f %.2f %.2f)",
          a->client->ps.kick_angles[0], a->client->ps.kick_angles[1], a->client->ps.kick_angles[2]);
}
#endif

enum { TOSS_FLOOR, TOSS_WALL, TOSS_CORNER, TOSS_STAIRS, TOSS_SCENES };
enum { TOSS_STARTS = 6, TOSS_ARC_STEPS = 35, TOSS_MAX_HITS = 8 };

typedef struct {
    float x, z;
} toss_point_t;

typedef struct {
    int tick, face;
    toss_point_t point;
} toss_hit_t;

typedef struct {
    int stop, count;
    toss_point_t path[TOSS_ARC_STEPS];
    toss_hit_t hits[TOSS_MAX_HITS];
} toss_arc_t;

typedef struct {
    int count;
    toss_point_t entry, stop;
} toss_pad_ref_t;

#include "toss-reference.h"

static struct {
    int start, count, exploded;
    toss_hit_t hits[TOSS_MAX_HITS];
    toss_point_t explosion;
    float impact_y, explosion_y;
    void (*touch)(edict_t *, edict_t *, cplane_t *, csurface_t *);
    void (*think)(edict_t *);
} toss_events;

static toss_point_t h_toss_point(const edict_t *ent)
{
    return (toss_point_t){ ent->s.origin[0], ent->s.origin[2] };
}

static float h_toss_error(toss_point_t a, toss_point_t b)
{
    return hypotf(a.x - b.x, a.z - b.z);
}

static void h_toss_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    int face = plane ? (int)plane->normal[0] + 2 * (int)plane->normal[2] : 0;
    if (self != other && toss_events.count < TOSS_MAX_HITS)
        toss_events.hits[toss_events.count] = (toss_hit_t){
            h_frames + 1 - toss_events.start, face, h_toss_point(self)
        };
    if (self != other) toss_events.impact_y = self->s.origin[1];
    toss_events.count += self != other;
    if (toss_events.touch) toss_events.touch(self, other, plane, surf);
}

static void h_toss_explode(edict_t *ent)
{
    toss_events.exploded = h_frames + 1 - toss_events.start;
    toss_events.explosion = h_toss_point(ent);
    toss_events.explosion_y = ent->s.origin[1];
    toss_events.think(ent);
}

enum { H_TOSS_GRENADE, H_TOSS_ITEM, H_TOSS_DEBRIS, H_TOSS_HAND_GRENADE };

static edict_t *h_toss_spawn(edict_t *player, int kind, vec3_t start, const vec3_t velocity, int fuse)
{
    edict_t *obj;
    if (kind == H_TOSS_ITEM) {
        obj = Drop_Item(player, FindItem("Shells"));
    } else if (kind == H_TOSS_DEBRIS) {
        obj = G_Spawn();
        obj->movetype = MOVETYPE_BOUNCE;
        obj->solid = SOLID_BBOX;
        obj->clipmask = MASK_SHOT;
        obj->owner = player;
        obj->think = G_FreeEdict;
        obj->nextthink = level.framenum + fuse * h_div;
        VectorSet(obj->avelocity, 300, 300, 300);
    } else {
        vec3_t dir = { 1, 0, 0 };
#ifdef HARNESS_OPENTDM
        float timer = fuse / 10.0f;
#else
        int timer = fuse * h_div;
#endif
        if (kind == H_TOSS_HAND_GRENADE)
            fire_grenade2(player, start, dir, 125, 0, timer, 165, false);
        else
            fire_grenade(player, start, dir, 120, 0, timer, 160);
        obj = h_by_class(kind == H_TOSS_HAND_GRENADE ? "hgrenade" : "grenade", 0);
    }
    if (!obj) imp_error("toss object did not spawn");
    VectorCopy(start, obj->s.origin);
    VectorCopy(start, obj->old_origin);
    VectorCopy(velocity, obj->velocity);
    gi.linkentity(obj);

    memset(&toss_events, 0, sizeof(toss_events));
    toss_events.start = h_frames;
    toss_events.touch = obj->touch;
    obj->touch = h_toss_touch;
    if (obj->think && kind != H_TOSS_ITEM) {
        toss_events.think = obj->think;
        obj->think = h_toss_explode;
    }
    return obj;
}

static void sc_t24_toss_free(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    for (int item = 0; item <= 1; item++) {
        const char *name = item ? "item" : "grenade";
        vec3_t start = { 0, 1000, 10000 }, velocity = { 400, 0, 300 };
        edict_t *obj = h_toss_spawn(player, item, start, velocity, 30);
        float error = 0;
        int wait = h_grid_wait();
        for (int i = 0; i < h_rate; i++) {
            h_frame();
            int step = item ? i / h_div : (i + h_div - wait) / h_div;
            float t = item ? (float)(i % h_div + 1) / h_div : 0;
            toss_point_t a = toss_free_reference[step], b = toss_free_reference[item ? step + 1 : step];
            toss_point_t expected = { a.x + t * (b.x - a.x), a.z + t * (b.z - a.z) };
            toss_point_t actual = { obj->s.origin[0], obj->s.origin[2] - start[2] };
            error = fmaxf(error, h_toss_error(actual, expected));
            error = fmaxf(error, fabsf(obj->s.origin[1] - start[1]));
        }
        float dz = obj->s.origin[2] - start[2], vz = obj->velocity[2];
        CHECK(obj->inuse && !toss_events.count && error <= 0.01f &&
              fabsf(dz + 140) <= 0.01f && fabsf(vz + 500) <= 0.01f,
              "%s free flight: dz %.5f, vz %.5f, worst path error %.5f", name, dz, vz, error);
        G_FreeEdict(obj);
    }
}

static edict_t *h_toss_bounce_scene(void)
{
    h_scene_base();
    h_add_world(300, 1500, 0, 364, 1700, 1000);
    h_add_world(200, 2100, 0, 264, 2300, 1000);
    /* Eight risers down to the floor. */
    h_add_world(-512, 2700, 0, 0, 2900, 128);
    for (int step = 1; step <= 7; step++)
        h_add_world(32 * (step - 1), 2700, 0, 32 * step, 2900, 128 - 16 * step);
    h_start(h_ents);
    return h_join(0, "p1");
}

static edict_t *h_toss_throw(edict_t *player, int scene, int start, int fuse, int kind)
{
    vec3_t origin = { 0, 1000 + 600 * scene, 64 }, velocity = { 600, 0, 200 };
    if (scene == TOSS_CORNER) VectorSet(velocity, 500, 0, 0);
    VectorMA(origin, start / 60.0f, velocity, origin);
    if (scene == TOSS_STAIRS) {
        VectorSet(origin, -16 + 2 * start, 2800, 300);
        VectorSet(velocity, 30 * (start + 1), 0, 0);
    }
    return h_toss_spawn(player, kind, origin, velocity, fuse);
}

static int h_toss_at_rest(const edict_t *obj)
{
    return obj->groundentity && VectorCompare(obj->velocity, vec3_origin);
}

static float h_toss_path_tolerance(void)
{
    return h_rate == 10 ? 0.0001f : 0.01f;
}

static float h_toss_tolerance(void)
{
    return h_rate == 10 ? 0.0001f : 0.1f;
}

static void h_toss_check_hits(const toss_arc_t *ref, int steps, int first_step)
{
    int count = 0;
    while (count < ref->count && ref->hits[count].tick <= steps) count++;
    CHECK(toss_events.count == count, "impact count %d (10 Hz: %d)", toss_events.count, count);
    for (int i = 0; i < count && i < toss_events.count; i++) {
        const toss_hit_t *got = &toss_events.hits[i], *want = &ref->hits[i];
        float error = h_toss_error(got->point, want->point);
        int boundary = want->tick * h_div;
        int on_time = first_step ? got->tick == first_step + boundary - h_div :
                      boundary - h_div < got->tick && got->tick <= boundary;
        CHECK(got->face == want->face && error <= h_toss_tolerance() && on_time,
              "hit %d: face %d/%d, position error %.5f, tick %d (10 Hz: %d)",
              i + 1, got->face, want->face, error, got->tick, boundary);
    }
}

static void h_toss_bounce_case(edict_t *player, int scene, int start, int steps)
{
    int msgs = h_nmsgs, stop = 0;
    float path_error = 0;
    edict_t *obj = h_toss_throw(player, scene, start, steps, H_TOSS_GRENADE);
    float y = obj->s.origin[1];
    int first_step = h_grid_wait() + 1;
    int limit = first_step + steps * h_div;
    const toss_arc_t *ref = &toss_reference[scene][start];
    for (int tick = 1; tick <= limit; tick++) {
        h_frame();
        if (!obj->inuse) break;
        if (!stop && h_toss_at_rest(obj)) stop = tick;
        if (tick < first_step || (tick - first_step) % h_div) continue;
        int node = (tick - first_step) / h_div + 1;
        if (ref->stop && node > ref->stop) node = ref->stop;
        path_error = fmaxf(path_error, h_toss_error(h_toss_point(obj), ref->path[node - 1]));
        path_error = fmaxf(path_error, fabsf(obj->s.origin[1] - y));
    }
    CHECK(!obj->inuse && toss_events.exploded == limit && h_explosions(msgs) == 1,
          "scene %d start %d fuse %.1fs: one explosion at tick %d (expected %d)",
          scene, start, steps / 10.0f, toss_events.exploded, limit);
    int stop_boundary = first_step + (ref->stop - 1) * h_div;
    int node = ref->stop && steps > ref->stop ? ref->stop : steps;
    float explosion_error = h_toss_error(toss_events.explosion, ref->path[node - 1]);
    CHECK(path_error <= h_toss_path_tolerance() && explosion_error <= h_toss_tolerance(),
          "scene %d start %d: worst 100 ms position error %.5f; explosion error %.5f",
          scene, start, path_error, explosion_error);
    CHECK(ref->stop && ref->stop <= steps ? stop == stop_boundary : !stop,
          "stop tick %d (10 Hz: %d, fuse boundary: %d)", stop, stop_boundary, limit);
    h_toss_check_hits(ref, steps, first_step);
    if (obj->inuse) G_FreeEdict(obj);
}

static void sc_t24_toss_bounce(void)
{
    edict_t *player = h_toss_bounce_scene();
    for (int scene = 0; scene < TOSS_SCENES; scene++)
        for (int start = 0; start < TOSS_STARTS; start++) {
            h_toss_bounce_case(player, scene, start, 10);
            h_toss_bounce_case(player, scene, start, 15);
            h_toss_bounce_case(player, scene, start, 25);
            if (scene == TOSS_STAIRS && toss_reference[scene][start].stop > 25)
                h_toss_bounce_case(player, scene, start, 28);
        }
}

#ifndef HARNESS_ENGINE
static void sc_t24_toss_hold(void)
{
    edict_t *player = h_toss_bounce_scene();
    const toss_arc_t *ref = &toss_reference[TOSS_WALL][0];
    edict_t *obj = h_toss_throw(player, TOSS_WALL, 0, 60, H_TOSS_DEBRIS);
    int hit = 0, hold_end = 0, stop = 0, waiting = 0, moved = 0, still = 0;
    float path_error = 0;
    vec3_t impact = { 0 };
    for (int tick = 1; tick <= TOSS_ARC_STEPS * h_div; tick++) {
        h_frame();
        if (tick <= hold_end && toss_events.count != hit)
            moved++;
        if (tick == hold_end + 1 && hold_end)
            still += VectorCompare(obj->s.origin, impact);
        if (toss_events.count > hit && toss_events.count <= TOSS_MAX_HITS) {
            hit = toss_events.count;
            if (!h_toss_at_rest(obj)) {
                toss_point_t point = toss_events.hits[hit - 1].point;
                VectorSet(impact, point.x, toss_events.impact_y, point.z);
                hold_end = ((tick + h_div - 1) / h_div) * h_div;
            }
        }
        if (tick <= hold_end) {
            waiting += tick > toss_events.hits[hit - 1].tick;
            moved += !VectorCompare(obj->s.origin, impact);
        }
        if (!stop && h_toss_at_rest(obj)) stop = tick;
        if (tick % h_div == 0) {
            int node = tick / h_div;
            if (ref->stop && node > ref->stop) node = ref->stop;
            path_error = fmaxf(path_error, h_toss_error(h_toss_point(obj), ref->path[node - 1]));
        }
    }
    CHECK((h_div == 1 || waiting > 0) && !moved && !still,
          "wait at impact until the step ends: %d waiting ticks, %d moved, %d late resumes", waiting, moved, still);
    CHECK(ref->count >= 4 && path_error <= h_toss_path_tolerance(),
          "four or more impacts with aligned 100 ms positions (path error %.5f)", path_error);
    CHECK(stop > (ref->stop - 1) * h_div && stop <= ref->stop * h_div,
          "stop tick %d (10 Hz: %d)", stop, ref->stop * h_div);
    h_toss_check_hits(ref, TOSS_ARC_STEPS, 0);
    G_FreeEdict(obj);

    vec3_t start = { 294, 1600, 200 }, velocity = { 600, 0, 0 };
    obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, velocity, 30);
    h_frames_n(h_div);
    CHECK(toss_events.count == 1 && toss_events.hits[0].tick == 1 && !obj->groundentity,
          "spin setup: one wall impact in the first frame, hit tick %d count %d",
          toss_events.hits[0].tick, toss_events.count);
    CHECK(fabsf(obj->s.angles[0] - 30) <= H_EPS &&
          fabsf(obj->s.angles[1] - 30) <= H_EPS && fabsf(obj->s.angles[2] - 30) <= H_EPS,
          "spin through wall hold: angles %.5f %.5f %.5f (10 Hz: 30 30 30)",
          obj->s.angles[0], obj->s.angles[1], obj->s.angles[2]);
    G_FreeEdict(obj);

    obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, velocity, 1);
    /* At 60 Hz the debris expires before the held step closes. */
    int delay = h_div > 2 ? 2 : h_div;
    const toss_point_t wall_contact = { 299.969025f, 199.204132f };
    obj->nextthink -= h_div - delay;
    h_frames_n(delay + 1);
    CHECK(!obj->inuse && toss_events.exploded == delay + 1 &&
          toss_events.count == 1 && toss_events.hits[0].tick == 1 &&
          h_toss_error(toss_events.explosion, wall_contact) <= h_toss_tolerance() &&
          fabsf(toss_events.explosion_y - start[1]) <= h_toss_tolerance(),
          "expiry during wall hold: hit tick %d, freed at tick %d (expected %d), position %.7f %.7f %.7f",
          toss_events.hits[0].tick, toss_events.exploded, delay + 1,
          toss_events.explosion.x, toss_events.explosion_y, toss_events.explosion.z);
}

static void sc_t24_toss_head(void)
{
    edict_t *player = h_toss_bounce_scene();
    for (int wall = 0; wall <= 1; wall++) {
        edict_t *body = EDICT(game.maxclients + level.body_que + 1);
        player->movetype = MOVETYPE_TOSS;
        respawn(player);
        if (!body->die) imp_error("respawn did not leave a body");
        VectorSet(body->s.origin, wall ? 246 : 0, 1600, 200);
        VectorSet(body->mins, -48, -16, -24);
        VectorSet(body->maxs, 48, 16, 16);
        VectorSet(body->velocity, 600, 0, 0);
        body->groundentity = NULL;
        gi.linkentity(body);
        h_frame();
        CHECK(body->inuse && !body->groundentity &&
              (wall ? body->velocity[0] <= 0 : body->velocity[0] == 600),
              "body setup %s: still airborne, vx %.5f", wall ? "at wall" : "in flight", body->velocity[0]);

        body->health = -41;
        body->die(body, player, player, 100, body->s.origin);
        float z = body->s.origin[2], vz = body->velocity[2];
        CHECK(body->s.effects == EF_GIB && body->movetype == MOVETYPE_BOUNCE,
              "body setup %s: converted to a head", wall ? "at wall" : "in flight");
        h_frame();
        CHECK(fabsf(body->velocity[2] - (vz - 80)) <= H_EPS &&
              fabsf((body->s.origin[2] - z) - (vz - 80) * FRAMETIME) <= H_EPS,
              "head from body %s: first-frame gravity %.5f, dz %.5f (expected %.5f)",
              wall ? "at wall" : "in flight", vz - body->velocity[2], body->s.origin[2] - z, (vz - 80) * FRAMETIME);
    }
}

static void sc_t24_toss_land(void)
{
    h_scene_base();
    for (int phase = 0; phase < h_div; phase++) {
        int model = h_add_inline(300 * phase - 32, 1968, 128, 300 * phase + 32, 2032, 192);
        h_ent("{\n\"classname\" \"func_wall\"\n\"model\" \"*%d\"\n}\n", model);
    }
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    int phases = 0;
    for (int i = 0; i < h_div; i++) {
        /* Shift the launch by one world tick each time. Remove the support as soon as it catches the debris. */
        while (h_frames % h_div != i) h_frame();
        edict_t *wall = h_by_model(va("*%d", i + 1));
        if (!wall) imp_error("toss support did not spawn");
        vec3_t start = { 300 * i, 2000, 196 }, zero = { 0 };
        edict_t *obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, zero, 30);
        for (int tick = 0; tick < h_rate && !obj->groundentity; tick++) h_frame();
        CHECK(obj->groundentity == wall, "launch phase %d: landed on the support", i);
        phases |= 1 << (h_frames % h_div);
        float z = obj->s.origin[2];
        G_FreeEdict(wall);
        h_frame();
        CHECK(!obj->groundentity && obj->s.origin[2] < z, "launch phase %d: fell in the tick after support removal (dz %.6f)",
              i, obj->s.origin[2] - z);
        G_FreeEdict(obj);
    }
    CHECK(phases == (1 << h_div) - 1, "support removal covered all %d world-tick phases", h_div);
}

#endif

static void sc_t24_toss_wall_toggle(void)
{
    h_scene_base();
    int model = h_add_inline(-32, 1968, 128, 32, 2032, 192);
    h_ent("{\n\"classname\" \"func_wall\"\n\"model\" \"*%d\"\n\"spawnflags\" \"7\"\n}\n", model);
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    edict_t *wall = h_by_model("*1");
    if (!wall || !wall->use)
        imp_error("toggleable wall did not spawn");

    for (int phase = 0; phase < h_div; phase++) {
        vec3_t start = { 0, 2000, 196 }, zero = { 0 };
        edict_t *obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, zero, 30);
        for (int tick = 0; tick < h_rate && !obj->groundentity; tick++)
            h_frame();
        CHECK(obj->groundentity == wall, "phase %d: debris rests on the wall", phase);
        while (h_frames % h_div != phase)
            h_frame();

        float z = obj->s.origin[2];
        wall->use(wall, player, player);
        CHECK(wall->inuse && wall->solid == SOLID_NOT, "phase %d: wall switched off without freeing it", phase);
        h_frame();
        CHECK(!obj->groundentity && obj->s.origin[2] < z,
              "phase %d: debris falls on the first tick after wall toggle (height change %.6f)",
              phase, obj->s.origin[2] - z);
        G_FreeEdict(obj);
        wall->use(wall, player, player);
    }
}

#ifdef HARNESS_ENGINE
static void sc_t24_toss_edge(void)
{
    h_scene_base();
    h_add_world(-64, 1936, 128, 64, 2064, 192);
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    vec3_t start = { 0, 2000, 199.99996948242188f }, zero = { 0 };
    edict_t *obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, zero, 30);
    h_frames_n(h_div);
    CHECK(toss_events.count == 1 && toss_events.hits[0].face == 2 && obj->groundentity == EDICT(0) &&
          fabsf(obj->s.origin[2] - 192.03125f) <= H_EPS,
          "fractional edge: contact in first 100 ms, got %d hits at z %.9g (hit tick %d, ground %d)",
          toss_events.count, obj->s.origin[2], toss_events.hits[0].tick, obj->groundentity == EDICT(0));
}

static vec3_t h_outer_corner_normal;
static edict_t *h_outer_corner_hit;

static void h_outer_corner_touch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surface)
{
    if (!toss_events.count && plane) {
        h_outer_corner_hit = other;
        VectorCopy(plane->normal, h_outer_corner_normal);
    }
    h_toss_touch(ent, other, plane, surface);
}

static void sc_t24_toss_outer_corner(void)
{
    h_scene_base();
    h_add_world(2000, 0, 64, 2300, 300, 192);
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    vec3_t start = { 1990, -10, 128 }, velocity = { 200, 199.9998f, 0 };
    edict_t *obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, velocity, 30);
    CHECK(VectorCompare(obj->mins, vec3_origin) && VectorCompare(obj->maxs, vec3_origin),
          "outer corner fixture uses a point debris");
    obj->touch = h_outer_corner_touch;
    h_frames_n(h_div);
    CHECK(toss_events.count == 1 && h_outer_corner_hit == EDICT(0) && h_outer_corner_normal[0] == 0 &&
          h_outer_corner_normal[1] == -1 && h_outer_corner_normal[2] == 0 &&
          fabsf(obj->velocity[0] - 200) <= H_EPS &&
          fabsf(obj->velocity[1] + 100) <= 0.001f && fabsf(obj->velocity[2] + 80) <= H_EPS,
          "outer corner uses the classic Y face: normal (%g, %g, %g), outgoing (%.7g, %.7g, %.7g), %d hit(s)",
          h_outer_corner_normal[0], h_outer_corner_normal[1], h_outer_corner_normal[2],
          obj->velocity[0], obj->velocity[1], obj->velocity[2], toss_events.count);
    G_FreeEdict(obj);

    // At 10 and 20 Hz, the debris reaches the corner on the first tick.
    if (h_div <= 2)
        return;
    obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, velocity, 30);
    obj->touch = h_outer_corner_touch;
    h_frame();
    edict_t *blocker = G_Spawn();
    blocker->solid = SOLID_BBOX;
    float x = (obj->s.origin[0] + 2000) * 0.5f;
    VectorSet(blocker->s.origin, x, x - 2000, 128);
    VectorSet(blocker->mins, -0.5f, -1, -8);
    VectorSet(blocker->maxs, 0.5f, 1, 8);
    gi.linkentity(blocker);
    h_frames_n(h_div - 1);
    CHECK(toss_events.count == 1 && h_outer_corner_hit == blocker && h_outer_corner_normal[0] == -1,
          "a closer entity entering the path wins over the world corner");
    G_FreeEdict(obj);
    G_FreeEdict(blocker);

    obj = h_toss_spawn(player, H_TOSS_DEBRIS, start, velocity, 30);
    h_frame();
    obj->velocity[1] = 0;
    h_frames_n(h_div - 1);
    CHECK(!toss_events.count && obj->s.origin[0] > 2000 && obj->s.origin[1] < 0,
          "changing velocity between ticks lets the debris miss the world corner");
}

static void sc_t24_grid_solid(void)
{
    h_scene_base();
    h_add_world(100, 900, 0, 500, 1100, 256);
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    for (int bugs = 0; bugs <= 2; bugs += 2) {
        gi.cvar_forceset("g_bugs", va("%d", bugs));
        for (int phase = 0; phase < h_div; phase++) {
            h_frames_n((h_grid_wait() + phase) % h_div);
            int wait = h_grid_wait(), msg = h_nmsgs;
            vec3_t start = { 200, 1000, 100 }, dir = { 1, 0, 0 };
            fire_rocket(player, start, dir, 100, 650, 120, 120);
#ifdef HARNESS_OPENTDM
            int creation_hit = bugs < 2;
#else
            int creation_hit = 0;
#endif
            CHECK(h_explosions(msg) == creation_hit, "g_bugs %d phase %d: explosion in creation command", bugs, phase);
            h_frames_n(wait);
            CHECK(h_explosions(msg) == creation_hit, "g_bugs %d phase %d: no explosion while waiting for grid", bugs, phase);
            h_frame();
            CHECK(h_explosions(msg) == (bugs < 2), "g_bugs %d phase %d: explosion by the first grid step", bugs, phase);
            edict_t *rocket = h_by_class("rocket", 0);
            if (rocket) G_FreeEdict(rocket);
        }
    }
}

static void sc_t24_toss_drop(void)
{
    h_scene_base();
    int model = h_add_inline(-64, 1936, 256, 64, 2064, 320);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"256\"\n}\n", model);
    h_add_world(200, 1936, 0, 328, 2064, 64);
    h_start(h_ents);
    edict_t *plat = h_by_model("*1"), *player = h_join(0, "p1");
    if (!plat) imp_error("toss platform did not spawn");
    static const int heights[] = { 4, 8, 16, 24, 28, 36, 48 };
    for (int item = 0; item <= 1; item++)
        for (int platform = 0; platform <= 1; platform++)
            for (int i = 0; i < (int)(sizeof(heights) / sizeof(heights[0])); i++) {
                vec3_t start = { platform ? 8 : 264, 2000, 64 + heights[i] }, zero = { 0 };
                edict_t *obj = h_toss_spawn(player, item ? H_TOSS_ITEM : H_TOSS_DEBRIS, start, zero, 30);
                obj->s.origin[2] -= obj->mins[2];
                gi.linkentity(obj);
                h_frames_n(3 * h_rate / 2);
                int landed = obj->groundentity == (platform ? plat : EDICT(0)) &&
                             fabsf(obj->s.origin[2] + obj->mins[2] - 64) <= 0.1f;
                /* A classic step that ends exactly on a face can already miss it at 10 Hz. */
                if (h_rate == 10 || toss_drop_reference[i])
                    CHECK(landed == toss_drop_reference[i],
                          "%s +%d on %s: landed %d (10 Hz: %d)", item ? "item" : "debris",
                          heights[i], platform ? "platform" : "brush", landed, toss_drop_reference[i]);
                G_FreeEdict(obj);
            }
}
#else

static struct {
    int count, impulses;
    toss_point_t first;
    void (*touch)(edict_t *, edict_t *, cplane_t *, csurface_t *);
} toss_pad;

static void h_toss_pad_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other->movetype == MOVETYPE_BOUNCE || (other->spawnflags & DROPPED_ITEM)) {
        if (!toss_pad.count) toss_pad.first = h_toss_point(other);
        toss_pad.count++;
    }
    vec3_t before;
    VectorCopy(other->velocity, before);
    toss_pad.touch(self, other, plane, surf);
    toss_pad.impulses += !VectorCompare(before, other->velocity);
}

static void sc_t24_toss_pad(void)
{
    h_scene_base();
    static const int boxes[][3] = { { 270, 330, 56 }, { 335, 355, 45 }, { 420, 520, 32 } };
    for (int i = 0; i < (int)(sizeof(boxes) / sizeof(boxes[0])); i++) {
        int y = 1000 + 500 * i;
        int model = h_add_inline(boxes[i][0], y - 100, 0, boxes[i][1], y + 100, boxes[i][2]);
        h_ent("{\n\"classname\" \"trigger_push\"\n\"model\" \"*%d\"\n\"angles\" \"-45 0 0\"\n\"speed\" \"50\"\n}\n", model);
    }
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    static const int cases[][3] = {
        { H_TOSS_GRENADE, 0, 0 }, { H_TOSS_GRENADE, 1, 1 }, { H_TOSS_GRENADE, 2, 2 },
        { H_TOSS_ITEM, 0, 3 }, { H_TOSS_DEBRIS, 1, 1 }, { H_TOSS_HAND_GRENADE, 0, 4 }
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        int kind = cases[i][0], lane = cases[i][1], item = kind == H_TOSS_ITEM;
        edict_t *trigger = h_by_model(va("*%d", lane + 1));
        if (!trigger || !trigger->touch) imp_error("trigger_push did not spawn");
        toss_pad.count = toss_pad.impulses = 0;
        toss_pad.touch = trigger->touch;
        trigger->touch = h_toss_pad_touch;

        vec3_t start = { 0, 1000 + 500 * lane, lane == 1 ? 200 : 64 };
        vec3_t velocity = { 600, 0, lane == 1 ? 0 : 200 };
        edict_t *obj = h_toss_spawn(player, kind, start, velocity, 50);
        if (item) obj->s.origin[2] -= obj->mins[2];
        gi.linkentity(obj);
        for (int tick = 0; tick < 4 * h_rate && !h_toss_at_rest(obj); tick++) h_frame();
        CHECK(h_toss_at_rest(obj), "pad case %d: object came to rest", i);
        const toss_pad_ref_t *ref = &toss_pad_reference[cases[i][2]];
        float entry_error = toss_pad.count ? h_toss_error(toss_pad.first, ref->entry) : 0;
        float stop_error = h_toss_error(h_toss_point(obj), ref->stop);
        CHECK(toss_pad.count == ref->count && entry_error <= h_toss_tolerance() &&
              (item || kind == H_TOSS_HAND_GRENADE || stop_error <= h_toss_tolerance()),
              "pad case %d: %d touches (10 Hz: %d), entry error %.5f, stop error %.5f",
              i, toss_pad.count, ref->count, entry_error, stop_error);
        CHECK(toss_pad.impulses == (kind == H_TOSS_GRENADE ? ref->count : 0),
              "pad case %d: launcher grenade only gets a push (got %d impulses)", i, toss_pad.impulses);
        trigger->touch = toss_pad.touch;
        G_FreeEdict(obj);
    }
}

static void h_grid_use_blaster(edict_t *caller)
{
    edict_t *shooter = caller->enemy;
    shooter->use(shooter, caller, caller);
}

static void sc_t24_grid_mapblaster(void)
{
    h_scene_base();
    h_add_world(70, 936, 0, 120, 1064, 256);
    h_ent("{\n\"classname\" \"target_blaster\"\n\"origin\" \"0 1000 100\"\n\"speed\" \"300\"\n}\n");
    h_start(h_ents);
    edict_t *shooter = h_by_class("target_blaster", 0);
    if (!shooter) imp_error("target_blaster missing");

    edict_t *slot = G_Spawn(), *caller = G_Spawn();
    G_FreeEdict(slot);
    caller->enemy = shooter;
    caller->think = h_grid_use_blaster;
    caller->nextthink = level.framenum + 1;
    h_frames_n(2);
    edict_t *bolt = h_by_class("bolt", 0);
    CHECK(bolt == slot && bolt && bolt->s.origin[0] == 0, "map blaster shot takes a passed slot and has not moved");
    if (!bolt) return;
    CHECK(!!(bolt->svflags & SVF_NOCLIENT) == (h_div > 1), "visibility of a map bolt in its creation frame");
    h_frames_n(h_grid_wait());
    CHECK(bolt->inuse && bolt->s.origin[0] == 0, "map bolt waits for its first grid step");
    h_frame();
    CHECK(bolt->inuse && !(bolt->svflags & SVF_NOCLIENT) && fabsf(bolt->s.origin[0] - 30) < H_EPS,
          "map bolt becomes visible 30 units along its path on the first grid step (x %g)", bolt->s.origin[0]);
    h_frames_n(h_div);
    CHECK(bolt->inuse && fabsf(bolt->s.origin[0] - 60) < H_EPS, "map bolt travels 60 units in two steps");
    h_frames_n(h_div - 1);
    CHECK(bolt->inuse, "map bolt has not touched the wall before the third grid step");
    h_frame();
    CHECK(!bolt->inuse, "map bolt touches the wall on the third grid step");
    CHECK(!bolt->grid_move && !bolt->grid_frame && VectorCompare(bolt->grid_from, vec3_origin) &&
          VectorCompare(bolt->grid_from_angles, vec3_origin), "freeing the projectile clears its grid state");
    edict_t *reuse = G_Spawn();
    CHECK(reuse == bolt && !(reuse->svflags & SVF_NOCLIENT), "a reused projectile slot is visible immediately");
}

static void sc_t24_toss_projectile(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *player = h_join(0, "p1");
    for (int rocket = 0; rocket <= 1; rocket++) {
        for (int phase = 0; phase < h_div; phase++) {
            h_frames_n((h_grid_wait() + phase) % h_div);
            int wait = h_grid_wait();
            vec3_t start, dir = { 1, 0, 0 };
            VectorCopy(player->s.origin, start);
            start[2] += 20;
            if (rocket) fire_rocket(player, start, dir, 100, 650, 120, 120);
            else fire_blaster(player, start, dir, 15, 1000, EF_BLASTER, false);
            edict_t *obj = h_by_class(rocket ? "rocket" : "bolt", 0);
            if (!obj) imp_error("projectile did not spawn");
            float error = 0;
            for (int tick = 0; tick < h_rate; tick++) {
                h_frame();
                vec3_t point, velocity = { toss_projectile_speed[rocket], 0, 0 };
                int step = (tick + h_div - wait) / h_div;
                VectorMA(start, step * 0.1f, velocity, point);
                vec3_t delta;
                VectorSubtract(point, obj->s.origin, delta);
                error = fmaxf(error, VectorLength(delta));
                VectorSubtract(velocity, obj->velocity, delta);
                error = fmaxf(error, VectorLength(delta));
            }
            CHECK(obj->inuse && error <= 0.01f, "%s phase %d: worst grid position or velocity error %.5f",
                  rocket ? "rocket" : "blaster", phase, error);
            G_FreeEdict(obj);
        }
    }
}

#ifdef HARNESS_OPENTDM
static void h_toss_timeout_run(edict_t *player, int kind, int pause, toss_arc_t *arc)
{
    if (kind == H_TOSS_GRENADE) h_real_phase(0);
    vec3_t start = { 0, 1000, 64 }, velocity = { 600, 0, 200 };
    edict_t *obj = h_toss_spawn(player, kind, start, velocity, 60);
    int freeze = -1;
    for (int tick = 1; tick <= TOSS_ARC_STEPS * h_div; tick++) {
        if (pause && tick == h_div + 2) {
            h_client_command(player, "time");
            CHECK(tdm_match_status == MM_TIMEOUT, "entered timeout mid-flight");
            freeze = (level.realframenum + h_div - 1) / h_div * h_div;
        }
        if (level.realframenum == freeze) {
            h_frame();
            int match_frame = level.framenum;
            vec3_t origin, speed;
            VectorCopy(obj->s.origin, origin);
            VectorCopy(obj->velocity, speed);
            int budget = 15 * h_rate, frozen = 1;
            for (int i = 1; i < budget && tdm_match_status == MM_TIMEOUT; i++) {
                if (i == h_rate) h_client_command(player, "time");
                h_frame();
                if (tdm_match_status == MM_TIMEOUT)
                    frozen &= obj->inuse && VectorCompare(obj->s.origin, origin) &&
                              VectorCompare(obj->velocity, speed) && level.framenum == match_frame;
            }
            CHECK(frozen && tdm_match_status == MM_PLAYING, "trajectory and match clock stayed frozen until resume");
            h_frames_n(h_div - 1);
            int paused = h_frames - toss_events.start - (tick - 1);
            toss_events.start += paused;
        }
        h_frame();
        if (tick % h_div == 0) arc->path[tick / h_div - 1] = h_toss_point(obj);
        if (!arc->stop && h_toss_at_rest(obj)) arc->stop = tick;
    }
    CHECK(obj->inuse && !toss_events.exploded, "object is live throughout the timeout arc");
    arc->count = toss_events.count;
    memcpy(arc->hits, toss_events.hits, sizeof(arc->hits));
    G_FreeEdict(obj);
}

static void h_check_timeout_arc(edict_t *player, int kind, const char *name)
{
    toss_arc_t continuous = { 0 }, paused = { 0 };
    h_toss_timeout_run(player, kind, 0, &continuous);
    h_toss_timeout_run(player, kind, 1, &paused);
    float error = 0;
    for (int i = 0; i < TOSS_ARC_STEPS; i++)
        error = fmaxf(error, h_toss_error(continuous.path[i], paused.path[i]));
    int same = continuous.count > 0 && continuous.count == paused.count && continuous.count <= TOSS_MAX_HITS;
    for (int i = 0; same && i < continuous.count; i++) {
        const toss_hit_t *before = &continuous.hits[i], *after = &paused.hits[i];
        same = before->tick == after->tick && before->face == after->face &&
               h_toss_error(before->point, after->point) <= H_EPS;
    }
    CHECK(same && continuous.stop && continuous.stop == paused.stop && error <= H_EPS,
          "%s follows the same arc, impacts and stop as uninterrupted play (path error %.5f)", name, error);
}

static void sc_t24_toss_timeout(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    h_check_timeout_arc(a, H_TOSS_DEBRIS, "debris");
    h_check_timeout_arc(a, H_TOSS_GRENADE, "grenade");
}
#endif
#endif

/* ---------------------------------------------------------------- T23 */

#ifdef HARNESS_OPENTDM
/* OpenTDM's engine applies userinfo separately from ClientConnect. */
static void h_t23_userinfo(edict_t *p, const char *name, unsigned code)
{
    char userinfo[MAX_INFO_STRING];
    snprintf(userinfo, sizeof(userinfo), "\\name\\%s\\skin\\male/grunt\\fov\\90\\joincode\\%u",
             name, code);
    ge->ClientUserinfoChanged(p, userinfo);
}

static int h_t23_announcements(const char *text, int from, int *first)
{
    int n = 0;
    *first = -1;
    for (int i = from; i < h_nbprints; i++) {
        if (!strstr(h_bprints[i].text, text)) continue;
        if (!n++) *first = h_bprints[i].frame;
    }
    return n;
}

static void h_t23_announcement(const char *event, const char *text, int from, int start, int ticks)
{
    int when, n = h_t23_announcements(text, from, &when);
    CHECK(n == 1 && when - start == ticks, "%s: one announcement at %d ticks (got %d at %d)",
          event, ticks, n, when - start);
    h_t23_sample(event, when - start, n);
}

static void h_t23_countdown(edict_t *a, edict_t *b)
{
    int bp = h_nbprints, start = h_frames;
    h_client_command(a, "ready");
    h_frames_n(40 * h_rate + 1);
    int nags = 0;
    for (int i = bp; i < h_nbprints; i++) {
        if (!strstr(h_bprints[i].text, "Waiting on p2")) continue;
        int elapsed = h_bprints[i].frame - start;
        nags++;
        CHECK(elapsed == nags * 20 * h_rate, "ready reminder %d after %d ticks", nags, elapsed);
        h_t23_sample(va("ready_nag_%d", nags), elapsed, 1);
    }
    CHECK(nags == 2, "ready: two 20 s reminders (got %d)", nags);

    int sounds = h_nsounds;
    bp = h_nbprints;
    start = h_frames;
    int countdown = (h_div - h_frames % h_div) % h_div + 15 * h_rate;
    h_client_command(b, "ready");
    h_t23_announcement("countdown", "Starting countdown", bp, start, 0);
    CHECK(tdm_match_status == MM_COUNTDOWN, "match enters countdown");
    h_frames_n(countdown + 1);
    h_t23_announcement("fight", "Fight!", bp, start, countdown);
    int idx = imp_soundindex("world/10_0.wav");
    int n = h_count_sounds(idx, sounds), at = -1;
    for (int i = sounds; i < h_nsounds; i++)
        if (h_sounds_log[i].soundindex == idx) at = h_sounds_log[i].frame;
    CHECK(n == 1 && start + countdown - at == 104 * h_rate / 10,
          "countdown sound 10.4 s before Fight! (got %d sounds at tick %d)", n, at - start);
    h_t23_sample("countdown_sound", at - start, n);
    n = h_announced(bp, CS_TDM_TIMELIMIT_STRING, "countdown");
    CHECK(n == 5, "countdown has five HUD announcements");
    h_t23_sample("countdown_numbers", 0, n);
}

static void sc_t23_match_phases(void)
{
    h_set_override("g_match_time", "12");
    h_set_override("g_overtime", "4");
    h_set_override("g_force_screenshot", "1");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    h_t23_userinfo(a, "p1", 0);
    h_t23_userinfo(b, "p2", 0);
    h_t23_countdown(a, b);
    CHECK(tdm_match_status == MM_PLAYING, "Fight! enters regulation");
    if (tdm_match_status != MM_PLAYING) return;

    int start = h_frames - 1, bp = h_nbprints;
    h_frames_n(2 * h_rate);
    CHECK(!strcmp(h_timer_at(h_frames - 1), " 0:10"), "regulation HUD has ten seconds left");
    CHECK(!strcmp(gi.cvar("time_remaining", "", 0)->string, "0:10"), "serverinfo advertises ten seconds left");
    h_t23_sample("regulation_hud", h_frames - start - 1, atoi(h_timer_at(h_frames - 1) + 3));
    h_frames_n(10 * h_rate);
    h_t23_announcement("overtime", "adding", bp, start, 12 * h_rate);
    CHECK(tdm_match_status == MM_OVERTIME,
          "tied regulation advances to timed overtime");

    /* On a tied overtime boundary the sudden-death rule has no clock. */
    gi.cvar_forceset("g_tie_mode", "2");
    h_frames_n(4 * h_rate);
    h_t23_announcement("sudden_death", "entering Sudden Death", bp, start, 16 * h_rate);
    h_frames_n(2 * h_rate);
    CHECK(tdm_match_status == MM_SUDDEN_DEATH, "tie persists past the old deadline");
    vec3_t dir = { 0, 0, 0 };
    T_Damage(b, a, a, dir, b->s.origin, dir, 200, 0, DAMAGE_NO_PROTECTION, MOD_RAILGUN);
    int tx = h_ntexts;
    h_frame();
    CHECK(tdm_match_status == MM_SCOREBOARD,
          "first sudden-death frag ends the match");
    int end = h_frames - 1;
    h_t23_sample("finished", end - start, tdm_match_status);
    h_frames_n(5 * h_rate - 1);
    CHECK(tdm_match_status == MM_SCOREBOARD, "scoreboard still open one step before five seconds");
    h_frame();
    CHECK(tdm_match_status == MM_WARMUP,
          "scoreboard ends in warmup");
    h_t23_sample("warmup", h_frames - end - 1, tdm_match_status);
    for (int slot = 0; slot < 2; slot++) {
        edict_t *p = slot ? b : a;
        const htext_t *shot = h_text_to(p, H_SVC_STUFFTEXT, "screenshot", tx);
        CHECK(shot && shot->frame - end == h_rate / 2, "p%d screenshot 0.5 s after match end", slot + 1);
        h_t23_sample(slot ? "screenshot_b" : "screenshot_a", shot ? shot->frame - end : -1, shot != NULL);
    }
}

static void h_t23_wait_resume(void)
{
    for (int i = 0; i < 11 * h_rate && tdm_match_status == MM_TIMEOUT; i++) h_frame();
    CHECK(tdm_match_status == MM_PLAYING, "timeout resumed play");
}

static int h_t23_players_seconds(edict_t *viewer, int slot)
{
    int tx = h_ntexts;
    h_client_command(viewer, "players");
    const htext_t *list = h_text_to(viewer, 0, "id    time", tx);
    for (const char *line = list ? list->text : NULL; line; ) {
        int id, min, sec;
        if (sscanf(line, "%d %d:%d", &id, &min, &sec) == 3 && id == slot) return min * 60 + sec;
        const char *next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
    }
    return -1;
}

static int h_t23_pause_clock(int frames)
{
    CHECK(!strcmp(h_timer_at(h_frames - 1), h_hud_text(frames)), "match HUD is rounded up (got %s)",
          h_timer_at(h_frames - 1));
    return tdm_match_status == MM_TIMEOUT;
}

static void sc_t23_timeout_clocks(void)
{
    h_set_override("g_match_time", "180");
    h_set_override("g_max_timeout", "62");
    h_set_override("g_timeout_limit", "1");
    h_set_override("g_1v1_timeout", "15");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    int age = level.framenum - a->client->resp.enterframe;
    h_frames_n(205 * h_rate / 10 - age);
    int left = level.match_end_framenum - level.framenum;
    int before = h_t23_players_seconds(a, 0);
    CHECK(before >= 0, "players includes the caller");
    CHECK(!h_t23_pause_clock(left), "clock runs before time");
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "time starts a pause");
    int start = h_frames, bp = h_nbprints;
    h_frames_n(2 * h_rate + 1);
    h_t23_announcement("timeout_minute", "Automatically resuming in 1 minute", bp, start, 2 * h_rate);
    int texts = h_ntexts;
    edict_t *viewer = h_connect(2, "viewer");
    CHECK(h_text_to(viewer, 0, "Match is currently paused and will auto resume in 0:59.", texts),
          "new spectator gets the real-clock seconds left in the pause");
    CHECK(h_t23_pause_clock(left) && h_t23_players_seconds(viewer, 0) == before,
          "new spectator sees the frozen match clock and players time");
    h_t23_sample("paused_players", h_frames - start, before);
    h_frames_n(60 * h_rate);
    h_t23_announcement("automatic_resume", "Game resuming", bp, start, 62 * h_rate);
    h_t23_wait_resume();
    h_t23_announcement("automatic_fight", "Fight!", bp, start, 724 * h_rate / 10);
    h_frames_n(h_rate);
    int after = h_t23_players_seconds(a, 0);
    CHECK(!h_t23_pause_clock(level.match_end_framenum - level.framenum) && after == before + 1,
          "match clock and players time advance after resume");
    h_client_command(a, "time");
    CHECK(h_text_to(a, 0, "Timeout limit exceeded (1/1)", 0) && tdm_match_status == MM_PLAYING,
          "caller has spent their one timeout");

    h_client_command(b, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "other player can call time");
    h_frames_n(h_rate);
    start = h_frames;
    bp = h_nbprints;
    int resume_pad = (h_div - level.realframenum % h_div) % h_div;
    h_view_resume(b);
    h_t23_announcement("manual_fight", "Fight!", bp, start, resume_pad + 104 * h_rate / 10);

    unsigned code = a->client->resp.teamplayerinfo->joincode;
    h_place(b, 800, 0, 24);
    h_tick(b, 0, 1);
    before = h_t23_players_seconds(b, 0);
    ge->ClientDisconnect(a);
    CHECK(tdm_match_status == MM_TIMEOUT, "duelist disconnect starts a pause");
    h_frames_n(h_rate);
    start = h_frames;
    bp = h_nbprints;
    resume_pad = (h_div - level.realframenum % h_div) % h_div;
    char userinfo[MAX_INFO_STRING] = "\\name\\p1\\skin\\male/grunt";
    CHECK(ge->ClientConnect(a, userinfo), "returning duelist connects");
    h_t23_userinfo(a, "p1", code);
    ge->ClientBegin(a);
    int recovered = h_t23_players_seconds(b, 0);
    CHECK(a->client->pers.team == TEAM_A && recovered == before,
          "join code restores player and players time (%d -> %d)", before, recovered);
    h_t23_wait_resume();
    h_t23_announcement("duel_reconnect", "Fight!", bp, start, resume_pad + 104 * h_rate / 10);
    h_t23_sample("recovered_players", 0, recovered);
    start = h_frames;
    bp = h_nbprints;
    ge->ClientDisconnect(a);
    CHECK(tdm_match_status == MM_TIMEOUT, "second duelist disconnect also pauses");
    h_frames_n(15 * h_rate);
    int announced;
    CHECK(!h_t23_announcements("Game resuming", bp, &announced), "duel still paused one step before 15 seconds");
    h_frame();
    h_t23_announcement("duel_expiry", "Game resuming", bp, start, 15 * h_rate);
}

static int h_t23_score_ping(const htext_t *layout, const char *name)
{
    if (!layout) return -1;
    const char *prefix = va("\"%s ", name);
    for (const char *row = layout->text; (row = strstr(row, prefix)); row += strlen(prefix)) {
        int frags, deaths, net, ping;
        if (sscanf(row + strlen(prefix), "%d %d %d %d", &frags, &deaths, &net, &ping) == 4)
            return ping;
    }
    return -1;
}

static const htext_t *h_t23_next_layout(edict_t *p, int since, int limit)
{
    for (int n = 0; n <= limit; n++) {
        const htext_t *layout = h_text_to(p, H_SVC_LAYOUT, "Frags Dths Net Ping", since);
        if (layout) return layout;
        if (n < limit) h_frame();
    }
    return NULL;
}

static void sc_t23_scoreboard_handicap(void)
{
    h_set_override("g_ping_handicap", "50");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    h_t23_userinfo(a, "p1", 0);
    h_t23_userinfo(b, "p2", 0);
    edict_t *s = h_connect(2, "watcher");
    h_t23_userinfo(s, "watcher", 0);
    if (!h_tdm_start(a, b)) return;
    h_frames_n(2 * h_rate);
    a->client->ping = 12;
    b->client->ping = 77;
    s->client->ping = 81;
    int tx = h_ntexts;
    if (s->client->pers.menu.active) h_client_command(s, "score");
    h_client_command(s, "score");
    const htext_t *layout = h_t23_next_layout(s, tx, 0);
    CHECK(layout, "score sends match layout");
    int start = -1, prior = -1;
    for (int i = 0; i < 4; i++) {
        tx = h_ntexts;
        layout = h_t23_next_layout(s, tx, 3 * h_rate);
        CHECK(layout, "live scoreboard refresh %d", i + 1);
        if (!layout) return;
        if (start < 0) start = layout->frame;
        CHECK(prior < 0 || layout->frame - prior == 3 * h_rate, "live layout interval is 3 s");
        prior = layout->frame;
        int tick = (level.framenum - 1) / h_div;
        int want = tick % 8 == 0 ? 53 : tick % 4 == 0 ? 48 : 50;
        int got = h_t23_score_ping(layout, "p1");
        CHECK(got == want && h_t23_score_ping(layout, "p2") == 77,
              "match layout ping at tick %d: p1 %d (want %d), p2 unchanged", tick, got, want);
        CHECK(strstr(layout->text, va(":%d(", want)) && strstr(layout->text, ":77(") &&
              strstr(layout->text, "watcher:81"), "team averages and spectator row have expected ping");
        h_t23_sample(va("active_%d", i), layout->frame - start, got);
    }

    int phase = level.framenum % (8 * h_div);
    h_frames_n((8 * h_div - phase) % (8 * h_div));
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "scoreboard stays open in timeout");
    int frozen = level.framenum / h_div;
    int want = frozen % 8 == 0 ? 53 : frozen % 4 == 0 ? 48 : 50;
    prior = -1;
    for (int i = 0; i < 3; i++) {
        tx = h_ntexts;
        layout = h_t23_next_layout(s, tx, 15 * h_rate / 10);
        CHECK(layout, "paused scoreboard refresh %d", i + 1);
        if (!layout) return;
        if (i == 0) start = layout->frame;
        CHECK(prior < 0 || layout->frame - prior == 15 * h_rate / 10, "timeout layout interval is 1.5 s");
        prior = layout->frame;
        int got = h_t23_score_ping(layout, "p1");
        CHECK(got == want, "scoreboard ping is frozen in timeout at %d (got %d)", want, got);
        h_t23_sample(va("paused_%d", i), layout->frame - start, got);
    }
    h_view_resume(a);
    int end = h_frames;
    int jitter = level.framenum / h_div;
    int saved = jitter % 8 == 0 ? 53 : jitter % 4 == 0 ? 48 : 50;
    TDM_EndMatch();
    h_frames_n(5 * h_rate + 1);
    a->client->ping = 111;
    b->client->ping = 122;
    tx = h_ntexts;
    h_client_command(s, "oldscores");
    layout = h_text_to(s, H_SVC_LAYOUT, "oldscoreboard: harness", tx);
    CHECK(layout && h_t23_score_ping(layout, "p1") == saved && h_t23_score_ping(layout, "p2") == 77,
          "oldscores retains both original pings after live pings change (p1 %d, want %d)",
          h_t23_score_ping(layout, "p1"), saved);
    h_t23_sample("oldscore", h_frames - end, h_t23_score_ping(layout, "p2"));
}

static int h_t23_limited(edict_t *p, const char *cmd)
{
    int tx = h_ntexts;
    h_client_command(p, cmd);
    return h_text_to(p, 0, "rate limiting", tx) != NULL;
}

static void h_t23_penalty(edict_t *p, const char *first, const char *repeat, int ticks, const char *event)
{
    int start = h_frames;
    CHECK(!h_t23_limited(p, first), "%s: initial '%s' accepted", event, first);
    int same = h_t23_limited(p, repeat);
    CHECK(same, "%s: same-frame '%s' rate limited", event, repeat);
    h_frames_n(ticks - 1);
    int prior = h_t23_limited(p, repeat);
    CHECK(prior, "%s: '%s' rate limited one tick before %d", event, repeat, ticks);
    h_frame();
    int limited = h_t23_limited(p, repeat);
    CHECK(!limited, "%s: '%s' accepted at %d ticks without input", event, repeat, ticks);
    h_t23_sample(event, h_frames - start, same + prior + !limited);
}

static void h_t23_vote_penalties(edict_t *a, edict_t *b)
{
    static const struct { const char *command, *event; unsigned flags; } cases[] = {
        { "vote timelimit 4", "vote_time", VOTE_TIMELIMIT },
        { "vote map fixture_map", "vote_map", VOTE_MAP },
        { "vote weapons -rg", "vote_weapons", VOTE_WEAPONS },
        { "vote kick 2", "vote_kick", VOTE_KICK },
        { "vote powerups -quad", "vote_powerups", VOTE_POWERUPS },
        { "vote gamemode duel", "vote_mode", VOTE_GAMEMODE },
        { "vote tiemode sd", "vote_tie", VOTE_TIEMODE },
        { "vote telemode nofreeze", "vote_tele", VOTE_TELEMODE },
        { "vote switchmode fast", "vote_switch", VOTE_SWITCHMODE },
        { "vote overtime 2", "vote_overtime", VOTE_OVERTIME },
        { "vote chat players", "vote_chat", VOTE_CHAT },
        { "vote bugs 1", "vote_bugs", VOTE_BUGS },
        { "vote smartmap 1", "vote_smartmap", VOTE_MAP },
    };
    smartmap_t *duel_maps = &game.smartmaps[SM_DUEL];
    duel_maps->index = 0;
    duel_maps->total = 4;
    for (int i = 0; i < duel_maps->total; i++) duel_maps->maps[i] = "fixture_smartmap";
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        h_client_command(a, "vote timelimit 2");
        CHECK(vote.active && vote.flags == VOTE_TIMELIMIT, "replacement starts with an active vote");
        edict_t *spectator = EDICT(3);
        spectator->client->resp.last_invited_by = NULL;
        h_t23_penalty(a, cases[i].command, "invite 2", 2 * h_rate, cases[i].event);
        CHECK(spectator->client->resp.last_invited_by == a, "%s releases the invite at the deadline", cases[i].event);
        CHECK(vote.active && vote.flags == cases[i].flags && vote.initiator == a,
              "%s reaches its replacement proposal", cases[i].event);
        if (cases[i].flags == VOTE_TIMELIMIT)
            CHECK(vote.newtimelimit == 4, "timelimit replacement changes the proposed value");
        h_client_command(a, "vote cancel");
        h_frames_n(10 * h_rate);
    }
    h_client_command(a, "vote timelimit 2");
    h_t23_penalty(b, "vote no", "vote yes", 2 * h_rate, "vote_yes_no");
    CHECK(!vote.active && g_match_time->value == 120, "yes vote passes when its cooldown ends");
    h_frames_n(2 * h_rate);
}

static void sc_t23_cmd_penalties(void)
{
    h_set_override("g_admin_password", HARNESS_MOD);
    h_set_override("g_tdm_allow_pick", "1");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    edict_t *s = h_connect(2, "watcher");
    h_t23_penalty(s, "team a", "team spec", 2 * h_rate, "team");
    CHECK(s->client->pers.team == TEAM_SPEC, "team penalty sequence ends in spectator");
    h_t23_penalty(a, "invite 2", "invite 2", 2 * h_rate, "invite");
    CHECK(s->client->resp.last_invited_by == a, "invite path reached spectator");
    h_frames_n(2 * h_rate);
    edict_t *other = h_connect(3, "other");
    h_place(a, -800, 0, 24);
    h_tick(a, 0, 1);
    h_place(b, 800, 0, 24);
    h_tick(b, 0, 1);
    h_t23_penalty(a, "pick 2", "pick 3", h_rate, "pick");
    CHECK(s->client->pers.team == TEAM_A && other->client->pers.team == TEAM_A,
          "both pick targets actually join the team");
    h_client_command(s, "team spec");
    h_client_command(other, "team spec");
    h_frames_n(h_rate);
    h_t23_penalty(a, "ready", "notready", h_rate, "ready");
    CHECK(!a->client->resp.ready, "notready accepted");
    h_frames_n(h_rate);
    h_client_command(a, "ready");
    h_frames_n(h_rate);
    h_t23_penalty(a, "notready", "ready", h_rate, "notready");
    CHECK(a->client->resp.ready, "ready accepted");
    h_t23_penalty(s, "admin invalid", "admin invalid", h_rate, "admin");
    CHECK(h_text_to(s, 0, "Invalid password", 0), "admin path reached credential check");
    h_t23_vote_penalties(a, b);
    if (!h_tdm_start(a, b)) return;
    unsigned code = a->client->resp.teamplayerinfo->joincode;
    int tx = h_ntexts;
    h_t23_penalty(s, "ghost 12345", "ghost 12345", h_rate / 5, "ghost");
    CHECK(h_text_to(s, 0, "No client information found", tx), "absent join code performs lookup");
    h_frames_n(h_rate);
    h_place(b, 800, 0, 24);
    h_tick(b, 0, 1);
    ge->ClientDisconnect(a);
    CHECK(tdm_match_status == MM_TIMEOUT, "valid join-code fixture is paused");
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "ghost %u", code);
    h_t23_penalty(s, "ghost 12345", cmd, h_rate / 5, "ghost_paused");
    CHECK(s->client->pers.team == TEAM_A, "valid join code restores the disconnected player");
}

static void sc_t23_vote_window(void)
{
    h_set_override("g_match_time", "90");
    h_set_override("g_vote_time", "4");
    h_set_override("g_tie_mode", "1");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    h_client_command(a, "vote overtime 3");
    h_client_command(b, "vote yes");
    CHECK(g_overtime->value == 180, "overtime vote converts minutes to seconds");
    h_t23_sample("overtime_seconds", 0, (int)g_overtime->value);
    int tx = h_ntexts;
    OpenVoteMenu(a);
    CHECK(h_text_to(a, H_SVC_LAYOUT, "Overtime:  3 minutes", tx) &&
          h_text_to(a, H_SVC_LAYOUT, "Timelimit: 1 minutes", tx),
          "warmup vote menu converts match time and overtime to minutes");
    PMenu_Close(a);
    h_frames_n(2 * h_rate);
    if (!h_tdm_start(a, b)) return;
    int start = h_frames, prints = h_nbprints;
    h_client_command(a, "vote timelimit 2");
    h_frame();
    CHECK(strstr(h_cs_at(CS_TDM_VOTE_STRING, start), "Yes: 1 (2) No: 0 [04]"),
          "vote HUD starts at 4 s with the initiator's yes");
    h_client_command(b, "vote no");
    h_frames_n(h_rate);
    CHECK(strstr(h_cs_at(CS_TDM_VOTE_STRING, h_frames - 1), "Yes: 1 (2) No: 1 [03]"),
          "vote HUD shows a tied ballot after one second");

    int freeze = h_frames + (h_div - level.realframenum % h_div) % h_div;
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "vote has an active timeout");
    h_frames_n(5 * h_rate);
    CHECK(vote.active && strstr(h_cs_at(CS_TDM_VOTE_STRING, h_frames - 1), "[03]"),
          "vote HUD and expiry freeze in timeout");
    int resume = h_frames + (h_div - level.realframenum % h_div) % h_div + 104 * h_rate / 10;
    h_view_resume(a);
    int frozen = resume - freeze;
    h_frames_n(3 * h_rate);
    h_t23_announcement("expired", "Vote failed.", prints, start, 4 * h_rate + frozen);
    CHECK(!vote.active && !*h_cs_at(CS_TDM_VOTE_STRING, h_frames - 1), "vote HUD clears at expiry");

    h_frames_n(start + 14 * h_rate + frozen - 1 - h_frames);
    tx = h_ntexts;
    h_client_command(a, "vote timelimit 2");
    CHECK(!vote.active && h_text_to(a, 0, "wait a short time", tx), "re-proposal is rejected before 10 s");
    h_frame();
    int at = h_frames;
    h_client_command(a, "vote timelimit 2");
    CHECK(vote.active, "re-proposal opens on the first allowed match tick");
    h_t23_sample("propose_again", at - start, vote.active);
    h_client_command(b, "vote yes");
    CHECK(!vote.active && g_match_time->value == 120 &&
          level.match_end_framenum - level.match_start_framenum == 120 * h_rate,
          "passing a vote extends the current deadline to two minutes");
    tx = h_ntexts;
    OpenVoteMenu(a);
    CHECK(h_text_to(a, H_SVC_LAYOUT, "Timelimit: 2", tx), "in-match vote menu shows two minutes");
    PMenu_Close(a);

    a->client->pers.admin = true;
    h_client_command(a, "tl 1");
    a->client->pers.admin = false;
    int deadline = level.match_start_framenum + 60 * h_rate;
    tx = h_ntexts;
    h_client_command(a, "tl");
    CHECK(level.match_end_framenum == deadline && h_text_to(a, 0, "Timelimit is 1 minute.", tx),
          "admin tl sets a one-minute deadline and the readback displays minutes");
    h_t23_sample("admin_deadline", level.match_end_framenum - level.match_start_framenum, (int)g_match_time->value);
    h_frames_n(deadline - level.framenum);
    CHECK(tdm_match_status == MM_PLAYING, "regulation is active one step before overtime");
    h_frame();
    CHECK(tdm_match_status == MM_OVERTIME && level.match_end_framenum == deadline + 180 * h_rate,
          "tie opens three minutes of overtime at one minute");
    h_t23_sample("overtime_start", level.framenum - 1 - level.match_start_framenum, tdm_match_status);
    h_client_command(a, "vote timelimit 2");
    h_client_command(b, "vote yes");
    CHECK(!vote.active && tdm_match_status == MM_PLAYING && level.match_end_framenum == deadline + 60 * h_rate,
          "timelimit vote cancels overtime and restores regulation");
    h_t23_sample("overtime_canceled", level.match_end_framenum - level.match_start_framenum, tdm_match_status);
    CHECK(gi.cvar_set("g_tie_mode", "0")->value == 0, "fixture disables overtime");
    h_frames_n(15 * h_rate);
    h_client_command(a, "vote timelimit 1");
    h_client_command(b, "vote yes");
    h_frames_n((h_div - h_frames % h_div) % h_div);
    CHECK(tdm_match_status == MM_PLAYING, "shortened timelimit leaves play active before the grid frame");
    h_frame();
    CHECK(g_match_time->value == 60 && tdm_match_status == MM_SCOREBOARD,
          "shortened timelimit that has already elapsed ends the match");
    h_t23_sample("shortened_match", 0, tdm_match_status);
}

#endif

/* Shared scenarios enter a live OpenTDM match or the OpenFFA active game. */
static int h_t23_start(edict_t *a, edict_t *b)
{
#ifdef HARNESS_OPENTDM
    return h_tdm_start(a, b);
#else
    CHECK(PlayerSpawned(a) && PlayerSpawned(b), "both players spawned");
    return PlayerSpawned(a) && PlayerSpawned(b);
#endif
}

static void h_t23_seed(unsigned seed)
{
#ifdef HARNESS_OPENTDM
    init_genrand(seed);
#else
    Q_srand(seed);
#endif
}

static int h_t23_print(edict_t *p, const char *cmd, const char *result)
{
    int tx = h_ntexts;
    h_client_command(p, cmd);
    return h_text_to(p, 0, result, tx) != NULL;
}

static void sc_t23_flood(void)
{
    h_set_override("flood_msgs", "2");
    h_set_override("flood_waitdelay", "4");
    h_set_override("flood_waves", "2");
#ifdef HARNESS_OPENTDM
    h_set_override("flood_waves_waitdelay", "1");
    const char *wave_lock = "can't use waves for 1 minutes";
#else
    h_set_override("flood_perwave", "6");
    h_set_override("flood_persecond", "4");
    h_set_override("flood_wavedelay", "60");
    const char *wave_lock = "can't use waves for 60 seconds";
#endif
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    h_frames_n(3 * h_rate);
    int waves = 0;
    for (int i = 0; i < 2; i++) {
        waves += h_t23_print(a, "wave 1", "salute");
        h_frames_n(2 * h_rate);
    }
    CHECK(waves == 2 && h_t23_print(a, "wave 1", wave_lock), "third wave starts a one minute lock");
#ifdef HARNESS_OPENTDM
    int wave_deadline = level.framenum + 60 * h_rate;
#else
    int wave_deadline = h_frames + 60 * h_rate;
#endif
    edict_t *viewer = h_connect(2, "reader");
    CHECK(h_t23_print(a, "say window-one", ": window-one") &&
          h_t23_print(a, "say window-two", ": window-two"), "two early messages fill the window");
    CHECK(h_t23_print(b, "say expired-message", ": expired-message"), "first message enters the flood window");
    h_frames_n(4 * h_rate - 1);
    CHECK(h_t23_print(a, "say window-before", "can't talk for 4 seconds"),
          "both early messages count until one step before the window closes");
    h_frame();
    int chat = h_t23_print(b, "say first", ": first");
    int texts = h_ntexts;
    chat += h_t23_print(b, "say second", ": second");
    CHECK(chat == 2 && h_text_to(viewer, 0, ": second", texts), "older message leaves the window exactly at four seconds");
    texts = h_ntexts;
    CHECK(h_t23_print(b, "say third", "can't talk for 4 seconds") && !h_text_to(viewer, 0, ": third", texts),
          "third message in the current window starts a four second lock and is not delivered");
    int start = h_frames;
#ifdef HARNESS_OPENTDM
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "both flood locks started before pause");
#endif
    h_frames_n(3 * h_rate);
    CHECK(h_t23_print(b, "say in-pause", "can't talk for 1 more second"), "one second remains on the chat lock");
    h_frames_n(h_rate - 1);
    CHECK(h_t23_print(b, "say early", "can't talk"), "chat blocked one step before deadline");
    h_frame();
    texts = h_ntexts;
    int accepted = h_t23_print(b, "say allowed", ": allowed");
    CHECK(accepted && h_text_to(viewer, 0, ": allowed", texts), "chat delivered on the deadline");
    h_t23_sample("chat_unlocked", h_frames - start, accepted);

#ifdef HARNESS_OPENTDM
    h_view_resume(a);
    CHECK(h_t23_print(b, "say after-resume", ": after-resume"), "chat remains unlocked after resume");
    h_frames_n(wave_deadline - 1 - level.framenum);
#else
    h_frames_n(wave_deadline - 1 - h_frames);
#endif
    CHECK(!h_t23_print(a, "wave 1", "salute"), "wave blocked one step before deadline");
    h_frame();
    accepted = h_t23_print(a, "wave 1", "salute");
    CHECK(accepted, "wave allowed on the first eligible step");
    h_t23_sample("wave_unlocked", h_frames - start, accepted);
}

#ifdef HARNESS_OPENTDM
static void sc_t23_mute(void)
{
    h_set_override("flood_msgs", "0");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    edict_t *admin = h_connect(2, "moderator");
    admin->client->pers.admin = true;
    CHECK(h_t23_print(b, "say before", ": before"), "global chat enabled before mute");
    CHECK(h_t23_print(admin, "mute 1", "muted for 5 minutes"), "default mute is five minutes");
    CHECK(h_t23_print(b, "say default", "muted for 300 more seconds"), "five minute default sets a real deadline");
    CHECK(h_t23_print(admin, "mute 1 90", "muted for 60 minutes"), "mute is capped at 60 minutes");
    CHECK(h_t23_print(b, "say capped", "muted for 3600 more seconds"), "cap sets a one-hour deadline");
    CHECK(h_t23_print(admin, "mute 1 1", "muted for 1 minute"), "one-minute mute set");
    int start = h_frames;
    int deadline = level.framenum + 60 * h_rate;
    h_frames_n(2 * h_rate);
    int texts = h_ntexts;
    CHECK(h_t23_print(b, "say before-pause", "muted for 58 more seconds") &&
          !h_text_to(a, 0, ": before-pause", texts), "muted message is refused and not delivered before timeout");
    int freeze = (level.framenum + h_div - 1) / h_div * h_div;
    h_client_command(a, "time");
    h_frames_n(3 * h_rate);
    CHECK(h_t23_print(b, "say in-pause", va("muted for %d more seconds", (deadline - freeze) / h_rate)),
          "mute display freezes with the match clock on the grid");
    h_view_resume(a);
    h_frames_n(deadline - 1 - level.framenum);
    CHECK(h_t23_print(b, "say early", "You are muted"), "chat blocked one match step before mute ends");
    h_frame();
    texts = h_ntexts;
    int accepted = h_t23_print(b, "say deadline", ": deadline");
    CHECK(accepted && h_text_to(a, 0, ": deadline", texts), "chat is delivered on first match step after mute ends");
    h_t23_sample("unmuted", h_frames - start, accepted);
    h_client_command(admin, "mute 1 1");
    CHECK(h_t23_print(admin, "unmute 1", "is not muted anymore"), "admin can cancel an active mute");
    accepted = h_t23_print(b, "say cancel-mute", ": cancel-mute");
    CHECK(accepted, "explicit unmute allows chat immediately");
    h_t23_sample("explicit_unmute", 0, accepted);
}

#endif
static void sc_t23_inactivity(void)
{
#ifdef HARNESS_OPENTDM
    h_set_override("g_motd_message", "fixture motd");
#endif
    h_set_override("g_idle_time", "12");
    h_scene_base();
    h_start(h_ents);
    int start = h_frames, tx = h_ntexts;
#ifdef HARNESS_OPENTDM
    edict_t *a = h_join(0, "p1");
    h_frames_n(11 * h_rate - h_frames);
    const htext_t *motd = h_text_to(a, H_SVC_LAYOUT, "fixture motd", tx);
    CHECK(motd && motd->frame - start == 10 * h_rate, "MOTD arrives after ten seconds");
    h_t23_sample("motd", motd ? motd->frame - start : -1, motd != NULL);
    h_client_command(a, "vote timelimit 1");
    CHECK(g_match_time->value == 60, "one player applies a nondefault setting");
    int idle = 13;
#else
    Q_strlcpy(game.motd, "fixture motd", sizeof(game.motd));
    edict_t *a = h_connect(0, "p1");
    h_frames_n(15 * h_rate / 10);
    CHECK(!h_text_to(a, H_SVC_LAYOUT, "fixture motd", tx), "pregame MOTD waits for its deadline");
    h_frame();
    const htext_t *motd = h_text_to(a, H_SVC_LAYOUT, "fixture motd", tx);
    CHECK(motd && motd->frame - start == 15 * h_rate / 10, "pregame MOTD arrives after 1.5 seconds");
    h_t23_sample("motd", motd ? motd->frame - start : -1, motd != NULL);
    h_tick(a, BUTTON_ATTACK, 1);
    h_tick(a, 0, 1);
    int idle = 12;
#endif
    h_frames_n(11 * h_rate);
    h_think(a, BUTTON_ATTACK, 10);
    h_think(a, 0, 10);
    h_frames_n(11 * h_rate);
#ifdef HARNESS_OPENTDM
    CHECK(a->client->pers.team == TEAM_A, "button transition refreshes warmup activity");
    h_client_command(a, "settings");
    start = h_frames;
#else
    CHECK(PlayerSpawned(a), "button transition refreshes player activity");
    start = h_frames - 11 * h_rate;
#endif
    int prints = h_nbprints;
#ifdef HARNESS_OPENTDM
    h_frames_n(start + idle * h_rate - h_frames);
#else
    h_frames_n(start + idle * h_rate + 1 - h_frames);
#endif
#ifdef HARNESS_OPENTDM
    CHECK(a->client->pers.team == TEAM_A, "command refreshes warmup activity");
#else
    CHECK(PlayerSpawned(a), "player stays active through the idle deadline");
#endif
    h_frame();
#ifdef HARNESS_OPENTDM
    h_t23_announcement("idle", "due to inactivity", prints, start, idle * h_rate);
    CHECK(a->client->pers.team == TEAM_SPEC, "inactive team player moves to spectator");
    int left = h_frames - 1;
    h_frames_n(left + 300 * h_rate - h_frames);
    CHECK(g_match_time->value == 60, "defaults are still pending one step before 300 s of inactivity");
    h_frame();
    int when, resets = h_t23_announcements("restoring default match settings", prints, &when);
    CHECK(resets == 1 && when == left + 300 * h_rate && g_match_time->value == 600,
          "empty session restores defaults exactly 300 s after the last player leaves");
    h_t23_sample("reset", when - left, resets);
#else
    int removed = 0;
    for (int i = prints; i < h_nbprints; i++)
        removed += strstr(h_bprints[i].text, "due to inactivity") != NULL;
    CHECK(a->client->pers.connected == CONN_SPECTATOR && removed == 1, "inactive player moves to spectator once");
    h_t23_sample("idle", h_frames - start - 1, removed);
#endif
    CHECK(!motd || !h_text_to(a, H_SVC_LAYOUT, "fixture motd", (int)(motd - h_texts) + 1),
          "MOTD is shown only once");
}

static edict_t *h_t23_item(const char *name)
{
    edict_t *item = G_Find(NULL, FOFS(classname), (char *)name);
    CHECK(item && item->solid == SOLID_TRIGGER && item->touch, "%s is available for pickup", name);
    return item && item->touch ? item : NULL;
}

static int h_to_grid(void)
{
    return G_WindowFrame(level.framenum) - level.framenum;
}

static void sc_t23_item_respawn(void)
{
    static const struct { const char *name; const char *cvar; int seconds; } items[] = {
#ifdef HARNESS_OPENTDM
        { "weapon_railgun", "g_respawn_weapon", 3 },
        { "ammo_shells", "g_respawn_ammo", 4 },
        { "item_armor_body", "g_respawn_armor", 5 },
        { "item_health", "g_respawn_health", 6 },
        { "item_quad", "g_respawn_quad", 8 },
        { "item_health_mega", "g_respawn_mega", 7 },
        { "item_adrenaline", "g_respawn_adren", 9 },
        { "item_ancient_head", NULL, 60 },
        { "item_bandolier", NULL, 60 },
        { "item_pack", "g_respawn_backpack", 10 },
        { "item_power_screen", "g_respawn_ps", 11 },
        { "item_power_shield", "g_respawn_ps", 11 },
        { "item_invulnerability", "g_respawn_invuln", 12 },
#else
        { "weapon_railgun", NULL, 30 },
        { "ammo_shells", NULL, 30 },
        { "item_armor_body", NULL, 20 },
        { "item_health", NULL, 30 },
        { "item_quad", NULL, 60 },
        { "item_health_mega", NULL, 20 },
        { "item_adrenaline", NULL, 60 },
        { "item_ancient_head", NULL, 60 },
        { "item_bandolier", NULL, 60 },
        { "item_pack", NULL, 180 },
        { "item_power_screen", NULL, 60 },
        { "item_power_shield", NULL, 60 },
        { "item_invulnerability", NULL, 300 },
#endif
    };
    char values[sizeof(items) / sizeof(items[0])][16];
    h_set_override("g_timeout_limit", "0");
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n}\n");
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (items[i].cvar) {
            snprintf(values[i], sizeof(values[i]), "%d", items[i].seconds);
            h_set_override(items[i].cvar, values[i]);
        }
        h_ent("{\n\"classname\" \"%s\"\n\"origin\" \"%d 1000 40\"\n}\n", items[i].name, 64 * (int)i);
    }
    h_start(h_ents);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);
    if (!h_t23_start(a, b)) return;
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        edict_t *item = h_t23_item(items[i].name);
        if (!item) return;
        h_real_phase(1 % h_div);
        int start = h_frames + h_to_grid(), paused = 0;
        a->health = i == 5 ? 2 : 50; /* mega takes just two one-point decay steps */
        int pickup = CS_ITEMS + ITEM_INDEX(item->item);
        item->touch(item, a, NULL, NULL);
        CHECK(item->solid == SOLID_NOT && (item->svflags & SVF_NOCLIENT) &&
              h_stat(a, STAT_PICKUP_STRING) == pickup, "%s pickup hides the item", items[i].name);
#ifdef HARNESS_OPENTDM
        if (i == 0) {
            int freeze = h_frames + (h_div - level.realframenum % h_div) % h_div;
            h_client_command(a, "time");
            h_frames_n(4 * h_rate);
            CHECK(tdm_match_status == MM_TIMEOUT && item->solid == SOLID_NOT, "item stays hidden across its wall-time deadline");
            h_view_resume(a);
            paused = h_frames - freeze - 1;
        }
#endif
        if (i == 5) {
            h_frames_n(start + 5 * h_rate - h_frames);
            CHECK(a->health == 102, "mega health holds for five seconds");
            h_frame();
            CHECK(a->health == 101, "mega health starts one-point decay");
            h_frames_n(h_rate);
            CHECK(a->health == 100, "mega health decays again one second later");
            h_t23_sample("mega_decay", h_frames - start - 1, a->health);
            h_frames_n(h_rate - 1);
            start = h_frames;
        }
        int deadline = start + paused + items[i].seconds * h_rate;
        h_frames_n(deadline - h_frames);
        CHECK(item->solid == SOLID_NOT, "%s absent one step before respawn", items[i].name);
        h_frame();
        int visible = item->solid == SOLID_TRIGGER && !(item->svflags & SVF_NOCLIENT);
        CHECK(visible, "%s respawns on its deadline", items[i].name);
        h_t23_sample(items[i].name, h_frames - start - paused - 1, visible);
        if (i == 3) a->health = 50;
        if (i == 2) a->client->inventory[ITEM_INDEX(item->item)] = 0;
        if (visible) item->touch(item, a, NULL, NULL);
        CHECK(visible && item->solid == SOLID_NOT && h_stat(a, STAT_PICKUP_STRING) == pickup,
              "%s can be picked up again", items[i].name);
    }
}

static void sc_t23_dropped_items(void)
{
    char flags[16];
    snprintf(flags, sizeof(flags), "%d", DF_QUAD_DROP);
#ifdef HARNESS_OPENTDM
    h_set_override("g_tdmflags", flags);
    h_set_override("g_1v1flags", flags);
#else
    h_set_override("dmflags", flags);
#endif
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    gitem_t *item = (gitem_t *)FindItem("Shells");
    h_real_phase(1 % h_div);
    edict_t *drop = Drop_Item(a, item);
    drop->movetype = MOVETYPE_NONE;
    int start = h_frames + h_to_grid(), index = ITEM_INDEX(item), initial = a->client->inventory[index];
    h_frames_n(start + h_rate - h_frames);
    drop->touch(drop, a, NULL, NULL);
    CHECK(drop->inuse && a->client->inventory[index] == initial, "owner blocked before pickup opens");
    h_frame();
    if (drop->inuse && drop->touch)
        drop->touch(drop, a, NULL, NULL);
    CHECK(!drop->inuse && a->client->inventory[index] > initial, "owner can pick up after one second");
    h_t23_sample("owner_pickup", h_frames - start - 1, a->client->inventory[index] - initial);

    h_real_phase(1 % h_div);
    drop = Drop_Item(a, item);
    drop->movetype = MOVETYPE_NONE;
    start = h_frames + h_to_grid();
    h_frames_n(start + 30 * h_rate - h_frames);
    CHECK(drop->inuse, "untouched drop still exists one step before 30 s");
    h_frame();
    CHECK(!drop->inuse, "untouched drop disappears at 30 s");
    h_t23_sample("drop_expired", h_frames - start - 1, !drop->inuse);

    h_real_phase(0);
    a->client->quad_framenum = level.framenum + 2 * h_rate;
    h_hit(a, 200);
    drop = G_Find(NULL, FOFS(classname), "item_quad");
    CHECK(drop && (drop->spawnflags & DROPPED_PLAYER_ITEM), "death drops a quad with two seconds left");
    if (!drop) return;
    start = h_frames;
    h_frames_n(h_rate / 2 + 1 % h_div);
    drop->touch(drop, b, NULL, NULL);
    h_real_phase(0);
    h_frame();
    CHECK(!drop->inuse && (b->s.effects & EF_QUAD), "pickup transfers dropped quad to opponent");
    h_frames_n(start + 2 * h_rate - h_frames);
    int icon = imp_imageindex("p_quad");
    CHECK(h_stat(b, STAT_TIMER_ICON) == icon, "transferred quad still active one step before the original deadline");
    h_frame();
    CHECK(h_stat(b, STAT_TIMER_ICON) != icon && !(b->s.effects & EF_QUAD), "transferred quad ends on the original deadline");
    h_t23_sample("inherited_quad", h_frames - start - 1, !!(b->s.effects & EF_QUAD));
    h_real_phase(1 % h_div);
    b->client->quad_framenum = level.framenum + h_to_grid() + h_rate;
    h_hit(b, 200);
    CHECK(!G_Find(NULL, FOFS(classname), "item_quad"), "no drop with exactly one second left");
    h_t23_sample("short_quad_drop", 0, G_Find(NULL, FOFS(classname), "item_quad") != NULL);
}

static const struct {
    const char *name, *icon, *warning;
    int effect;
} t23_powerups[] = {
    { "quad damage", "p_quad", "items/damage2.wav", EF_QUAD },
    { "invulnerability", "p_invulnerability", "items/protect2.wav", EF_PENT },
    { "environment suit", "p_envirosuit", "items/airout.wav", 0 },
    { "rebreather", "p_rebreather", "items/airout.wav", 0 },
};

static void h_t23_use_powerup(edict_t *p, int index)
{
    const char *name = t23_powerups[index].name;
    h_give(p, name, 0);
    h_client_command(p, va("use %s", name));
}

static int h_t23_blaster_damage(edict_t *p, edict_t *target)
{
    h_real_phase(0);
    h_tick(p, BUTTON_ATTACK, 1);
    edict_t *bolt = h_by_class("bolt", 0);
    for (int i = 0; !bolt && i < h_div; i++) {
        h_tick(p, BUTTON_ATTACK, 1);
        bolt = h_by_class("bolt", 0);
    }
    CHECK(bolt && bolt->owner == p, "blaster created a projectile");
    if (!bolt) return 0;
    int hp = h_hp_now(target);
    cplane_t plane = { 0 };
    bolt->touch(bolt, target, &plane, NULL);
    h_tick(p, 0, 1);
    return hp - h_hp_now(target);
}

static void sc_t23_powerup_lifetime(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    h_place(a, -800, 0, 24);
    h_tick(a, 0, 1);
    h_place(b, 800, 0, 24);
    h_tick(b, 0, 1);
    h_frames_n(2 * h_rate);
    edict_t *s = h_connect(2, "observer");
#ifdef HARNESS_OPENTDM
    h_client_command(s, "chase");
    CHECK(s->client->chase_target != NULL, "spectator follows a player");
    SetChase(s, a);
#else
    SetChaseTarget(s, a);
    CHECK(s->client->chase_target == a, "spectator follows a player");
#endif
    int invul_sound = imp_soundindex("items/protect4.wav");
    for (unsigned i = 0; i < sizeof(t23_powerups) / sizeof(t23_powerups[0]); i++) {
        h_real_phase(1 % h_div);
        int start = h_frames + h_to_grid(), icon = imp_imageindex(t23_powerups[i].icon);
        h_t23_use_powerup(a, i);
        h_frames_n(start + 1 - h_frames);
        CHECK(h_stat(a, STAT_TIMER) == 30 && h_stat(a, STAT_TIMER_ICON) == icon,
              "%s starts with 30 s and its icon", t23_powerups[i].name);
        h_frames_n(h_rate - 1);
        h_t23_use_powerup(a, i);
        h_frame();
        CHECK(h_stat(a, STAT_TIMER) == 59 && h_stat(a, STAT_TIMER_ICON) == icon,
              "%s adds 30 s to its remaining duration", t23_powerups[i].name);
        CHECK(h_stat(s, STAT_TIMER) == 59 && h_stat(s, STAT_TIMER_ICON) == icon,
              "observer sees the extended timer");
        h_t23_sample(va("extended_%d", i), h_frames - start - 1, h_stat(a, STAT_TIMER));
        if (i == 0) {
            int sounds = h_nsounds, quad = imp_soundindex("items/damage3.wav");
            int dealt = h_t23_blaster_damage(a, b);
            CHECK(dealt == 60 && h_count_sounds(quad, sounds) == 1, "quad shot deals fourfold damage with sound");
#ifdef HARNESS_OPENTDM
            CHECK(a->client->resp.teamplayerinfo->quad_dealt == 60, "quad damage is recorded");
#endif
            h_t23_sample("quad_damage", 0, dealt);
        }
        if (i == 1) {
            int hp = a->health, sounds = h_nsounds;
            h_hit(a, 15);
            h_hit(a, 15);
            h_frames_n(2 * h_rate);
            h_hit(a, 15);
            CHECK(a->health == hp && h_count_sounds(invul_sound, sounds) == 1, "invulnerability blocks damage with a two second sound debounce");
            h_frame();
            h_hit(a, 15);
            CHECK(h_count_sounds(invul_sound, sounds) == 2, "protection sound becomes eligible after its debounce");
        }
        h_frames_n(start + 58 * h_rate + 1 - h_frames);
        CHECK(h_stat(a, STAT_TIMER) == 2, "%s has two seconds left", t23_powerups[i].name);
        h_frames_n(2 * h_rate - 1);
        CHECK(h_stat(a, STAT_TIMER_ICON) == icon && h_stat(s, STAT_TIMER_ICON) == icon,
              "%s icon remains through its last active tick", t23_powerups[i].name);
        h_frame();
        CHECK(h_stat(a, STAT_TIMER) == 0 && h_stat(a, STAT_TIMER_ICON) == 0 &&
              !(a->s.effects & t23_powerups[i].effect), "%s expires", t23_powerups[i].name);
        h_t23_sample(va("expired_%d", i), h_frames - start - 1, h_stat(a, STAT_TIMER_ICON));
        if (i == 0) {
            int sounds = h_nsounds, quad = imp_soundindex("items/damage3.wav");
            CHECK(h_t23_blaster_damage(a, b) == 15 && h_count_sounds(quad, sounds) == 0,
                  "after expiry blaster damage and sound return to ordinary");
#ifdef HARNESS_OPENTDM
            CHECK(a->client->resp.teamplayerinfo->quad_dealt == 60, "ordinary hit does not count toward quad statistics");
#endif
        }
        if (i == 1) {
            int hp = a->health;
            h_hit(a, 10);
            CHECK(a->health < hp, "expired invulnerability no longer blocks damage");
        }
    }
    h_real_phase(0);
    h_t23_use_powerup(a, 0);
    int both = h_frames;
    h_t23_use_powerup(a, 1);
    h_frame();
    CHECK(h_stat(a, STAT_TIMER_ICON) == imp_imageindex("p_quad") &&
          h_stat(a, STAT_TIMER2_ICON) == imp_imageindex("p_invulnerability") &&
          h_stat(a, STAT_TIMER) == 30 && h_stat(a, STAT_TIMER2) == 30,
          "quad and invulnerability occupy two timer slots");
    h_t23_sample("secondary_timer", 0, h_stat(a, STAT_TIMER2));
    h_frames_n(h_rate);
    CHECK(h_stat(a, STAT_TIMER2) == 29 && h_stat(s, STAT_TIMER2) == 29 &&
          h_stat(s, STAT_TIMER2_ICON) == imp_imageindex("p_invulnerability"),
          "secondary timer decreases for the player and the observer");
    h_t23_sample("secondary_tick", h_frames - both - 1, h_stat(s, STAT_TIMER2));
    h_frames_n(both + 30 * h_rate - h_frames);
    CHECK(h_stat(a, STAT_TIMER2_ICON) && h_stat(s, STAT_TIMER2_ICON), "secondary timer icon persists through the last tick");
    h_frame();
    CHECK(!h_stat(a, STAT_TIMER2_ICON) && !h_stat(s, STAT_TIMER2_ICON), "secondary timer clears on both HUDs at expiry");
}

static void sc_t23_powerup_warning(void)
{
    h_set_override("g_timeout_limit", "0");
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    gclient_t *cl = a->client;
    int *ends[] = { &cl->quad_framenum, &cl->invincible_framenum, &cl->enviro_framenum, &cl->breather_framenum };
    int air = imp_soundindex("items/airout.wav");
    for (int i = 0; i < 5; i++) {
        int power = i % 4;
        int sound = imp_soundindex(t23_powerups[power].warning);
        for (int k = 0; k < 4; k++) *ends[k] = 0;
        h_real_phase(0);
        int start = h_frames, mark = h_nsounds;
        *ends[power] = level.framenum + 34 * h_rate / 10;
        if (i == 4) *ends[2] = *ends[0];
        h_frames_n(4 * h_rate / 10);
        CHECK(h_count_sounds(sound, mark) == 0, "powerup %d is quiet before 3 s", i);
        h_frames_n(h_div);
        int at = -1, warnings = h_count_sounds(sound, mark);
        for (int n = mark; n < h_nsounds; n++)
            if (h_sounds_log[n].soundindex == sound) at = h_sounds_log[n].frame;
        CHECK(warnings == 1 && at - start == 4 * h_rate / 10, "powerup %d warns once at 3 s", i);
        CHECK(cl->ps.blend[3] > 0 && (!t23_powerups[power].effect || (a->s.effects & t23_powerups[power].effect)),
              "powerup %d still shows its effect at the warning threshold", i);
        h_t23_sample(va("warning_%d", i), at - start, warnings);
        if (i == 4) {
            int obscured = h_count_sounds(air, mark);
            CHECK(obscured == 0, "quad suppresses the simultaneous enviro warning");
            h_t23_sample("enviro_under_quad", 0, obscured);
        }
        h_frames_n(5 * h_rate / 10);
        CHECK(cl->ps.blend[3] == 0, "powerup %d reaches an off phase in the last 3 s", i);
        h_frames_n(start + 34 * h_rate / 10 + 1 - h_frames);
        CHECK(h_count_sounds(sound, mark) == 1 && (i != 4 || h_count_sounds(air, mark) == 0),
              "powerup %d has no additional warnings through natural expiry", i);

#ifdef HARNESS_OPENTDM
        while (h_frames % h_div) h_frame();
        *ends[power] = level.framenum + 3 * h_rate;
        if (i == 4) *ends[2] = *ends[0];
        mark = h_nsounds;
        start = h_frames;
        h_client_command(a, "time");
        h_frames_n(4 * h_rate);
        int during = h_count_sounds(sound, mark);
        CHECK(tdm_match_status == MM_TIMEOUT && during == 0 &&
              h_stat(a, STAT_TIMER_ICON) == imp_imageindex(t23_powerups[power].icon) &&
              cl->ps.blend[3] > 0, "powerup %d stays active and quiet past its wall-time deadline", i);
        h_t23_sample(va("silent_%d", i), h_frames - start, during);
        h_view_resume(a);
        int resume = h_frames - 1;
        h_frames_n(h_div);
        h_check_powerup_warning_resume(sound, mark, resume);
#endif
    }
}

#ifdef HARNESS_OPENTDM
static int h_t23_statusbar(edict_t *p, int from)
{
    int at = -1, count = 0;
    for (int i = from; i < h_nmsgs; i++)
        if (h_msgs[i].svc == H_SVC_CONFIGSTRING && h_msgs[i].sub == CS_STATUSBAR &&
            h_msgs[i].entnum == NUM(p)) {
            if (!count++) at = h_msgs[i].frame;
        }
    CHECK(count <= 1, "statusbar update is unique since observation %d (got %d)", from, count);
    return at;
}

#endif
static void sc_t23_item_hud(void)
{
    h_set_override("g_tdmflags", "0");
    h_set_override("g_1v1flags", "0");
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n}\n");
    h_ent("{\n\"classname\" \"ammo_grenades\"\n\"origin\" \"0 1000 40\"\n}\n");
    h_ent("{\n\"classname\" \"weapon_railgun\"\n\"origin\" \"0 1100 40\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1"), *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);
    if (!h_t23_start(p, b)) return;
    h_give(p, "machinegun", 0);
    h_give(p, "bullets", 20);
    h_client_command(p, "use machinegun");
    h_frames_n(3 * h_rate);
    edict_t *grenades = h_t23_item("ammo_grenades"), *railgun = h_t23_item("weapon_railgun");
    if (!grenades || !railgun) return;
    int start;
#ifdef HARNESS_OPENTDM
    int mark = h_nmsgs;
    start = h_frames;
#endif
    int grenade_index = ITEM_INDEX(grenades->item), rail_index = ITEM_INDEX(railgun->item);
    grenades->touch(grenades, p, NULL, NULL);
    CHECK(h_stat(p, STAT_PICKUP_STRING) == CS_ITEMS + grenade_index, "grenade pickup is shown");
#ifdef HARNESS_OPENTDM
    h_frames_n(h_rate);
    CHECK(h_t23_statusbar(p, mark) < 0, "no statusbar before the first second");
    h_frame();
    int first = h_t23_statusbar(p, mark);
    CHECK(first == start + h_rate, "grenade pickup updates statusbar after one second");
    h_t23_sample("grenade_pickup", first - start, first >= 0);
    mark = h_nmsgs;
#endif
    h_client_command(p, "drop grenades");
    CHECK(p->client->inventory[grenade_index] == 0, "drop removes grenades");
#ifdef HARNESS_OPENTDM
    h_frames_n(start + 3 * h_rate - h_frames);
    CHECK(h_t23_statusbar(p, mark) < 0, "drop statusbar waits for the two second cooldown");
    h_frame();
    int second = h_t23_statusbar(p, mark);
    CHECK(second == first + 2 * h_rate, "drop statusbar on the first eligible step");
    h_t23_sample("grenade_drop", second - first, second >= 0);
#endif

    h_give(p, "quad damage", 0);
    h_real_phase(1 % h_div);
    start = h_frames;
    int pickup_deadline = start + h_to_grid() + 3 * h_rate;
#ifdef HARNESS_OPENTDM
    mark = h_nmsgs;
#endif
    railgun->touch(railgun, p, NULL, NULL);
#ifdef HARNESS_OPENTDM
    h_client_command(p, "armortimer");
    h_client_command(p, "weapontimer");
    h_frame();
    CHECK(h_stat(p, STAT_TIMER) == 20 && h_stat(p, STAT_TIMER2) == 30 &&
          h_stat(p, STAT_TIMER_ICON) == imp_imageindex("i_combatarmor") &&
          h_stat(p, STAT_TIMER2_ICON) == imp_imageindex("w_blaster"), "manual armor and weapon HUD timers");
#endif
    h_client_command(p, "use quad damage");
    h_frame();
    CHECK(h_stat(p, STAT_TIMER_ICON) == imp_imageindex("p_quad"), "quad takes precedence over armor icon");
    h_frames_n(pickup_deadline + h_div - h_frames);
    CHECK(h_stat(p, STAT_PICKUP_STRING) == CS_ITEMS + rail_index, "pickup HUD persists through three seconds");
#ifdef HARNESS_OPENTDM
    int third = h_t23_statusbar(p, mark);
    CHECK(third == second + 2 * h_rate, "weapon pickup updates after the statusbar cooldown");
    h_t23_sample("weapon_pickup", third - second, third >= 0);
#endif
    h_frame();
    CHECK(h_stat(p, STAT_PICKUP_STRING) == 0 && h_stat(p, STAT_PICKUP_ICON) == 0, "pickup HUD clears on the next grid frame");
    h_t23_sample("pickup_hud_clear", h_frames - start - 1, h_stat(p, STAT_PICKUP_ICON));
#ifdef HARNESS_OPENTDM
    int drop = h_frames;
    mark = h_nmsgs;
#endif
    h_client_command(p, "drop railgun");
    h_frames_n(2 * h_rate);
    CHECK(!p->client->inventory[rail_index], "weapon drop removes inventory");
#ifdef HARNESS_OPENTDM
    int update = h_t23_statusbar(p, mark);
    CHECK(update == drop + h_rate,
          "weapon drop updates statusbar exactly one second later");
    h_t23_sample("weapon_drop", update - drop, update >= 0);
#endif
}

static void sc_t23_power_armor(void)
{
    static const struct { const char *name; int effect; } armor[] = {
        { "power screen", EF_POWERSCREEN },
        { "power shield", EF_COLOR_SHELL },
    };
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_give(p, "body armor", 0);
    h_give(p, "cells", 50);
    int cell_index = ITEM_INDEX(FindItem("cells"));
    for (unsigned i = 0; i < sizeof(armor) / sizeof(armor[0]); i++) {
        h_give(p, armor[i].name, 0);
        if (!(p->flags & FL_POWER_ARMOR)) h_client_command(p, va("use %s", armor[i].name));
        int phase = level.framenum % (16 * h_div);
        h_frames_n((16 * h_div - phase) % (16 * h_div));
        vec3_t ahead, point, zero = { 0, 0, 0 };
        AngleVectors(p->s.angles, ahead, NULL, NULL);
        VectorMA(p->s.origin, 16, ahead, point);
        int initial = p->client->inventory[cell_index], start = h_frames;
        T_Damage(p, world, world, zero, point, zero, 18, 0, 0, MOD_TRIGGER_HURT);
        h_frames_n(h_rate / 5);
        int cells = p->client->inventory[cell_index];
        CHECK(cells == initial - 6 && (p->s.effects & armor[i].effect), "%s consumes six cells and its effect lasts through 0.2 s", armor[i].name);
        h_t23_sample(va("active_%d", i), h_frames - start - 1, cells);
        for (int phase = 2; phase <= 8; phase++) {
            h_frame();
            CHECK(!(p->s.effects & armor[i].effect), "%s effect clears at 0.2 s", armor[i].name);
            int want = imp_imageindex(phase & 8 ? "i_powershield" : "i_bodyarmor");
            CHECK(h_stat(p, STAT_ARMOR_ICON) == want, "armor HUD alternates on 100 ms phases");
            h_t23_sample(va("icon_%d_%d", i, phase), h_frames - start - 1, h_stat(p, STAT_ARMOR_ICON));
            h_frames_n(h_div - 1);
        }
    }
}

static void h_t23_view_feedback(edict_t *p)
{
    h_real_phase(1 % h_div);
    int start = h_frames, sounds = h_nsounds, hp = h_hp_now(p);
    int next = 0, first = -1, flash_ticks = 0;
    float peak = 0;
    for (int tick = 0; tick < 3 * h_div; tick++) {
        for (; next < 5 && next * h_div / 5 == tick; next++) h_hit(p, 5);
        h_frame();
        if (h_stat(p, STAT_FLASHES)) {
            if (first < 0) first = h_frames - 1;
            flash_ticks++;
        }
        if (p->client->ps.blend[3] > peak) peak = p->client->ps.blend[3];
    }
    int dealt = hp - h_hp_now(p), pain = h_count_prefix("*pain", sounds);
    CHECK(dealt == 25 && pain == 1, "five hits in one window deal 25 and play one pain sound");
    CHECK(flash_ticks == h_div && fabsf(peak - 0.25f) < 0.001f, "flash holds 100 ms and peak blend is 0.25");
    h_t23_sample("flash", first - start, dealt);
    h_t23_sample("flash_length", flash_ticks, pain);
    h_t23_sample("damage_peak", 0, lroundf(1000 * peak));
    for (int i = 0; i < 3; i++) {
        h_t23_sample(va("damage_blend_%d", i), i * h_div, lroundf(1000 * p->client->ps.blend[3]));
        h_frames_n(h_div);
    }
    sounds = h_nsounds;
    h_hit(p, 10);
    h_frames_n(h_div);
    CHECK(h_count_prefix("*pain", sounds) == 0, "debounce suppresses pain in the next damage window");
    h_t23_sample("protected_pain", 0, h_count_prefix("*pain", sounds));
    h_frames_n(h_rate);
    edict_t *pickup = Drop_Item(world, FindItem("Shells"));
    pickup->touch(pickup, p, NULL, NULL);
    for (int i = 0; i < 4; i++) {
        h_frame();
        float blend = p->client->ps.blend[3];
        CHECK(fabsf(blend - fmaxf(0, 0.25f - 0.1f * i)) < H_EPS,
              "pickup blend decays by elapsed game time at sample %d (%.3f)", i, blend);
        h_t23_sample(va("pickup_blend_%d", i), i * h_div, lroundf(1000 * blend));
        h_frames_n(h_div - 1);
    }
}

static void h_t23_view_bob(edict_t *p)
{
    static const struct { int speed; float advance; } steps[] = {
        { 80, 0.3125f }, { 180, 0.625f }, { 300, 1.25f },
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        VectorClear(p->velocity);
        h_frame();
        h_real_phase(0);
        VectorSet(p->velocity, steps[i].speed, 0, 0);
        for (int tick = 0; tick < h_rate / 2; tick++) {
            p->groundentity = world;
            h_frame();
        }
        float bob = p->client->bobtime;
        CHECK(fabsf(bob - steps[i].advance) < H_EPS, "half a second at speed %d advances bob to %.4f", steps[i].speed, bob);
        h_t23_sample(va("run_bob_%d", i), h_rate / 2, lroundf(10000 * bob));
    }
    float hold = p->client->bobtime;
    for (int i = 0; i < h_rate / 2; i++) {
        p->groundentity = NULL;
        h_frame();
    }
    CHECK(p->client->bobtime == hold, "airborne bob preserves the running phase");
    h_t23_sample("air_bob", h_rate / 2, lroundf(1000 * p->client->bobtime));
    VectorClear(p->velocity);
    p->groundentity = world;
    h_frames_n(h_rate);
}

static void h_t23_view_turn(edict_t *p)
{
    gclient_t *cl = p->client;
    h_real_phase(0);
    float yaw = cl->ps.viewangles[YAW] + 12;
    cl->ps.viewangles[YAW] = cl->v_angle[YAW] = yaw;
    h_frame();
    CHECK(fabsf(cl->ps.gunangles[YAW] + 2.4f) < 0.001f && cl->oldviewangles[YAW] == yaw,
          "12 degree turn is shown with 2.4 degrees of gun sway");
    CHECK(VectorCompare(cl->ps.gunoffset, vec3_origin), "default gun offset is zero");
    int start = h_frames - 1;
    h_t23_sample("turn_before_pause", 0, lroundf(1000 * cl->ps.gunangles[YAW]));
    for (int i = 1; i < h_div; i++) {
        cl->ps.viewangles[YAW] = cl->v_angle[YAW] = yaw + i;
        h_frame();
        CHECK(fabsf(cl->ps.gunangles[YAW] + 2.4f) < H_EPS && cl->oldviewangles[YAW] == yaw,
              "changing input leaves gun angle and history held until presentation");
    }
    cl->ps.viewangles[YAW] = cl->v_angle[YAW] = yaw;
#ifdef HARNESS_OPENTDM
    h_view_pause(p);
    h_t23_sample("pause_entry", h_frames - start, lroundf(1000 * cl->ps.gunangles[YAW]));
    start = h_frames;
    for (int i = 0; i < 5; i++) {
        h_frame();
        CHECK(fabsf(cl->ps.gunangles[YAW]) < H_EPS,
              "pre-pause sway clears at presentation step %d (yaw %.3f)", i, cl->ps.gunangles[YAW]);
        h_t23_sample(va("pause_yaw_%d", i), h_frames - start - 1, lroundf(1000 * cl->ps.gunangles[YAW]));
        h_frames_n(h_div - 1);
    }
    h_t23_sample("before_resume", 0, lroundf(1000 * cl->ps.gunangles[YAW]));
    h_view_resume(p);
    start = h_frames - 1;
    for (int i = 0; i < 2; i++) {
        CHECK(fabsf(cl->ps.gunangles[YAW]) < H_EPS, "no stale gun sway around Fight! (sample %d)", i);
        h_t23_sample(va("resume_yaw_%d", i), h_frames - start - 1, lroundf(1000 * cl->ps.gunangles[YAW]));
        h_frames_n(h_div);
    }
#else
    h_frame();
    CHECK(fabsf(cl->ps.gunangles[YAW]) < H_EPS, "turn sway clears at the next active presentation step");
    h_t23_sample("turn_cleared", h_frames - start - 1, lroundf(1000 * cl->ps.gunangles[YAW]));
#endif
}

static void sc_t23_view_batches(void)
{
#ifdef HARNESS_OPENTDM
    edict_t *a = h_view_player();
    if (!a) return;
#else
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    h_place(a, -800, 0, 24);
    h_tick(a, 0, 1);
    h_frames_n(h_rate);
#endif
    h_t23_view_feedback(a);
    h_t23_view_bob(a);
    h_t23_view_turn(a);
    h_real_phase(0);
#ifdef HARNESS_OPENTDM
    a->client->kick_angles_final[PITCH] = -1;
    a->client->kick_origin_start = 0;
    a->client->kick_origin_end = h_div;
#else
    a->client->kick_angles[PITCH] = -1;
#endif
    int held = 0;
    for (int i = 0; i < 2 * h_div; i++) {
        h_frame();
        held += a->client->ps.kick_angles[PITCH] == -1;
    }
    CHECK(held == h_div && a->client->ps.kick_angles[PITCH] == 0, "ordinary weapon kick holds one presentation window");
    h_t23_sample("weapon_kick", held, held > 0);
}

static void sc_t23_lava_slime(void)
{
    edict_t *p, *b;
    h_tdm_pair(&p, &b);
    if (!h_t23_start(p, b)) return;
#ifdef HARNESS_OPENTDM
    int sound[] = { imp_soundindex("player/burn2.wav"), imp_soundindex("player/burn1.wav") };
#else
    int sound[] = { imp_soundindex("player/burn1.wav"), imp_soundindex("player/burn2.wav") };
#endif
    for (int phase = 0; phase < h_div; phase++) {
        for (int medium = 0; medium < 2; medium++) {
            for (int shield = 0; shield < 2; shield++) {
                p->waterlevel = 0;
                p->client->enviro_framenum = 0;
                h_frames_n(2 * h_rate);
                h_real_phase(phase);
                p->health = 100;
                p->pain_debounce_framenum = 0;
#ifdef HARNESS_OPENTDM
                p->client->ps.gunframe = 9;
                b->client->ps.gunframe = 9;
#else
                p->client->weaponframe = 9;
                b->client->weaponframe = 9;
#endif
                h_t23_seed(shield);
                p->waterlevel = 1;
                p->watertype = medium ? CONTENTS_SLIME : CONTENTS_LAVA;
                if (shield) p->client->enviro_framenum = level.framenum + 3 * h_rate;
                int start = h_frames, mark = h_nsounds, hits = 0, prev = -1;
                for (int t = 0; t < h_rate; t++) {
                    int hp = p->health;
                    h_frame();
                    if (hp > p->health) {
                        if (prev >= 0) CHECK(t - prev == h_div, "medium %d hits are 100 ms apart", medium);
                        prev = t;
                        hits++;
                    }
                }
                int wanted = medium ? (shield ? 0 : 1) : (shield ? 1 : 3);
                CHECK(p->health == 100 - 10 * wanted && hits == (wanted ? 10 : 0),
                      "medium %d protection %d phase %d: %d hits, %d damage", medium, shield, phase, hits, 100 - p->health);
                int burns = h_count_prefix("player/burn", mark);
                CHECK(burns == !medium && (medium || h_count_sounds(sound[shield], mark) == 1),
                      "medium %d protection %d phase %d: deterministic burn and one-second debounce", medium, shield, phase);
                if (!phase) {
                    h_t23_sample(va("damage_%d_%d", medium, shield), h_frames - start, 100 - p->health);
                    h_t23_sample(va("burn_%d_%d", medium, shield), 0, burns);
                }
                if (!medium) {
                    int first_grid = (h_div - phase) % h_div;
                    CHECK(h_sounds_log[mark].frame == start + first_grid, "lava burns on the first grid frame");
                    h_frames_n(first_grid + 1);
                    CHECK(h_count_prefix("player/burn", mark) == 2 &&
                          h_sounds_log[h_nsounds - 1].frame == start + first_grid + h_rate,
                          "next burn sound is allowed after exactly one second");
                    if (!phase) h_t23_sample(va("burn_again_%d", shield), h_frames - start - 1, 2);
                }
            }
        }
    }
#ifdef HARNESS_OPENTDM
    for (int medium = 0; medium < 2; medium++) {
        p->waterlevel = 1;
        p->watertype = medium ? CONTENTS_SLIME : CONTENTS_LAVA;
        p->client->enviro_framenum = 0;
        if (!medium) h_client_command(p, "time");
        int hp = p->health, sounds = h_nsounds;
        h_frames_n(2 * h_rate);
        CHECK(tdm_match_status == MM_TIMEOUT && p->health == hp && h_count_prefix("player/burn", sounds) == 0,
              "medium %d is harmless and silent in timeout", medium);
        h_t23_sample(va("paused_medium_%d", medium), 2 * h_rate, hp - p->health);
    }
#endif
}

static void sc_t23_drowning(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    h_real_phase(0);
    p->health = 100;
    p->waterlevel = 3;
    p->watertype = CONTENTS_WATER;
    int start = h_frames, mark = h_nsounds, hits = 0, previous = -1;
    for (int t = 0; t < 15 * h_rate; t++) {
        int hp = p->health;
        h_frame();
        if (p->health >= hp) continue;
        int gap = previous < 0 ? t : t - previous;
        int want = previous < 0 ? 12 * h_rate : 11 * h_rate / 10;
        CHECK(gap == want, "drown hit %d after %d steps (expected %d)", hits + 1, gap, want);
        previous = t;
        CHECK(hp - p->health == 4 + 2 * hits, "drown hit %d deals %d", hits + 1, hp - p->health);
        h_t23_sample(va("drown_%d", hits), h_frames - start - 1, hp - p->health);
        hits++;
    }
    CHECK(hits == 3 && h_count_prefix("*gurp", mark) == 3 && h_count_prefix("*pain", mark) == 0,
          "three gulp sounds and no double pain after air expires");
    p->waterlevel = 0;
    h_real_phase(0);
    h_frame();
    CHECK(h_count_prefix("player/gasp1.wav", mark) == 1, "surfacing after running out of air gasps loudly");
    p->waterlevel = 3;
    h_frames_n(2 * h_rate);
    p->waterlevel = 0;
    h_real_phase(0);
    h_frame();
    CHECK(h_count_prefix("player/gasp2.wav", mark) == 1, "surfacing before air expires uses normal gasp");
    p->waterlevel = 3;
    start = h_frames;
    int first_drown = 12 * h_rate + h_div - 1;
    int fresh_health = p->health;
    for (int i = 0; i < first_drown; i++) {
        h_frame();
        if (p->health != fresh_health) break;
    }
    CHECK(h_frames == start + first_drown && p->health == fresh_health,
          "surfacing replenishes the full twelve seconds of air");
    h_frame();
    CHECK(fresh_health - p->health == 4, "the renewed air supply ends with a fresh four-point hit");
    h_t23_sample("renewed_air", h_frames - start - 1, fresh_health - p->health);
    mark = h_nsounds;
    int last_drown = h_frames - 1;
    h_frames_n(h_div - 1);
    h_hit(p, 1);
    h_frame();
#ifndef HARNESS_OPENTDM
    /* OpenFFA excludes the debounce deadline itself and processes pain on keyframes. */
    CHECK(!h_count_prefix("*pain", mark), "the first keyframe after drowning suppresses ordinary pain");
    h_frames_n(2 * h_div - last_drown % h_div - (h_frames - last_drown));
    h_hit(p, 1);
    h_frame();
#endif
    CHECK(h_count_prefix("*pain", mark) == 1, "ordinary pain returns on the first eligible keyframe after drowning");
    h_t23_sample("drown_pain_returns", h_frames - last_drown - 1, h_count_prefix("*pain", mark));

    h_real_phase(0);
    h_t23_use_powerup(p, 3);
    p->waterlevel = 3;
    mark = h_nsounds;
    start = h_frames;
    h_frames_n(5 * h_rate + 1);
    int breaths = 0;
    for (int i = mark; i < h_nsounds; i++) {
        const hsound_t *sound = &h_sounds_log[i];
        if (strncmp(h_sounds[sound->soundindex], "player/u_breath", strlen("player/u_breath"))) continue;
        int at = sound->frame - start;
        CHECK(at == breaths * 5 * h_rate / 2, "breath %d at %d steps", breaths, at);
        h_t23_sample(va("breath_%d", breaths++), at, 1);
    }
    CHECK(breaths == 3, "rebreather sounds at 0, 2.5 and 5 seconds");
    h_t23_sample("breaths", 5 * h_rate, breaths);
    p->client->breather_framenum = 0;
    start = h_frames;
    int hp = p->health;
    h_frames_n(10 * h_rate + h_div - 1);
    CHECK(p->health == hp, "last assisted breath provides ten seconds of air");
    h_frame();
    CHECK(p->health < hp, "air runs out ten seconds after rebreather stops");
    h_t23_sample("assisted_air", h_frames - start - 1, hp - p->health);
}

static void h_t23_animation(edict_t *p, const char *tag, int frames, int first)
{
    int start = h_frames, prev = p->s.frame, last = -1, n = 0;
    for (int i = 0; i < frames; i++) {
        h_frame();
        if (p->s.frame == prev) continue;
        CHECK(i == (last >= 0 ? last + h_div : first), "%s animation change %d at step %d", tag, n, i);
        last = i;
        prev = p->s.frame;
        h_t23_sample(va("%s_%d", tag, n), h_frames - start - 1, prev);
        n++;
    }
    CHECK(n == frames / h_div, "%s displays %d animation frames (got %d)", tag, frames / h_div, n);
}

static void sc_t23_player_animation(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(2 * h_rate);
    int idle = p->s.frame;
    for (int i = 0; i < h_div && p->s.frame == idle; i++) h_frame();
    CHECK(p->s.frame != idle, "standing animation gives a reference step before running");
    p->groundentity = world;
    VectorSet(p->velocity, 180, 0, 0);
    h_t23_animation(p, "run", 5 * h_div, h_div - 1);
    VectorClear(p->velocity);
    h_frames_n(h_rate);
    h_real_phase(1 % h_div);
    CHECK(h_t23_print(p, "wave 1", "salute"), "wave dispatch accepted between animation steps");
#ifdef HARNESS_OPENTDM
    h_t23_animation(p, "salute", 12 * h_div, 0);
#else
    h_t23_animation(p, "salute", 12 * h_div, h_div - 1);
#endif
    CHECK(p->client->anim_priority == ANIM_BASIC, "salute returns to idle after its last frame");
}

static void h_t23_spawn_boundary(edict_t *p, edict_t *other, int boundary, const char *tag)
{
    CHECK(level.numspawns == 2, "spawn test needs precisely two spots");
    if (level.numspawns != 2) return;
    edict_t *occupied = level.spawns[0], *free = level.spawns[1];
    h_place(p, -600, 0, 24);
    h_tick(p, 0, 1);
    h_place(other, occupied->s.origin[0], occupied->s.origin[1], 24);
    h_tick(other, 0, 1);
    h_frames_n(boundary - h_frames - 1);
    int solid = other->solid;
    other->solid = SOLID_NOT;
    gi.linkentity(other);
    unsigned seed = 0;
    do {
        h_t23_seed(seed++);
#ifdef HARNESS_OPENTDM
    } while (genrand_uniform(2) != 0);
#else
    } while (Q_rand_uniform(2) != 0);
#endif
    h_t23_seed(seed - 1);
#ifdef HARNESS_OPENTDM
    respawn(p);
#else
    PutClientInServer(p);
#endif
    int before = fabsf(p->s.origin[0] - free->s.origin[0]) < H_EPS;
    CHECK(before, "respawn avoids occupied spot before %s boundary", tag);
    h_frame();
    h_place(p, -600, 0, 24);
    h_think(p, 0, 10);
    h_t23_seed(seed - 1);
#ifdef HARNESS_OPENTDM
    respawn(p);
#else
    PutClientInServer(p);
#endif
    int after = fabsf(p->s.origin[0] - occupied->s.origin[0]) < H_EPS;
    CHECK(after, "seeded random respawn mode returns at %s boundary", tag);
    h_t23_sample(tag, 0, before + after);
    other->solid = solid;
    h_place(other, 800, 0, 24);
    h_think(other, 0, 10);
}

static void sc_t23_respawn(void)
{
    char flags[16];
    snprintf(flags, sizeof(flags), "%d", DF_FORCE_RESPAWN);
#ifdef HARNESS_OPENTDM
    h_set_override("g_tdmflags", flags);
    h_set_override("g_1v1flags", flags);
    h_set_override("g_respawn_time", "3");
    const int kill_wait = 1;
#else
    h_set_override("dmflags", flags);
    const int kill_wait = 0;
#endif
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
#ifdef HARNESS_OPENTDM
    CHECK(tdm_match_status == MM_WARMUP, "map opens in warmup");
    h_t23_spawn_boundary(a, b, 5 * h_rate, "warmup_spawn");
    int prints = h_nbprints, fight = -1;
    if (!h_tdm_start(a, b)) return;
    CHECK(h_t23_announcements("Fight!", prints, &fight) == 1, "one Fight announcement anchors match spawn");
    h_t23_spawn_boundary(a, b, fight + h_rate, "match_spawn");
#else
    if (!h_t23_start(a, b)) return;
    h_t23_spawn_boundary(a, b, 5 * h_rate, "map_start_spawn");
#endif
    h_place(b, 800, 0, 24);
    h_tick(b, 0, 1);
    h_frames_n((h_div - h_frames % h_div) % h_div);
    int start = h_frames;
    h_hit(a, 200);
    h_frames_n(h_rate);
    h_think(a, BUTTON_ATTACK, 10);
    h_frame();
    CHECK(a->deadflag, "manual respawn waits through the one-second boundary");
    h_think(a, 0, 10);
    h_frames_n(start + h_rate + h_div - h_frames);
    CHECK(a->deadflag, "manual respawn waits for the first eligible grid frame");
    h_frame();
    CHECK(!a->deadflag && a->health > 0, "manual respawn on the first eligible grid frame");
    h_t23_sample("manual_spawn", h_frames - start - 1, a->health);
    start = h_frames - kill_wait;
    int spawn = a->client->respawn_framenum;
    int kill_at = G_WindowFrame(spawn) + 5 * h_rate;
    h_frames_n(kill_at - h_div - level.framenum);
    h_client_command(a, "kill");
    CHECK(!a->deadflag, "kill rejected one tick before five seconds from respawn");
    h_frame();
    h_client_command(a, "kill");
    CHECK(a->deadflag, "kill accepted at five seconds from respawn");
    h_t23_sample("kill_after_spawn", h_frames - start, !!a->deadflag);
    start = h_frames;
    int death_window = (start + h_div - 1) / h_div * h_div;
    int force_at = death_window + 3 * h_rate;
    h_frames_n(force_at + 1 - h_frames);
    CHECK(a->deadflag, "forced respawn waits through its deadline");
    h_frames_n(force_at + h_div - h_frames);
    CHECK(a->deadflag, "forced respawn waits for the first eligible grid frame");
    h_frame();
    CHECK(!a->deadflag && a->health > 0, "forced respawn on the first eligible grid frame");
    h_t23_sample("forced_spawn", h_frames - start - 1, a->health);
}

static void sc_t23_weapon_pain(void)
{
    static const struct { const char *name; int state, gunframe; } cases[] = {
        { "rocket launcher", WEAPON_READY, 13 },
        { "grenades", WEAPON_READY, 16 },
        { "machinegun", WEAPON_FIRING, 4 },
        { "hyperblaster", WEAPON_FIRING, 6 },
        { "chaingun", WEAPON_FIRING, 5 },
    };
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    int sound = imp_soundindex("weapons/noammo.wav");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gitem_t *weapon = (gitem_t *)FindItem((char *)cases[i].name);
        if (!weapon) imp_error("missing weapon %s", cases[i].name);
        h_real_phase(0);
        int start = h_frames, sounds = h_nsounds, msgs = h_nmsgs;
#ifdef HARNESS_OPENTDM
        int ammo = weapon->ammoindex;
#else
        int ammo = ITEM_INDEX(weapon->ammo ? FindItem(weapon->ammo) : weapon);
#endif
        for (int attempt = 0; attempt < 3; attempt++) {
            p->client->inventory[ammo] = 0;
            p->client->weapon = weapon;
            p->client->newweapon = NULL;
            p->client->ammo_index = ammo;
            p->client->weaponstate = cases[i].state;
#ifdef HARNESS_OPENTDM
            p->client->ps.gunframe = cases[i].gunframe;
#else
            p->client->weaponframe = cases[i].gunframe;
#endif
            if (attempt == 1) {
                /* Probe the exact pre-deadline world step between weapon steps. */
                p->client->buttons = BUTTON_ATTACK;
                weapon->weaponthink(p);
                p->client->buttons = 0;
            } else {
                h_think(p, BUTTON_ATTACK, 10);
                h_think(p, 0, 10);
            }
            int clicks = h_count_sounds(sound, sounds);
            CHECK(clicks == 1 + (attempt == 2), "%s attempt %d hears %d no-ammo sounds", weapon->pickup_name, attempt, clicks);
            CHECK(!h_count_msgs(H_SVC_MUZZLEFLASH, -1, msgs) && p->client->inventory[ammo] == 0,
                  "%s does not fire or consume ammo when empty", weapon->pickup_name);
            if (!attempt) h_frames_n(h_rate - 1);
            if (attempt == 1) {
                if (h_div == 1) p->client->weapon_thunk = true;
                h_frame();
            }
        }
        h_t23_sample(va("noammo_%d", i), h_frames - start, h_count_sounds(sound, sounds));
        h_frames_n(2 * h_rate);
    }
}

static void sc_t23_map_animations(void)
{
    static const struct { const char *name; int delay, period; } animations[] = {
        { "misc_blackhole", 2, 19 },
        { "misc_eastertank", 2, 39 },
        { "misc_easterchick", 2, 39 },
        { "misc_easterchick2", 2, 39 },
        { "misc_banner", 1, 16 },
        { "misc_satellite_dish", 1, 0 },
    };
    enum { count = sizeof(animations) / sizeof(animations[0]) };
    h_scene_base();
    for (int i = 0; i < count; i++)
        h_ent("{\n\"classname\" \"%s\"\n\"origin\" \"%d 1000 40\"\n}\n", animations[i].name, 160 * i);
    h_start(h_ents);
    edict_t *ent[count];
    int initial[count], changes[count] = { 0 };
    for (int i = 0; i < count; i++) {
        ent[i] = h_by_class(animations[i].name, 0);
        if (!ent[i]) imp_error("missing %s", animations[i].name);
        initial[i] = ent[i]->s.frame;
    }
    ent[count - 1]->use(ent[count - 1], world, world);
    int valid = 1;
    for (int tick = 0; tick <= 42 * h_div; tick++) {
        int before[count];
        for (int i = 0; i < count; i++) before[i] = ent[i]->s.frame;
        h_frame();
        for (int i = 0; i < count; i++) {
            int advance = ent[i]->s.frame != before[i];
            int n = tick / h_div - animations[i].delay + 1;
            int period = animations[i].period;
            int wanted = n > 0 && !(tick % h_div) && (period || n <= 38);
            if (advance != wanted) valid = 0;
            if (!advance) continue;
            changes[i]++;
            int frame = period == 16 ? (ent[i]->s.frame - initial[i] + 16) % 16 :
                        ent[i]->s.frame - initial[i];
            if (frame != (period ? n % period : n)) valid = 0;
            if (n == 1 || n == period || n == 38)
                h_t23_sample(va("%s_%d", animations[i].name, n), tick, frame);
        }
    }
    CHECK(valid && changes[count - 1] == 38, "map model frames advance, loop or stop at their scheduled times");
}

static void h_t23_gib_lifetimes(const char *const *map_gibs, unsigned seed)
{
    int count = map_gibs ? 6 : 3;
    int gone[6] = { 0 }, lifetime[6] = { 0, 0, 0, 30 * h_rate, 30 * h_rate, 30 * h_rate };
    h_t23_seed(seed);
    // Each lifetime draw follows the position, velocity and spin draws.
    const int prior_draws[] = { 9, 4, 6 };
#ifdef HARNESS_OPENTDM
    const int spawned = 3;
#else
    const int spawned = 2; /* OpenFFA has no ThrowDebris. */
    if (!map_gibs) count = spawned;
#endif
    for (int i = 0; i < spawned; i++) {
        for (int draw = 0; draw < prior_draws[i]; draw++) {
#ifdef HARNESS_OPENTDM
            genrand_int32();
#else
            Q_rand();
#endif
        }
        int base = i < 2 ? 10 : 5;
        lifetime[i] = (int)((base + base * random()) * h_rate);
    }
    edict_t *head = G_Spawn();
    VectorSet(head->s.origin, 0, 1000, 100);
    h_t23_seed(seed);
#ifdef HARNESS_OPENTDM
    ThrowGib(head, "models/objects/gibs/sm_meat/tris.md2", 40, GIB_ORGANIC);
    ThrowHead(head, "models/objects/gibs/skull/tris.md2", 40, GIB_ORGANIC);
    ThrowDebris(head, "models/objects/debris1/tris.md2", 1, head->s.origin);
    edict_t *ent[6] = { h_by_class("noclass", 1), head, h_by_class("debris", 0) };
#else
    ThrowGib(head, imp_modelindex("models/objects/gibs/sm_meat/tris.md2"), 40, GIB_ORGANIC);
    ThrowHead(head, imp_modelindex("models/objects/gibs/skull/tris.md2"), 40, GIB_ORGANIC);
    edict_t *ent[6] = { h_by_class("noclass", 1), head };
#endif
    for (int i = 3; i < count; i++) ent[i] = h_by_class(map_gibs[i - 3], 0);
    for (int i = 0; i < count; i++) {
        if (i >= spawned && i < 3) continue;
        if (!ent[i] || ent[i]->think != G_FreeEdict) imp_error("missing gib/debris fixture %d", i);
        ent[i]->movetype = MOVETYPE_NONE;
    }
    int start = h_frames;
    for (int tick = 0; tick < (map_gibs ? 31 : 21) * h_rate; tick++) {
        h_frame();
        for (int i = 0; i < count; i++)
            if (ent[i] && !gone[i] && !ent[i]->inuse) gone[i] = h_frames - start - 1;
    }
    for (int i = 0; i < count; i++) {
        if (!ent[i]) continue;
        CHECK(!ent[i]->inuse, "gib/debris %d is collected", i);
        CHECK(gone[i] == lifetime[i], "seed %u gib/debris %d frees at step %d (expected %d)",
              seed, i, gone[i], lifetime[i]);
        h_t23_sample(va("freed_%u_%d", seed, i), gone[i], !ent[i]->inuse);
    }
}

static void sc_t23_gib_lifetime(void)
{
    static const char *map_gibs[] = { "misc_gib_arm", "misc_gib_leg", "misc_gib_head" };
    h_scene_base();
    for (int i = 0; i < 3; i++)
        h_ent("{\n\"classname\" \"%s\"\n\"origin\" \"%d 1000 40\"\n}\n", map_gibs[i], 100 * i);
    h_start(h_ents);
    h_t23_gib_lifetimes(map_gibs, 73);
    h_t23_gib_lifetimes(NULL, 1234);
}

static void sc_t23_push_sound(void)
{
    h_scene_base();
    int model = h_add_inline(448, -64, 0, 576, 64, 128);
    h_ent("{\n\"classname\" \"trigger_push\"\n\"model\" \"*%d\"\n\"angle\" \"-1\"\n\"speed\" \"40\"\n}\n", model);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    int wind = imp_soundindex("misc/windfly.wav");
    for (int phase = 0; phase < h_div; phase++) {
        h_frames_n(2 * h_rate);
        h_real_phase(phase);
        int start = h_frames, mark = h_nsounds;
        int cutoff = start + h_to_grid() + 15 * h_div;
        h_place(p, 512, 0, 24);
        h_think(p, 0, 10);
        CHECK(p->velocity[2] == 400 && h_count_sounds(wind, mark) == 1,
              "jump pad launches with windfly on the contacting usercmd");
        h_think(p, 0, 10);
        h_frames_n(cutoff - h_div - h_frames);
        h_place(p, 512, 0, 24);
        h_think(p, 0, 10);
        CHECK(h_count_sounds(wind, mark) == 1, "jump pad is silent in window 14");
        h_frames_n(h_div);
        h_place(p, 512, 0, 24);
        h_think(p, 0, 10);
        CHECK(h_count_sounds(wind, mark) == 1, "jump pad is silent in window 15");
        h_frame();
        h_place(p, 512, 0, 24);
        h_think(p, 0, 10);
        CHECK(h_count_sounds(wind, mark) == 2, "jump pad plays again on the first contact in window 16");
        if (!phase)
            h_t23_sample("windfly", h_frames - start, h_count_sounds(wind, mark));
    }
}

static void sc_t23_edict_reuse(void)
{
    h_scene_base();
    h_start(h_ents);
    edict_t *source = G_Spawn(), *victim = G_Spawn();
    h_frames_n(2 * h_rate - 1);
    CHECK(G_CrushAllowed(source, victim) && !G_CrushAllowed(source, victim), "fixture has a pending crush cooldown");
    G_FreeEdict(source);
    edict_t *early = G_Spawn();
    CHECK(early == source && G_CrushAllowed(early, victim), "early reuse is immediate and clears the crush pair");
    h_t23_sample("initial_reuse", 0, early == source);
    h_frame();
    int start = h_frames;
    G_FreeEdict(early);
    edict_t *before = G_Spawn();
    CHECK(before != early, "later removal withholds its slot");
    h_frames_n(h_rate / 2);
    CHECK(G_Spawn() != early, "slot remains unavailable at exactly 0.5 s");
    h_frame();
    edict_t *after = G_Spawn();
    CHECK(after == early, "slot is available on the first step after 0.5 s");
    h_t23_sample("later_reuse", h_frames - start, after == early);
}

static void sc_t23_projectile_lifetime(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n}\n");
    h_ent("{\n\"classname\" \"target_blaster\"\n\"origin\" \"0 1000 100\"\n\"speed\" \"300\"\n}\n");
    h_start(h_ents);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);
    if (!h_t23_start(a, b)) return;
    edict_t *shooter = h_by_class("target_blaster", 0);
    if (!shooter) imp_error("target_blaster missing");
    vec3_t start = { 0, -1000, 100 }, aim = { 1, 0, 0 };
    fire_blaster(a, start, aim, 15, 300, EF_BLASTER, false);
    edict_t *bolt = h_by_class("bolt", 0);
    shooter->use(shooter, a, a);
    edict_t *map_bolt = h_by_class("bolt", 1);
    start[1] = 2000;
    fire_rocket(a, start, aim, 20, 2000, 20, 30);
    edict_t *rocket = h_by_class("rocket", 0);
    if (!bolt || !map_bolt || !rocket) imp_error("projectile did not spawn");
    int window = h_frames + h_grid_wait();
    int frozen = 0;
#ifdef HARNESS_OPENTDM
    h_frames_n(h_rate);
    int paused = h_frames + (h_div - level.realframenum % h_div) % h_div;
    h_client_command(a, "time");
    h_frames_n(5 * h_rate);
    CHECK(tdm_match_status == MM_TIMEOUT && bolt->inuse && map_bolt->inuse && rocket->inuse, "all lifetimes freeze in pause");
    h_client_command(a, "time");
    h_t23_wait_resume();
    frozen = h_frames - paused - 1;
#endif
    h_frames_n(window + frozen + 2 * h_rate - h_frames);
    /* The match clock stops during OpenTDM timeouts; the world grid keeps running. */
    window += h_grid_wait();
    h_frames_n(h_grid_wait());
    CHECK(bolt->inuse && map_bolt->inuse, "both bolts exist one step before two seconds");
    h_frame();
    CHECK(!bolt->inuse && !map_bolt->inuse && rocket->inuse, "two bolts expire after two match seconds");
    h_t23_sample("bolts", h_frames - window - frozen - 1, !bolt->inuse + !map_bolt->inuse);
    h_frames_n(2 * h_rate - 1);
    CHECK(rocket->inuse, "rocket exists one step before four seconds");
    h_frame();
    CHECK(!rocket->inuse, "rocket expires after 8000 units/speed in match time");
    h_t23_sample("rocket", h_frames - window - frozen - 1, !rocket->inuse);
}

static void sc_t23_bfg_effects(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"-300 0 24\"\n}\n");
    h_add_world(90, -512, 0, 128, 512, 512);
    h_start(h_ents);
    edict_t *owner = h_join(0, "p1"), *target = h_join(1, "p2");
    VectorSet(owner->s.origin, -300, 0, 24);
    imp_linkentity(owner);
    VectorSet(target->s.origin, -20, 150, 24);
    imp_linkentity(target);
    int mark = h_nmsgs;
    hbfg_t shot = h_bfg_shot(owner, target);
    edict_t *orb = h_by_class("bfg blast", 0);
    CHECK(orb && orb->s.frame == 1 && shot.lasers == 2 && shot.boom == 118, "wall impact: two lasers and one 118-point secondary hit");
    if (!orb || shot.explode < 0) return;
    h_t23_sample("lasers", shot.touch, shot.lasers);
    h_t23_sample("explosion", shot.explode - shot.touch, shot.boom);
    int first = h_frames - 1;
    for (int phase = 2; phase <= 5; phase++) {
        h_frames_n(h_div - 1);
        CHECK(orb->inuse && orb->s.frame == phase - 1, "BFG holds sprite frame %d until next window", phase - 1);
        h_frame();
        CHECK(orb->inuse && orb->s.frame == phase, "BFG sprite advances to %d", phase);
        h_t23_sample(va("sprite_%d", phase), (phase - 1) * h_div, orb->s.frame);
    }
    h_frames_n(h_div - 1);
    CHECK(orb->inuse && orb->s.frame == 5, "BFG holds its final sprite until the removal step");
    int removed = h_frames;
    h_frame();
    int hits = h_count_msgs(H_SVC_TEMP_ENTITY, TE_BFG_EXPLOSION, mark);
    CHECK(!orb->inuse && hits == 1, "BFG sprite is removed and no secondary hit repeats");
    h_t23_sample("removed", removed - first, hits);
}

static struct {
    int count, frames[16], values[16];
} h_t23_events[8];

static void h_t23_target_use(edict_t *self, edict_t *other, edict_t *activator)
{
    (void)other;
    (void)activator;
    int i = self->count;
    int n = h_t23_events[i].count++;
    if (n >= 16) imp_error("too many target activations");
    h_t23_events[i].frames[n] = h_frames;
    h_t23_events[i].values[n] = self->message ? atoi(self->message) : 0;
    if (i == 7) {
        int hours, minutes, seconds;
        if (!self->message || sscanf(self->message, "%d:%d:%d", &hours, &minutes, &seconds) != 3)
            imp_error("missing formatted count-up clock");
        h_t23_events[i].values[n] = (hours * 60 + minutes) * 60 + seconds;
    }
}

static void h_t23_target_event(int slot, int event, const char *name, int start, int delay, int value)
{
    int at = h_t23_events[slot].frames[event];
    CHECK(h_t23_events[slot].count > event && at - start == delay &&
          h_t23_events[slot].values[event] == value, "%s at %d ticks, value %d", name, at - start,
          h_t23_events[slot].values[event]);
    h_t23_sample(name, at - start, h_t23_events[slot].values[event]);
}

static void h_t23_map_timing_targets(edict_t *p)
{
    edict_t *clock = h_by_class("func_clock", 0);
    edict_t *timer = h_by_class("func_timer", 0), *jitter = h_by_class("func_timer", 1);
    edict_t *multiple = h_by_class("trigger_multiple", 0), *explosion = h_by_class("target_explosion", 0);
    if (!clock || !timer || !jitter || !multiple || !explosion) imp_error("map timing fixture did not spawn");
    h_real_phase(0);
    int start = h_frames;
    timer->use(timer, p, p);
    clock->use(clock, p, p);
    multiple->use(multiple, p, p);
    multiple->use(multiple, p, p);
    int mark = h_nmsgs;
    explosion->use(explosion, p, p);
    h_frames_n(h_rate / 2);
    CHECK(h_t23_events[4].count == 1 && !h_count_msgs(H_SVC_TEMP_ENTITY, TE_EXPLOSION1, mark),
          "multiple and explosion wait through their deadline");
    h_frame();
    multiple->use(multiple, p, p);
    int explosions = h_count_msgs(H_SVC_TEMP_ENTITY, TE_EXPLOSION1, mark);
    CHECK(explosions == 1 && h_t23_events[4].count == 2, "one explosion and one repeat activation after wait");
    h_t23_sample("explosion", h_frames - start - 1, explosions);
    h_t23_target_event(4, 1, "multiple", start, h_rate / 2 + 1, 0);
    h_frames_n(start + 16 * h_rate / 10 + 1 - h_frames);
    h_t23_target_event(5, 0, "timer_first", start, 6 * h_rate / 10, 0);
    h_t23_target_event(5, 1, "timer_repeat", start, 16 * h_rate / 10, 0);
    timer->use(timer, p, p);
    h_frames_n(start + 2 * h_rate + 1 - h_frames);
    h_t23_target_event(2, 0, "clock_2", start, 0, 2);
    h_t23_target_event(2, 1, "clock_1", start, h_rate, 1);
    h_t23_target_event(2, 2, "clock_0", start, 2 * h_rate, 0);
    h_t23_target_event(3, 0, "clock_done", start, 2 * h_rate, 0);
    start = h_frames;
    clock->use(clock, p, p);
    h_t23_target_event(2, 3, "clock_restart", start, 0, 2);

    CHECK(fabsf(jitter->random - (0.2f - 0.1f)) < H_EPS, "random equal to wait is clamped one tick below wait");
    h_t23_seed(0);
    jitter->use(jitter, p, p);
    int n = h_t23_events[5].count;
    for (int i = 0; h_t23_events[5].count == n && i < h_rate; i++) h_frame();
    CHECK(h_t23_events[5].count == n + 1, "seeded timer repeats exactly once before switch-off");
    h_t23_sample("random_repeat", h_frames - start - 1, h_t23_events[5].count - n);
    jitter->use(jitter, p, p);
    h_frames_n(2 * h_rate);
    CHECK(h_t23_events[5].count == n + 1, "switched-off timers stop activating targets");
}

static void sc_t23_map_timers(void)
{
    h_scene_base();
    int model = h_add_inline(1500, -48, 0, 1600, 48, 64);
    h_ent("{\n\"classname\" \"trigger_multiple\"\n\"model\" \"*%d\"\n\"wait\" \"0.5\"\n\"target\" \"multiple\"\n}\n", model);
    h_ent("{\n\"classname\" \"trigger_always\"\n\"target\" \"always\"\n}\n");
    h_ent("{\n\"classname\" \"target_crosslevel_trigger\"\n\"spawnflags\" \"1\"\n}\n");
    h_ent("{\n\"classname\" \"target_crosslevel_target\"\n\"spawnflags\" \"1\"\n\"target\" \"crosslevel\"\n}\n");
    h_ent("{\n\"classname\" \"func_clock\"\n\"target\" \"display\"\n\"pathtarget\" \"done\"\n\"spawnflags\" \"14\"\n\"count\" \"2\"\n}\n");
    h_ent("{\n\"classname\" \"func_timer\"\n\"target\" \"timer\"\n\"delay\" \"0.3\"\n\"wait\" \"1\"\n}\n");
    h_ent("{\n\"classname\" \"func_timer\"\n\"target\" \"timer\"\n\"wait\" \"0.2\"\n\"random\" \"0.2\"\n}\n");
    h_ent("{\n\"classname\" \"func_timer\"\n\"target\" \"auto\"\n\"spawnflags\" \"1\"\n\"pausetime\" \"0.4\"\n\"wait\" \"0.6\"\n}\n");
    h_ent("{\n\"classname\" \"func_clock\"\n\"target\" \"countup\"\n\"spawnflags\" \"1\"\n\"style\" \"2\"\n\"count\" \"61\"\n}\n");
    h_ent("{\n\"classname\" \"target_explosion\"\n\"origin\" \"1500 1000 24\"\n\"delay\" \"0.5\"\n}\n");
    h_ent("{\n\"classname\" \"target_laser\"\n\"origin\" \"200 0 24\"\n\"angle\" \"180\"\n\"spawnflags\" \"1\"\n}\n");
    h_ent("{\n\"classname\" \"target_earthquake\"\n\"targetname\" \"quake\"\n\"count\" \"1\"\n}\n");
    h_start(h_ents);
    static char *names[] = { "always", "crosslevel", "display", "done", "multiple", "timer", "auto", "countup" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        edict_t *sink = G_Spawn();
        sink->targetname = names[i];
        sink->count = i;
        sink->use = h_t23_target_use;
    }
    edict_t *cross = h_by_class("target_crosslevel_trigger", 0);
    edict_t *laser = h_by_class("target_laser", 0), *quake = h_by_class("target_earthquake", 0);
    edict_t *countup = h_by_class("func_clock", 1), *autotimer = h_by_class("func_timer", 2);
    if (!cross || !laser || !quake || !countup || !autotimer) imp_error("map timer fixture missing");
    countup->health = 59;
    cross->use(cross, world, world);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate - h_frames);
    CHECK(laser->use == NULL, "laser still uninitialized before one second");
    int hp = h_hp_now(p);
    for (int i = 0; i < h_rate; i++) {
        int before = h_hp_now(p);
        h_frame();
        CHECK(before - h_hp_now(p) == (i % h_div == 0), "laser damage at step %d", i);
    }
    CHECK(laser->use != NULL && h_hp_now(p) == hp - 10, "laser starts at one second and hits ten times a second");
    h_t23_sample("laser_damage", h_rate, hp - h_hp_now(p));
    laser->use(laser, p, p);
    h_frame();
    h_t23_target_event(0, 0, "always", 0, 2 * h_div, 0);
    h_t23_target_event(1, 0, "crosslevel", 0, 2 * h_rate, 0);
    h_t23_map_timing_targets(p);
    h_t23_target_event(6, 0, "timer_auto", 0, 2 * h_rate, 0);
    h_t23_target_event(6, 1, "timer_auto_repeat", 0, 26 * h_rate / 10, 0);
    autotimer->use(autotimer, p, p);
    for (int i = 0; i < 3; i++)
        h_t23_target_event(7, i, va("count_up_%d", i), 0, (i + 1) * h_rate, 59 + i);

    int start = h_frames, mark = h_nsounds, quake_sounds = imp_soundindex("world/quake.wav"), moves = 0;
    quake->use(quake, p, p);
    h_t23_seed(23);
    for (int i = 0; i <= h_rate; i++) {
        p->groundentity = world;
        p->groundentity_linkcount = world->linkcount;
        VectorClear(p->velocity);
        h_frame();
        CHECK((p->velocity[2] > 0) == (i > 0 && i % h_div == 0), "quake impulse at step %d", i);
        if (p->velocity[2] > 0) moves++;
    }
    int sounds = h_sounds_in(quake, quake_sounds, start, h_frames);
    CHECK(moves == 10 && sounds == 2, "one-second quake has ten impulses and two sounds");
    h_t23_sample("quake", h_rate, moves);
    int n = 0;
    for (int i = mark; i < h_nsounds; i++) {
        if (h_sounds_log[i].soundindex != quake_sounds) continue;
        int tick = h_sounds_log[i].frame - start;
        CHECK(tick == (1 + 6 * n) * h_div, "quake sound %d at step %d", n, tick);
        h_t23_sample(va("quake_sound_%d", n++), tick, 1);
    }
    p->groundentity = world;
    VectorClear(p->velocity);
    h_frames_n(h_div);
    CHECK(!p->velocity[2], "quake stops after one second");
}

static void sc_t23_viper_pitch(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"misc_viper_bomb\"\n\"origin\" \"0 0 3000\"\n}\n");
    h_start(h_ents);
    edict_t *bomb = h_by_class("misc_viper_bomb", 0), *viper = G_Spawn();
    if (!bomb || !bomb->use) imp_error("viper bomb missing");
    viper->classname = "misc_viper";
    VectorSet(viper->moveinfo.dir, 1, 0, 0);
    viper->moveinfo.speed = 100;
    h_frames_n(2 * h_rate);
    bomb->use(bomb, world, world);
    static const int phases[] = { 0, 2, 5, 8, 10, 12 };
    int start = h_frames;
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        int at = phases[i] * h_div;
        h_frames_n(start + at - h_frames);
        h_frame();
        double drop = phases[i] * 0.1;
        int want = drop >= 1 ? -270 : -(int)(atan2(-drop, 1 - drop) * 180 / M_PI);
        if (want > 0) want -= 360;
        CHECK(bomb->inuse && fabsf(bomb->s.angles[PITCH] - want) <= 0.01f,
              "viper pitch at %d tenths is %.2f (expected %d)", phases[i], bomb->s.angles[PITCH], want);
        h_t23_sample(va("pitch_%d", phases[i]), at, lroundf(bomb->s.angles[PITCH] * 100));
    }
}

static void h_t23_centerprint(edict_t *p, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    h_log_long(NUM(p), -1, text);
}

static int h_t23_mover_segment(edict_t *e, const char *tag, const vec3_t end, int angular)
{
    float *v = angular ? e->avelocity : e->velocity;
    for (int i = 0; H_VZERO(v) && i < 6 * h_rate; i++) h_frame();
    int start = h_frames;
    CHECK(!H_VZERO(v), "%s starts moving", tag);
    for (int i = 0; !H_VZERO(v) && i < 4 * h_rate; i++) h_frame();
    float *pos = angular ? e->s.angles : e->s.origin;
    vec3_t error;
    VectorSubtract(pos, end, error);
    CHECK(H_VZERO(v) && VectorLength(error) <= (angular ? H_EPS : 0.125f),
          "%s stops at endpoint (error %.4f)", tag, VectorLength(error));
    h_t23_sample(tag, h_frames - start, lroundf(VectorLength(pos)));
    return start;
}

static void h_t23_mover_wait(const char *event, int arrived, int departed, int delay)
{
    int elapsed = departed - arrived;
    CHECK(abs(elapsed - delay) <= h_div, "%s waits %.3f s (expected %.3f)", event,
          (double)elapsed / h_rate, (double)delay / h_rate);
    h_t23_sample(event, elapsed, 1);
}

static void h_t23_locked_door(edict_t *door, edict_t *p)
{
    CHECK(door->touch != NULL, "targeted door has a locked message");
    if (!door->touch) return;
    int start = h_frames, text = h_ntexts, events = h_nsounds;
    door->touch(door, p, NULL, NULL);
    door->touch(door, p, NULL, NULL);
    h_frames_n(5 * h_rate - 1);
    door->touch(door, p, NULL, NULL);
    h_frame();
    door->touch(door, p, NULL, NULL);
    int sounds = h_count_sounds(imp_soundindex("misc/talk1.wav"), events);
    const htext_t *first = h_text_to(p, -1, "locked fixture", text);
    const htext_t *second = first ? h_text_to(p, -1, "locked fixture", (int)(first - h_texts) + 1) : NULL;
    CHECK(first && second && first->frame == start && second->frame == h_frames && sounds == 2,
          "locked door prints and beeps only once per five seconds");
    h_t23_sample("locked_door", second ? second->frame - start : -1, sounds);
}

/* Use the RunFrame index of both sound and coordinate observations. */
static void h_t23_plat_cycle(edict_t *plat, edict_t *trigger, edict_t *p, int extend)
{
    int start = h_frames, mark = h_nsounds;
    trigger->touch(trigger, p, NULL, NULL);
    int top = -1, down = -1, bottom = -1, sound = -1;
    int top_touch = 25 * h_rate / 10;
    float upper = plat->pos1[2], lower = plat->pos2[2];
    for (int i = 0; i < 7 * h_rate && bottom < 0; i++) {
        float before = plat->s.origin[2];
        int tick = h_frames;
        if (extend && top >= 0 && tick - top - 1 == top_touch)
            trigger->touch(trigger, p, NULL, NULL);
        h_frame();
        float after = plat->s.origin[2];
        if (top < 0 && fabsf(after - upper) < H_EPS) top = tick;
        if (top < 0) continue;
        if (down < 0 && before - after >= 0.125f) down = tick;
        if (down >= 0 && fabsf(after - lower) < H_EPS) bottom = tick;
    }
    for (int i = mark; i < h_nsounds; i++)
        if (h_sounds_log[i].entnum == NUM(plat) && h_sounds_log[i].soundindex == plat->moveinfo.sound_start &&
            h_sounds_log[i].frame >= top) sound = h_sounds_log[i].frame;
    CHECK(top >= 0 && sound > top && down > sound && bottom > down,
          "plat cycle %d: from %d to top %d, sound %d, first down %d, bottom %d",
          extend, start, top, sound, down, bottom);
    int scheduled_wait = extend ? top_touch + h_rate : 3 * h_rate;
    CHECK(sound - top == scheduled_wait + extend,
          "platform sound follows the scheduled top wait (cycle %d)", extend);
    // Two steps from Move_Calc through Think_AccelMove to the next push, plus one from arrival to the touch clock.
    int dispatch_steps = 2 + extend;
    int wait_at_10hz = scheduled_wait / h_div + dispatch_steps;
    int expected_wait = wait_at_10hz * h_div - dispatch_steps * (h_div - 1);
    CHECK(down - top == expected_wait, "platform top wait (cycle %d): %d steps, expected %d",
          extend, down - top, expected_wait);
    int final_push = 0;
#ifndef HARNESS_OPENTDM
    /* OpenFFA's accelerated descent in this fixture reaches the endpoint a tick later at 40–60 Hz. */
    final_push = h_div >= 4;
#endif
    int travel = bottom - down + 1;
    CHECK(travel == 7 * h_div + final_push, "platform descent lasts %d steps (expected %d)",
          travel, 7 * h_div + final_push);
    CHECK(h_sounds_in(plat, plat->moveinfo.sound_end, start, h_frames) == 2,
          "platform reaches both endpoints once");
    h_t23_sample(va("plat_%d_top", extend), top - start, 1);
    h_t23_sample(va("plat_%d_sound", extend), sound - top, 1);
    h_t23_sample(va("plat_%d_wait", extend), down - top, 1);
    h_t23_sample(va("plat_%d_travel", extend), travel - final_push, 1);
}

static void sc_t23_movers_wait(void)
{
    h_scene_base();
    int plat_model = h_add_inline(-64, 936, 64, 64, 1064, 128);
    int button_model = h_add_inline(0, 1350, 0, 44, 1382, 64);
    int door_model = h_add_inline(0, 1650, 0, 108, 1714, 128);
    int rotating_model = h_add_inline(0, -16, -24, 80, 16, 40);
    int train_model = h_add_inline(128, 2000, 32, 176, 2048, 64);
    int secret_model = h_add_inline(0, 2500, 0, 50, 2550, 64);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"64\"\n\"speed\" \"180\"\n\"accel\" \"40\"\n\"decel\" \"60\"\n}\n", plat_model);
    h_ent("{\n\"classname\" \"func_button\"\n\"model\" \"*%d\"\n\"wait\" \"0.7\"\n}\n", button_model);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"speed\" \"50\"\n\"wait\" \"1.5\"\n\"targetname\" \"testdoor\"\n\"message\" \"locked fixture\"\n}\n", door_model);
    h_ent("{\n\"classname\" \"func_door_rotating\"\n\"model\" \"*%d\"\n\"origin\" \"0 3000 48\"\n\"targetname\" \"testrotating\"\n\"speed\" \"90\"\n\"wait\" \"1.5\"\n\"distance\" \"90\"\n}\n", rotating_model);
    h_ent("{\n\"classname\" \"func_train\"\n\"model\" \"*%d\"\n\"targetname\" \"testtrain\"\n\"target\" \"a\"\n}\n", train_model);
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"a\"\n\"target\" \"b\"\n\"origin\" \"128 2000 32\"\n}\n");
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"b\"\n\"target\" \"c\"\n\"origin\" \"228 2000 32\"\n\"wait\" \"1.5\"\n}\n");
    h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"c\"\n\"origin\" \"128 2000 32\"\n\"wait\" \"1.5\"\n}\n");
    h_ent("{\n\"classname\" \"func_door_secret\"\n\"model\" \"*%d\"\n\"wait\" \"1.5\"\n\"targetname\" \"testsecret\"\n}\n", secret_model);
    h_imports.centerprintf = h_t23_centerprint;
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    h_frames_n(h_rate);
    edict_t *plat = h_by_class("func_plat", 0), *button = h_by_class("func_button", 0);
    edict_t *door = h_by_class("func_door", 0), *rotate = h_by_class("func_door_rotating", 0);
    edict_t *train = h_by_class("func_train", 0), *secret = h_by_model(va("*%d", secret_model));
    if (!plat || !button || !door || !rotate || !train || !secret) imp_error("mover fixture did not spawn");
    edict_t *trigger = NULL;
    for (int i = 1; i < ge->num_edicts; i++) {
        edict_t *e = EDICT(i);
        if (e->inuse && e->enemy == plat && e->solid == SOLID_TRIGGER) trigger = e;
    }
    if (!trigger) imp_error("plat trigger missing");

    h_t23_locked_door(door, p);
    h_t23_plat_cycle(plat, trigger, p, 0);
    h_t23_plat_cycle(plat, trigger, p, 1);

    button->use(button, p, p);
    h_t23_mover_segment(button, "button_down", button->pos2, 0);
    int arrived = h_frames;
    int depart = h_t23_mover_segment(button, "button_up", button->pos1, 0);
    h_t23_mover_wait("button_wait", arrived, depart, 7 * h_div);

    door->use(door, p, p);
    h_t23_mover_segment(door, "door_open", door->pos2, 0);
    arrived = h_frames;
    h_frames_n(h_rate / 2);
    door->use(door, p, p);
    depart = h_t23_mover_segment(door, "door_close", door->pos1, 0);
    h_t23_mover_wait("door_wait", arrived, depart, 2 * h_rate);

    rotate->use(rotate, p, p);
    h_t23_mover_segment(rotate, "rotating_open", rotate->pos2, 1);
    arrived = h_frames;
    depart = h_t23_mover_segment(rotate, "rotating_close", rotate->pos1, 1);
    h_t23_mover_wait("rotating_wait", arrived, depart, 15 * h_rate / 10);

    vec3_t home, away;
    VectorCopy(train->s.origin, home);
    VectorCopy(home, away);
    away[0] += 100;
    train->use(train, p, p);
    h_t23_mover_segment(train, "train_out", away, 0);
    arrived = h_frames;
    depart = h_t23_mover_segment(train, "train_back", home, 0);
    h_t23_mover_wait("train_wait", arrived, depart, 15 * h_rate / 10);

    level.current_entity = p; /* ClientThink dispatches use outside the mover's own step. */
    secret->use(secret, p, p);
    h_t23_mover_segment(secret, "secret_recess", secret->pos1, 0);
    arrived = h_frames;
    depart = h_t23_mover_segment(secret, "secret_open", secret->pos2, 0);
    h_t23_mover_wait("secret_recess_wait", arrived, depart, h_rate);
    arrived = h_frames;
    depart = h_t23_mover_segment(secret, "secret_close", secret->pos1, 0);
    h_t23_mover_wait("secret_open_wait", arrived, depart, 15 * h_rate / 10);
    arrived = h_frames;
    depart = h_t23_mover_segment(secret, "secret_flush", vec3_origin, 0);
    h_t23_mover_wait("secret_close_wait", arrived, depart, h_rate);
}

#include "grid_clocks.h"
#include "timeout_movers.h"
#include "match_grid_rules.h"
#include "match_grid_timeout.h"
#include "match_timeout_toss.h"
#include "timeout_grenade.h"
#include "item_grid_edges.h"
#include "grid_regressions.h"

static void sc_unlag_default(void)
{
    h_scene_base();
    h_start(h_ents);
    cvar_t *mode = h_cvar_find("g_unlag");
    CHECK(mode && mode->value == 0, "g_unlag is registered and off by default");
}

static const struct { const char *name; void (*run)(void); } h_scenarios[] = {
    { "item_grid_dropped_quad_expiry", sc_item_grid_dropped_quad_expiry },
    { "item_grid_short_timer", sc_item_grid_short_timer },
#ifndef HARNESS_OPENTDM
    { "item_grid_spawn_protection", sc_item_grid_spawn_protection },
#endif
    { "match_kill_grid", sc_match_kill_grid },
    { "match_respawn_deadline", sc_match_respawn_deadline },
    { "match_vote_end_grid", sc_match_vote_end_grid },
    { "grid_durations", sc_grid_durations },
#ifdef HARNESS_OPENTDM
    { "match_overtime_vote_grid", sc_match_overtime_vote_grid },
    { "match_countdown_grid", sc_match_countdown_grid },
    { "match_timeout_grid", sc_match_timeout_grid },
    { "match_timeout_think_grid", sc_match_timeout_think_grid },
    { "match_timeout_reentry", sc_match_timeout_reentry },
    { "match_timeout_win_grid", sc_match_timeout_win_grid },
    { "match_timeout_restart_grid", sc_match_timeout_restart_grid },
    { "match_timeout_break_grid", sc_match_timeout_break_grid },
    { "match_timeout_toss", sc_match_timeout_toss },
    { "match_timeout_body_slot", sc_match_timeout_body_slot },
    { "timeout_grenade_fuse", sc_timeout_grenade_fuse },
    { "timeout_door_linear", sc_timeout_door_linear },
    { "timeout_door_accel", sc_timeout_door_accel },
    { "timeout_plat_linear", sc_timeout_plat_linear },
    { "timeout_plat_accel", sc_timeout_plat_accel },
    { "timeout_door_rotating", sc_timeout_door_rotating },
    { "timeout_train", sc_timeout_train },
    { "timeout_button_linear", sc_timeout_button_linear },
    { "timeout_button_accel", sc_timeout_button_accel },
    { "grid_exit_missing", sc_grid_exit_missing },
    { "grid_exit_same", sc_grid_exit_same },
#else
    { "grid_reset_command", sc_grid_reset_command },
    { "grid_reset_countdown", sc_grid_reset_countdown },
    { "grid_reset_missing", sc_grid_reset_missing },
    { "grid_reset_same", sc_grid_reset_same },
    { "grid_reset_phase_reader", sc_grid_reset_phase_reader },
    { "grid_reset_slow", sc_grid_reset_slow },
    { "grid_reset_map", sc_grid_reset_map },
    { "grid_reset_thresholds", sc_grid_reset_thresholds },
#endif
    { "smoke", sc_smoke },
    { "t01_rate_refuse", sc_t01_rate_refuse },
    { "t01_rate_zero", sc_t01_rate_zero },
    { "t04_cmd_schedule", sc_t04_cmd_schedule },
    { "t09_machinegun_held", sc_t09_machinegun_held },
    { "t09_hyperblaster_held", sc_t09_hyperblaster_held },
    { "t09_rocket_held", sc_t09_rocket_held },
    { "t09_rocket_tap", sc_t09_rocket_tap },
    { "w_timing_window", sc_w_timing_window },
    { "w_special_windowclock", sc_w_special_windowclock },
    { "w_second_short_tap", sc_w_second_short_tap },
#ifndef HARNESS_OPENTDM
    { "w_second_tap_while_firing", sc_w_second_tap_while_firing },
    { "w_respawn_latch", sc_w_respawn_latch },
#endif
    { "t09_rocket_tick_tap", sc_t09_rocket_tick_tap },
    { "t09_machinegun_tick_tap", sc_t09_machinegun_tick_tap },
    { "t09_hyperblaster_tick_tap", sc_t09_hyperblaster_tick_tap },
#ifndef HARNESS_OPENTDM
    { "t09_rocket_second_tap", sc_t09_rocket_second_tap },
    { "t09_machinegun_second_tap", sc_t09_machinegun_second_tap },
    { "t09_hyperblaster_second_tap", sc_t09_hyperblaster_second_tap },
#endif
    { "t09_machinegun_batch_taps", sc_t09_machinegun_batch_taps },
    { "t09_hyperblaster_batch_taps", sc_t09_hyperblaster_batch_taps },
    { "t09_rocket_tap_spam", sc_t09_rocket_tap_spam },
    { "t09_machinegun_tap_spam", sc_t09_machinegun_tap_spam },
    { "t09_machinegun_tap_observed", sc_t09_machinegun_tap_observed },
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
    { "t11_hurt_sound_grid", sc_t11_hurt_sound_grid },
    { "t11_hurt_toggle", sc_t11_hurt_toggle },
    { "t11_hurt_silent", sc_t11_hurt_silent },
    { "t11_hurt_god_plain", sc_t11_hurt_no_protection },
    { "t11_hurt_god_no_protection", sc_t11_hurt_no_protection_god },
    { "t11_hurt_shared", sc_t11_hurt_shared },
    { "t11_laser", sc_t11_laser },
    { "t11_knockback_zero_dir", sc_t11_knockback_zero_dir },
    { "t11_bfg_wall", sc_t11_bfg_wall },
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
    { "t13_viper_bomb_roll", sc_t13_viper_bomb_roll },
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
    { "t09_timeout_release_mg", sc_t09_timeout_release_mg },
    { "t10_timeout_release_grenade", sc_t10_timeout_release_grenade },
    { "t10_timeout_hold_grenade", sc_t10_timeout_hold_grenade },
    { "t01_fixed_engine", sc_t01_fixed_engine },
    { "t18_environment_resume", sc_t18_environment_resume },
    { "t17_ghost_ratelimit", sc_t17_ghost_ratelimit },
    { "t17_ping_handicap", sc_t17_ping_handicap },
    { "t18_flash_window", sc_t18_flash_window },
    { "t18_blend_batches", sc_t18_blend_batches },
    { "t18_gun_sway", sc_t18_gun_sway },
    { "t18_bob_air", sc_t18_bob_air },
    { "t18_lava_sound", sc_t18_lava_sound },
    { "t18_fall_pain", sc_t18_fall_pain },
    { "t18_drown_pain", sc_t18_drown_pain },
    { "t18_powerup_warn_timeout", sc_t18_powerup_warn_timeout },
    { "t18_kick_timeout", sc_t18_kick_timeout },
    /* OpenTDM only: ready, countdown, overtime and match settlement. */
    { "t23_match_phases", sc_t23_match_phases },
    { "t23_timeout_clocks", sc_t23_timeout_clocks }, /* Match clocks and duel reconnect during timeout. */
    { "t23_scoreboard_handicap", sc_t23_scoreboard_handicap }, /* Match scoreboard and ping handicap. */
    { "t23_cmd_penalties", sc_t23_cmd_penalties }, /* Team, match and vote commands. */
    { "t23_vote_window", sc_t23_vote_window }, /* OpenTDM ballots and match deadlines. */
#endif
    { "t23_flood", sc_t23_flood }, /* Box: Shared chat and gesture cooldowns. */
#ifdef HARNESS_OPENTDM
    { "t23_mute", sc_t23_mute }, /* OpenTDM per-player mute through timeout. */
#endif
    { "t23_inactivity", sc_t23_inactivity }, /* Box: Pregame MOTD and idle removal. */
    { "t23_item_respawn", sc_t23_item_respawn }, /* Box: Pickup, mega decay and item return. */
    { "t23_dropped_items", sc_t23_dropped_items }, /* Box: Owner lock, expiry and inherited quad. */
    { "t23_powerup_lifetime", sc_t23_powerup_lifetime }, /* Box: Powerup duration, HUD and damage. */
    { "t23_powerup_warning", sc_t23_powerup_warning }, /* Box: Warning and blink during active play. */
    { "t23_item_hud", sc_t23_item_hud }, /* Box: Pickup display, expiry and weapon drop. */
    { "t23_power_armor", sc_t23_power_armor }, /* Box: Cell consumption, effect and HUD. */
    { "t23_view_batches", sc_t23_view_batches }, /* Box: Damage, bob and kick during active play. */
    { "t23_lava_slime", sc_t23_lava_slime }, /* Box: Damage and sounds during active play. */
    { "t23_drowning", sc_t23_drowning }, /* Box: Air, drowning and rebreather. */
    { "t23_player_animation", sc_t23_player_animation }, /* Box: Running and gestures. */
    { "t23_respawn", sc_t23_respawn }, /* Box: Spawn selection, respawn and kill. */
    { "t23_weapon_pain", sc_t23_weapon_pain }, /* Box: Empty weapon debounce. */
    { "t23_map_animations", sc_t23_map_animations }, /* Box: Map model animation timing. */
    { "t23_gib_lifetime", sc_t23_gib_lifetime }, /* Box: Gib and head expiry. */
    { "t23_push_sound", sc_t23_push_sound }, /* Box: Jump pad sound debounce. */
    { "t23_edict_reuse", sc_t23_edict_reuse }, /* Box: Reuse delay and crush cleanup. */
    { "t23_projectile_lifetime", sc_t23_projectile_lifetime }, /* Box: Projectile expiry during active play. */
    { "t23_bfg_effects", sc_t23_bfg_effects }, /* Box: BFG hits, sprite and removal. */
    { "t23_map_timers", sc_t23_map_timers }, /* Box: Map timer and hazard timing. */
    { "t23_viper_pitch", sc_t23_viper_pitch }, /* Engine: Viper bomb pitch over time. */
    { "t23_movers_wait", sc_t23_movers_wait }, /* Engine: Mover waits and endpoints. */
    { "t24_plat_object", sc_t24_plat_object },
    { "t24_plat_object_linear", sc_t24_plat_object_linear },
#ifdef HARNESS_ENGINE
    { "t24_plat_real_debris", sc_t24_plat_real_debris },
#endif
    { "t24_plat_late_support", sc_t24_plat_late_support },
    { "t24_plat_fuse", sc_t24_plat_fuse },
    { "t24_grenade_windows", sc_t24_grenade_windows },
    { "t24_short_actions", sc_t24_short_actions },
#ifdef HARNESS_OPENTDM
    { "t24_timeout_blocked_rider", sc_t24_timeout_blocked_rider },
#endif
    { "t24_plat_zero_push", sc_t24_plat_zero_push },
#ifdef HARNESS_OPENTDM
    { "t24_door_final_push", sc_t24_door_final_push },
#endif
    { "t24_crush_overflow_count", sc_t24_crush_overflow_count },
#ifdef HARNESS_OPENTDM
    { "t24_view_timeout_fall", sc_t24_view_timeout_fall },
    { "t24_view_timeout_damage", sc_t24_view_timeout_damage },
    { "t24_view_no_timeout", sc_t24_view_no_timeout },
    { "t24_view_frozen", sc_t24_view_frozen },
#endif
    { "t24_toss_free", sc_t24_toss_free },
    { "t24_toss_bounce", sc_t24_toss_bounce },
    { "t24_toss_wall_toggle", sc_t24_toss_wall_toggle },
#ifdef HARNESS_ENGINE
    { "t24_toss_edge", sc_t24_toss_edge },
    { "t24_toss_outer_corner", sc_t24_toss_outer_corner },
    { "t24_toss_drop", sc_t24_toss_drop },
    { "t24_grid_solid", sc_t24_grid_solid },
#else
    { "t24_toss_head", sc_t24_toss_head },
    { "t24_toss_hold", sc_t24_toss_hold },
    { "t24_toss_land", sc_t24_toss_land },
    { "t24_toss_pad", sc_t24_toss_pad },
    { "t24_toss_projectile", sc_t24_toss_projectile },
    { "t24_grid_mapblaster", sc_t24_grid_mapblaster },
#ifdef HARNESS_OPENTDM
    { "t24_toss_timeout", sc_t24_toss_timeout },
#endif
#endif
#ifdef HARNESS_ENGINE
    { "unlag_fixture", sc_unlag_fixture },
    { "unlag_rail", sc_unlag_rail },
    { "unlag_no_extension", sc_unlag_no_extension },
    { "unlag_limits", sc_unlag_limits },
    { "unlag_delayed", sc_unlag_delayed },
    { "unlag_rail_row", sc_unlag_rail_row },
    { "unlag_rail_mixed_overlap", sc_unlag_rail_mixed_overlap },
    { "unlag_rail_history_overlap", sc_unlag_rail_history_overlap },
    { "unlag_high_slot", sc_unlag_high_slot },
    { "unlag_water", sc_unlag_water },
    { "unlag_shotgun_kill", sc_unlag_shotgun_kill },
    { "unlag_stationary_kill", sc_unlag_stationary_kill },
    { "unlag_history", sc_unlag_history },
    { "unlag_trace", sc_unlag_trace },
    { "unlag_lifetimes", sc_unlag_lifetimes },
    { "unlag_stance", sc_unlag_stance },
    { "unlag_held", sc_unlag_held },
    { "unlag_blockers", sc_unlag_blockers },
    { "unlag_bench", sc_unlag_bench },
    { "unlag_bench_dense", sc_unlag_bench_dense },
#ifdef HARNESS_OPENTDM
    { "unlag_timeout", sc_unlag_timeout },
    { "unlag_match_start", sc_unlag_match_start },
    { "unlag_teammate", sc_unlag_teammate },
    { "unlag_match_stats", sc_unlag_match_stats },
#endif
#endif
    { "unlag_default", sc_unlag_default },
    { NULL, NULL }
};
