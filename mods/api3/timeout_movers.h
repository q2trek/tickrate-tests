#ifdef HARNESS_OPENTDM
void Move_Begin(edict_t *ent);
void AngleMove_Begin(edict_t *ent);
void Think_AccelMove(edict_t *ent);

static const struct {
    const char *name, *classname, *keys, *path;
    int axis;
    void (*begin)(edict_t *);
} h_mover_probe_cases[] = {
    { "door-linear", "func_door", "\"angle\" \"0\"\n\"speed\" \"70\"\n\"wait\" \"-1\"\n",
      "Move_Begin", 0, Move_Begin },
    { "door-accel", "func_door", "\"angle\" \"0\"\n\"speed\" \"20\"\n\"accel\" \"10\"\n\"decel\" \"10\"\n\"wait\" \"-1\"\n",
      "Think_AccelMove", 0, Think_AccelMove },
    { "plat-linear", "func_plat", "\"height\" \"100\"\n\"speed\" \"1400\"\n\"accel\" \"1400\"\n\"decel\" \"1400\"\n",
      "Move_Begin", 2, Move_Begin },
    { "plat-accel", "func_plat", "\"height\" \"100\"\n\"speed\" \"400\"\n\"accel\" \"100\"\n\"decel\" \"100\"\n",
      "Think_AccelMove", 2, Think_AccelMove },
    { "rotate", "func_door_rotating", "\"origin\" \"0 3000 48\"\n\"speed\" \"90\"\n\"distance\" \"90\"\n\"wait\" \"-1\"\n",
      "AngleMove_Begin", YAW, AngleMove_Begin },
    { "train", "func_train", "\"targetname\" \"testtrain\"\n\"target\" \"a\"\n\"speed\" \"140\"\n",
      "Move_Begin", 0, Move_Begin },
    { "button-linear", "func_button", "\"angle\" \"0\"\n\"lip\" \"8\"\n\"speed\" \"140\"\n\"wait\" \"-1\"\n",
      "Move_Begin", 0, Move_Begin },
    { "button-accel", "func_button", "\"angle\" \"0\"\n\"lip\" \"8\"\n\"speed\" \"40\"\n\"accel\" \"10\"\n\"decel\" \"10\"\n\"wait\" \"-1\"\n",
      "Think_AccelMove", 0, Think_AccelMove },
};

typedef struct {
    vec3_t endpoint;
    int moved, arrived;
} h_mover_run_t;

static int h_mover_arrivals;
static void (*h_mover_endfunc)(edict_t *);

static void h_mover_arrived(edict_t *ent)
{
    h_mover_arrivals++;
    h_mover_endfunc(ent);
}

static void h_check_paused_ride(edict_t *rider, edict_t *plat, const vec3_t rider_at_time,
                                const vec3_t plat_at_time, int health, int direction)
{
    vec3_t rider_shift, plat_shift, error;
    VectorSubtract(rider->s.origin, rider_at_time, rider_shift);
    VectorSubtract(plat->s.origin, plat_at_time, plat_shift);
    VectorSubtract(rider_shift, plat_shift, error);
    CHECK(VectorLength(error) <= H_EPS,
          "frame %d paused rider shift (%.4f %.4f %.4f) equals platform shift (%.4f %.4f %.4f)",
          h_frames, rider_shift[0], rider_shift[1], rider_shift[2],
          plat_shift[0], plat_shift[1], plat_shift[2]);
    CHECK(rider->health == health && (meansOfDeath & ~MOD_FRIENDLY_FIRE) != MOD_CRUSH,
          "frame %d platform does no crush damage during timeout", h_frames);
    CHECK(plat->moveinfo.state == direction && plat->velocity[2] > 0,
          "frame %d platform does not reverse during timeout", h_frames);
}

