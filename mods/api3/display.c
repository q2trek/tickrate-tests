/* Private game state for the listen-server projectile display scenarios.
 * The driver uses engine headers and must not see the mod's edict layout. */
#include "g_local.h"
#ifdef HARNESS_OPENTDM
#include "g_tdm.h"
#endif

static int world_frame(void)
{
#ifdef HARNESS_OPENTDM
    return level.realframenum;
#else
    return level.framenum;
#endif
}

edict_t *display_projectile(const float *from, const float *to, int age)
{
    edict_t *shot = G_Spawn();
    shot->classname = "display fixture";
    shot->s.modelindex = gi.modelindex("models/objects/rocket/tris.md2");
    shot->movetype = MOVETYPE_FLYMISSILE;
    shot->grid_move = true;
    shot->grid_frame = world_frame() - 1 - age;
    VectorCopy(from, shot->grid_from);
    VectorCopy(to, shot->s.origin);
    gi.linkentity(shot);
    if (!age)
        VectorCopy(from, shot->s.old_origin);
    return shot;
}

void display_phase(edict_t *shot, int age)
{
    shot->grid_frame = world_frame() - 1 - age;
}

void display_angles(edict_t *shot, float from, float to)
{
    shot->grid_from_angles[YAW] = from;
    shot->s.angles[YAW] = to;
}

static __typeof__(gi.trace) mover_trace;
static edict_t *mover_blocker, *partial_rider;
static int partial_traces;

#ifdef HARNESS_OPENTDM
static trace_t EXPORT display_trace(vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, edict_t *pass, int mask)
#else
static trace_t q_gameabi display_trace(const vec3_t start, const vec3_t mins, const vec3_t maxs,
                                      const vec3_t end, edict_t *pass, int mask)
#endif
{
    if (partial_rider && pass == partial_rider && ++partial_traces == 1)
        return (trace_t){ .startsolid = true, .ent = &g_edicts[0] };
    if (mover_blocker && pass == mover_blocker)
        return (trace_t){ .startsolid = true, .ent = &g_edicts[0] };
    if (pass && pass->grid_move) {
        trace_t tr = { .fraction = 1 };
        VectorCopy(end, tr.endpos);
        return tr;
    }
    return mover_trace(start, mins, maxs, end, pass, mask);
}

static bool display_push(edict_t *shot, bool rotate, bool rollback, bool partial)
{
    edict_t *mover = G_Spawn();
    mover->movetype = MOVETYPE_PUSH;
    mover->solid = SOLID_BBOX;
    VectorSet(mover->mins, -64, -64, -4);
    VectorSet(mover->maxs, 64, 64, 4);
    VectorCopy(shot->s.origin, mover->s.origin);
    mover->s.origin[0] -= 24;
    mover->s.origin[2] -= 8;
    mover->velocity[2] = 8 / FRAMETIME;
    if (rotate)
        mover->avelocity[YAW] = 90 / FRAMETIME;
    gi.linkentity(mover);
    shot->movetype = MOVETYPE_BOUNCE;
    shot->solid = SOLID_BBOX;
    gi.linkentity(shot);
    shot->groundentity = mover;

    if (rollback) {
        mover_blocker = G_Spawn();
        mover_blocker->movetype = MOVETYPE_TOSS;
        mover_blocker->solid = SOLID_BBOX;
        VectorCopy(shot->s.origin, mover_blocker->s.origin);
        gi.linkentity(mover_blocker);
        mover_blocker->groundentity = mover;
    }

    vec3_t initial;
    VectorCopy(mover->s.origin, initial);
    if (partial)
        partial_rider = shot;
    partial_traces = 0;
    mover_trace = gi.trace;
    gi.trace = display_trace;
    G_RunEntity(mover);
    gi.trace = mover_trace;
    if (mover_blocker) {
        G_FreeEdict(mover_blocker);
        mover_blocker = NULL;
    }
    partial_rider = NULL;
    return partial_traces == 2 && VectorCompare(initial, mover->s.origin) && mover->s.angles[YAW] == 0;
}

void display_move(edict_t *shot, bool rotate, bool rollback)
{
    display_push(shot, rotate, rollback, false);
}

bool display_partial_push(edict_t *shot, vec3_t from)
{
    bool rolled_back = display_push(shot, true, true, true);
    VectorCopy(shot->grid_from, from);
    return rolled_back;
}

#ifdef HARNESS_OPENTDM
void display_freeze(void)
{
    level.last_tdm_match_status = tdm_match_status;
    tdm_match_status = MM_TIMEOUT;
    level.timeout_end_framenum = level.realframenum + SERVER_FPS * 10;
}
#else
void display_freeze(void) {}
#endif

int display_grid_frame(void)
{
    return FRAMESYNC;
}

void display_flight_trace(bool enable)
{
    if (enable) {
        mover_trace = gi.trace;
        gi.trace = display_trace;
    } else {
        gi.trace = mover_trace;
    }
}

bool display_flight_trace_active(void)
{
    return gi.trace == display_trace;
}

edict_t *display_fire(edict_t *owner)
{
    vec3_t start, dir = { 1, 0, 0 };
    VectorCopy(owner->s.origin, start);
    start[2] += 32;
    display_flight_trace(true);
    fire_rocket(owner, start, dir, 0, 650, 0, 0);
    display_flight_trace(false);
    for (int i = 1; i < globals.num_edicts; i++) {
        edict_t *ent = g_edicts + i;
        if (ent->inuse && ent->owner == owner && ent->classname && !strcmp(ent->classname, "rocket"))
            return ent;
    }
    return NULL;
}

int display_age(const edict_t *shot)
{
    return shot->grid_frame < 0 ? -1 : world_frame() - 1 - shot->grid_frame;
}

void display_remove(edict_t *shot)
{
    G_FreeEdict(shot);
}

bool display_in_timeout(void)
{
#ifdef HARNESS_OPENTDM
    return tdm_match_status == MM_TIMEOUT;
#else
    return false;
#endif
}

void display_schedule_resume(edict_t *shot)
{
#ifdef HARNESS_OPENTDM
    level.timeout_end_framenum = 0;
    level.match_resume_framenum = level.realframenum + 1;
    shot->velocity[0] = 650;
#endif
}

void display_free_flight_frame(void)
{
    display_flight_trace(true);
    globals.RunFrame();
    display_flight_trace(false);
}