static void h_mover_run(unsigned k, qboolean paused, h_mover_run_t *run)
{
    const char *name = h_mover_probe_cases[k].name;
    const char *classname = h_mover_probe_cases[k].classname;
    qboolean plat = !strcmp(classname, "func_plat"), train = !strcmp(classname, "func_train");
    qboolean angular = h_mover_probe_cases[k].begin == AngleMove_Begin;
    int axis = h_mover_probe_cases[k].axis, model;
    h_nworld = h_ninline = 0;
    h_scene_base();
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n\"angle\" \"0\"\n}\n");
    if (angular) model = h_add_inline(0, -16, -24, 80, 16, 40);
    else if (plat) model = h_add_inline(0, 1650, 128, 108, 1714, 192);
    else model = h_add_inline(0, 1650, 0, 108, 1714, 128);
    h_ent("{\n\"classname\" \"%s\"\n\"model\" \"*%d\"\n%s}\n", classname, model, h_mover_probe_cases[k].keys);
    if (train) {
        h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"a\"\n\"target\" \"b\"\n\"origin\" \"0 1650 0\"\n}\n");
        h_ent("{\n\"classname\" \"path_corner\"\n\"targetname\" \"b\"\n\"origin\" \"100.0625 1650 0\"\n\"wait\" \"-1\"\n}\n");
    }
    h_start(h_ents);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2");
    h_frames_n(3 * h_rate);
    if (!h_tdm_start(a, b)) return;
    edict_t *mover = h_by_class(classname, 0);
    if (!mover) imp_error("mover missing");
    level.current_entity = a;
    int use_frame = level.framenum;
    const float *pos = angular ? mover->s.angles : mover->s.origin;
    const float *velocity = angular ? mover->avelocity : mover->velocity;
    float start = pos[axis], last = start;
    float rider_height = 0;
    if (plat) {
        edict_t *trigger = NULL;
        for (int n = 1; n < ge->num_edicts; n++) {
            edict_t *e = EDICT(n);
            if (e->inuse && e->enemy == mover && e->solid == SOLID_TRIGGER) trigger = e;
        }
        if (!trigger || !trigger->touch) imp_error("platform trigger missing");
        VectorSet(b->s.origin, 54, 1682, mover->s.origin[2] + mover->maxs[2] - b->mins[2] + 0.125f);
        gi.linkentity(b);
        b->groundentity = mover;
        b->groundentity_linkcount = mover->linkcount;
        rider_height = b->s.origin[2] - mover->s.origin[2];
        trigger->touch(trigger, a, NULL, NULL);
    } else {
        mover->use(mover, a, a);
    }
    CHECK(mover->think == h_mover_probe_cases[k].begin, "%s entered %s", name, h_mover_probe_cases[k].path);


    h_mover_arrivals = 0;
    h_mover_endfunc = mover->moveinfo.endfunc;
    mover->moveinfo.endfunc = h_mover_arrived;
    int pause_frame = -1, moved = 0, arrived = -1, lost_support = 0;
    vec3_t rider_at_time, plat_at_time;
    int health = 0, direction = 0;
    qboolean carried = false;
    for (int i = 0; i < 20 * h_rate; i++) {
        if (paused && i == 5 * h_div) {
            CHECK(moved > 0 && !H_VZERO(velocity), "%s timeout caught active travel", name);
            if (plat) {
                VectorCopy(b->s.origin, rider_at_time);
                VectorCopy(mover->s.origin, plat_at_time);
                health = b->health;
                direction = mover->moveinfo.state;
                meansOfDeath = MOD_UNKNOWN;
            }
            h_client_command(a, "time");
            CHECK(tdm_match_status == MM_TIMEOUT, "%s entered timeout", name);
            pause_frame = h_frames;
        }
        if (pause_frame >= 0 && h_frames - pause_frame == h_rate) h_client_command(a, "time");
        qboolean check_ride = plat && pause_frame >= 0 && tdm_match_status == MM_TIMEOUT;
        if (check_ride) {
            usercmd_t cmd = { .msec = 1000 / h_rate, .forwardmove = 400 };
            h_pmove_self = b;
            ge->ClientThink(b, &cmd);
            h_check_paused_ride(b, mover, rider_at_time, plat_at_time, health, direction);
        }
        h_frame();
        if (check_ride) {
            h_check_paused_ride(b, mover, rider_at_time, plat_at_time, health, direction);
            carried |= mover->s.origin[2] > plat_at_time[2];
        }
        if (plat)
            lost_support += b->groundentity != mover || b->s.origin[2] - mover->s.origin[2] != rider_height;
        if (pos[axis] != last) moved++;
        last = pos[axis];
        if (moved && H_VZERO(velocity)) {
            arrived = level.framenum - use_frame;
            break;
        }
    }
    CHECK(arrived >= 0 && mover->moveinfo.state == H_STATE_TOP && tdm_match_status == MM_PLAYING,
          "%s reached STATE_TOP and finished travel", name);
    if (plat)
        CHECK(!lost_support, "%s rider stays on top of the platform (%d unsupported frames)", name, lost_support);
    if (plat && paused)
        CHECK(carried == (h_div > 1), "%s pre-freeze carry matches available world steps (carried %d)", name, carried);
    if (train)
        CHECK(fabsf(last - mover->moveinfo.end_origin[0]) >= 0.0625f,
              "%s endpoint is unreachable in steps of 1/8 unit", name);
    VectorCopy(pos, run->endpoint);
    h_frames_n(2 * h_rate);
    CHECK(VectorCompare(run->endpoint, pos) && mover->moveinfo.state == H_STATE_TOP &&
          h_mover_arrivals == 1, "%s settled for 2 s after one arrival", name);
    run->moved = moved;
    run->arrived = arrived;
    printf("MOVER %s %d %s %s endpoint=%.4f delta=%.4f moved=%d arrived=%d state=%d\n",
           name, h_rate, paused ? "timeout" : "control", h_mover_probe_cases[k].path,
           last, last - start, moved, arrived, mover->moveinfo.state);
}

static void h_mover_pair(unsigned k)
{
    h_mover_run_t control = { 0 }, timeout = { 0 };
    h_mover_run(k, false, &control);
    h_shutdown();
    h_mover_run(k, true, &timeout);
    vec3_t error;
    VectorSubtract(timeout.endpoint, control.endpoint, error);
    const char *name = h_mover_probe_cases[k].name;
    CHECK(VectorLength(error) <= H_EPS, "%s timeout endpoint equals control (error %.4f)",
          name, VectorLength(error));
    if (h_div == 1)
        CHECK(timeout.arrived == control.arrived + 1 && timeout.moved == control.moved,
              "%s completes the control travel one match frame later", name);
}

static void sc_timeout_door_linear(void) { h_mover_pair(0); }
static void sc_timeout_door_accel(void) { h_mover_pair(1); }
static void sc_timeout_plat_linear(void) { h_mover_pair(2); }
static void sc_timeout_plat_accel(void) { h_mover_pair(3); }
static void sc_timeout_door_rotating(void) { h_mover_pair(4); }
static void sc_timeout_train(void) { h_mover_pair(5); }
static void sc_timeout_button_linear(void) { h_mover_pair(6); }
static void sc_timeout_button_accel(void) { h_mover_pair(7); }
#endif
