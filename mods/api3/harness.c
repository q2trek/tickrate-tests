/*
 * API3 harness: drives the real, separately built OpenFFA/OpenTDM module
 * (GetGameAPI, SpawnEntities, ClientConnect/Begin/Command, ClientThink,
 * RunFrame). The engine side is replaced only at the import boundary:
 * controlled box geometry (world boxes + linked entity boxes) for trace,
 * BoxEdicts and Pmove contact, recorded sound/multicast output.
 *
 * HARNESS_ENGINE builds replace the box geometry with the engine collision
 * fixture (engine.c): a real BSP of the same boxes and the engine's own
 * trace, pointcontents, BoxEdicts, link/unlink, Pmove and inline bounds.
 *
 * Assertions use game consequences: player state stats (health, ammo),
 * muzzle flash / temp entity messages, sound calls, entity_state origins
 * and inuse. Never mod-private fields.
 *
 * Usage: harness <scenario> ; prints PASS/FAIL lines, exit 1 on failure.
 */
#include "g_local.h"
#ifdef HARNESS_OPENTDM
#include "g_tdm.h"
#endif
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HARNESS_MOD
#error "build with -DHARNESS_MOD=\"openffa\"|\"opentdm\""
#endif

#ifdef HARNESS_OPENTDM
#define H_MOD_RATE SERVER_FPS
#else
#define H_MOD_RATE HZ
#endif

#define H_SVC_MUZZLEFLASH 1
#define H_SVC_TEMP_ENTITY 3
#define H_SVC_LAYOUT 4
#define H_SVC_STUFFTEXT 11
#ifdef HARNESS_OPENTDM
_Static_assert(H_SVC_LAYOUT == SVC_LAYOUT && H_SVC_STUFFTEXT == SVC_STUFFTEXT, "svc numbers");
#else
_Static_assert(H_SVC_LAYOUT == svc_layout && H_SVC_STUFFTEXT == svc_stufftext, "svc numbers");
#endif

/* ------------------------------------------------------------------ */
/* failures */

static int h_failures;
static jmp_buf h_abort;
static char h_errmsg[1024];

#define CHECK(cond, ...) do { \
        if (cond) { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } \
        else { h_failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* ------------------------------------------------------------------ */
/* time base: the harness world rate (env HARNESS_RATE, default 10) */

static int h_rate = 10;
static int h_div = 1;           /* world ticks per 100 ms weapon/keyframe step */
static int h_frames;            /* RunFrame calls so far */

static double h_time(void) { return (double)h_frames / h_rate; }

/* client time in ms at the start of world tick k (floor): a usercmd before
 * tick k lasts h_tick_ms(k + 1) - h_tick_ms(k), 16/17 ms at 60 Hz, 1000 ms a second */
static int h_tick_ms(long long k, int per) { return (int)(k * 1000 / per); }

/* usercmd schedule of the path ticks (h_tick_sched; env HARNESS_SCHED):
 * tick: one command before every world tick; 30 / 10: the client's packets
 * per second (Q2PRO cl_maxpackets 30 by default; 10), one command per
 * packet, packet j sent at h_tick_ms(j, pps), read before the first tick that
 * starts after it; batch: a host that reads packets once per batch of
 * 5 world steps (variable FPS batches): at a tick k % 5 == 0 the commands of ticks
 * k-4..k, one per tick (a 5-tick command would pass the 250 ms the client
 * allows at 10 Hz), none before the other 4 ticks */
enum { H_SCHED_TICK, H_SCHED_30, H_SCHED_10, H_SCHED_BATCH };
static const char *const h_sched_names[] = { "tick", "30", "10", "batch" };
static int h_sched;
static int h_cmds, h_cmd_msec, h_cmd_min, h_cmd_max;   /* usercmds sent (h_think) */

/* ------------------------------------------------------------------ */
/* cvars */

typedef struct hcvar_s {
    cvar_t c;
    struct hcvar_s *next_h;
} hcvar_t;

static hcvar_t *h_cvars;

static const char *h_override(const char *name);

static void h_cvar_assign(cvar_t *c, const char *value)
{
    free(c->string);
    c->string = strdup(value);
    c->value = (float)atof(value);
    c->modified = 1;
}

static cvar_t *h_cvar_find(const char *name)
{
    for (hcvar_t *v = h_cvars; v; v = v->next_h)
        if (!strcmp(v->c.name, name))
            return &v->c;
    return NULL;
}

static cvar_t *imp_cvar(const char *name, const char *value, int flags)
{
    cvar_t *c = h_cvar_find(name);
    if (c) {
        c->flags |= flags;
        return c;
    }
    hcvar_t *v = calloc(1, sizeof(*v));
    v->c.name = strdup(name);
    v->c.flags = flags;
    const char *o = h_override(name);
    h_cvar_assign(&v->c, o ? o : (value ? value : ""));
    v->next_h = h_cvars;
    h_cvars = v;
    return &v->c;
}

static cvar_t *imp_cvar_set(const char *name, const char *value)
{
    cvar_t *c = h_cvar_find(name);
    if (!c)
        return imp_cvar(name, value, 0);
    h_cvar_assign(c, value);
    return c;
}

/* scenario cvar overrides (name=value pairs) */
static const char *h_overrides[64];
static int h_noverrides;

static void h_set_override(const char *name, const char *value)
{
    h_overrides[h_noverrides++] = name;
    h_overrides[h_noverrides++] = value;
}

static const char *h_override(const char *name)
{
    for (int i = 0; i < h_noverrides; i += 2)
        if (!strcmp(h_overrides[i], name))
            return h_overrides[i + 1];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* printing */

static int h_verbose;

/* broadcast prints and configstrings >= CS_GENERAL, with the frame they
 * were emitted in (consumer-visible HUD/status output) */
typedef struct { int frame, num; char text[128]; } hprint_t;
static hprint_t h_bprints[4096], h_cstrings[8192];
static int h_nbprints, h_ncstrings;

static void h_log_text(hprint_t *log, int *n, int cap, int num, const char *text)
{
    if (*n >= cap) return;
    hprint_t *e = &log[(*n)++];
    e->frame = h_frames;
    e->num = num;
    snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
}

static void imp_bprintf(int level, const char *fmt, ...)
{
    (void)level;
    char text[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    h_log_text(h_bprints, &h_nbprints, 4096, 0, text);
    if (h_verbose) printf("    [bprint] %s", text);
}
static void imp_dprintf(const char *fmt, ...)
{
    if (!h_verbose) return;
    va_list ap; va_start(ap, fmt); printf("    [dprint] "); vprintf(fmt, ap); va_end(ap);
}
/* client prints and stufftext/layout unicasts (the text a client gets),
 * with the frame and the receiving entity (-1: everyone) */
typedef struct { int frame, entnum, svc; char text[1400]; } htext_t;
static htext_t h_texts[512];
static int NUM(const edict_t *e);
static int h_ntexts;

static void h_log_long(int entnum, int svc, const char *text)
{
    if (h_ntexts >= 512) return;
    htext_t *e = &h_texts[h_ntexts++];
    e->frame = h_frames;
    e->entnum = entnum;
    e->svc = svc;
    snprintf(e->text, sizeof(e->text), "%s", text);
}

static void imp_cprintf(edict_t *ent, int level, const char *fmt, ...)
{
    (void)level;
    char text[1400];
    va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof(text), fmt, ap); va_end(ap);
    h_log_long(ent ? NUM(ent) : -1, 0, text);
    if (h_verbose) printf("    [cprint] %s", text);
}
static void imp_centerprintf(edict_t *ent, const char *fmt, ...)
{
    (void)ent;
    if (!h_verbose) return;
    va_list ap; va_start(ap, fmt); printf("    [center] "); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
static __attribute__((noreturn)) void imp_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(h_errmsg, sizeof(h_errmsg), fmt, ap); va_end(ap);
    longjmp(h_abort, 1);
}

/* ------------------------------------------------------------------ */
/* indexes and configstrings */

#define H_MAXNAMES 512
static char *h_models[H_MAXNAMES], *h_sounds[H_MAXNAMES], *h_images[H_MAXNAMES];

static int h_index(char **table, const char *name)
{
    if (!name || !*name) return 0;
    for (int i = 1; i < H_MAXNAMES; i++) {
        if (!table[i]) { table[i] = strdup(name); return i; }
        if (!strcmp(table[i], name)) return i;
    }
    imp_error("index table overflow");
}
static int imp_modelindex(const char *n) { return h_index(h_models, n); }
static int imp_soundindex(const char *n) { return h_index(h_sounds, n); }
static int imp_imageindex(const char *n) { return h_index(h_images, n); }
static int h_find(char **table, const char *name)
{
    for (int i = 1; i < H_MAXNAMES && table[i]; i++)
        if (!strcmp(table[i], name)) return i;
    return -1;
}

static void imp_configstring(int num, const char *s)
{
    if (num >= CS_GENERAL) h_log_text(h_cstrings, &h_ncstrings, 8192, num, s);
}

/* ------------------------------------------------------------------ */
/* edicts */

static game_export_t *ge;

static edict_t *EDICT(int n) { return (edict_t *)((byte *)ge->edicts + ge->edict_size * n); }
static int NUM(const edict_t *e) { return (int)(((const byte *)e - (const byte *)ge->edicts) / ge->edict_size); }

static int h_linked[MAX_EDICTS];

/* inline model bounds ("*N"), set by the scenario before SpawnEntities */
typedef struct { vec3_t mins, maxs; } hbox_t;
static hbox_t h_inline[64];
static int h_ninline;

/* world solid boxes; entity 0 is the world */
static hbox_t h_world[32];
static int h_nworld;

static void h_add_world(float x0, float y0, float z0, float x1, float y1, float z1)
{
    hbox_t *b = &h_world[h_nworld++];
    VectorSet(b->mins, x0, y0, z0);
    VectorSet(b->maxs, x1, y1, z1);
}

static int h_add_inline(float x0, float y0, float z0, float x1, float y1, float z1)
{
    hbox_t *b = &h_inline[++h_ninline];
    VectorSet(b->mins, x0, y0, z0);
    VectorSet(b->maxs, x1, y1, z1);
    return h_ninline;
}

#ifdef HARNESS_ENGINE
void he_init(void);
void he_load_map(const float *boxes, int nboxes, const float *inl, int ninline);
void he_set_game(void *exports);
void he_inline_bounds(const char *name, float *mins, float *maxs);
trace_t he_trace(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, edict_t *pass, int mask);
int he_pointcontents(const vec3_t p);
int he_boxedicts(const vec3_t mins, const vec3_t maxs, edict_t **list, int maxcount, int areatype);
void he_link(edict_t *ent);
void he_unlink(edict_t *ent);
void he_pmove(void *pm);
#endif

static void imp_linkentity(edict_t *ent)
{
#ifdef HARNESS_ENGINE
    he_link(ent);
    return;
#endif
    int n = NUM(ent);
    if (n == 0) { h_linked[0] = 1; return; }
    /* same bounds rules as the engine: rotated BSP uses radius */
    if (ent->solid == SOLID_BSP && (ent->s.angles[0] || ent->s.angles[1] || ent->s.angles[2])) {
        float max = 0;
        for (int i = 0; i < 3; i++) {
            float v = fabsf(ent->mins[i]); if (v > max) max = v;
            v = fabsf(ent->maxs[i]); if (v > max) max = v;
        }
        /* radius of the box corner */
        max = sqrtf(3.0f) * max;
        for (int i = 0; i < 3; i++) {
            ent->absmin[i] = ent->s.origin[i] - max;
            ent->absmax[i] = ent->s.origin[i] + max;
        }
    } else {
        VectorAdd(ent->s.origin, ent->mins, ent->absmin);
        VectorAdd(ent->s.origin, ent->maxs, ent->absmax);
    }
    for (int i = 0; i < 3; i++) { ent->absmin[i] -= 1; ent->absmax[i] += 1; }
    VectorSubtract(ent->maxs, ent->mins, ent->size);
    h_linked[n] = 1;
    ent->area.prev = ent->area.next = &ent->area;   /* "linked" marker read by SV_Push */
    ent->linkcount++;
    ent->areanum = 1;
}

static void imp_unlinkentity(edict_t *ent)
{
#ifdef HARNESS_ENGINE
    he_unlink(ent);
#endif
    h_linked[NUM(ent)] = 0;
    ent->area.prev = ent->area.next = NULL;
}

static void imp_setmodel(edict_t *ent, const char *name)
{
    if (!name) imp_error("setmodel: NULL");
    ent->s.modelindex = imp_modelindex(name);
    if (name[0] == '*') {
        int k = atoi(name + 1);
        if (k <= 0 || k > h_ninline) imp_error("setmodel: unknown inline model %s", name);
#ifdef HARNESS_ENGINE
        he_inline_bounds(name, ent->mins, ent->maxs);
#else
        VectorCopy(h_inline[k].mins, ent->mins);
        VectorCopy(h_inline[k].maxs, ent->maxs);
#endif
        imp_linkentity(ent);
    }
}

/* collision box of a linked entity (the linked bounds without the 1 unit slop) */
static void h_solid_box(edict_t *e, vec3_t mins, vec3_t maxs)
{
    for (int i = 0; i < 3; i++) { mins[i] = e->absmin[i] + 1; maxs[i] = e->absmax[i] - 1; }
}

static int h_entity_contents(edict_t *e)
{
    if (e->solid == SOLID_BSP) return CONTENTS_SOLID;
    if (e->svflags & SVF_DEADMONSTER) return CONTENTS_DEADMONSTER;
    return CONTENTS_MONSTER;
}

/* ------------------------------------------------------------------ */
/* trace: swept AABB against world boxes and linked solid entities */

static csurface_t h_nullsurf;

static void h_clip_box(trace_t *tr, const vec3_t start, const vec3_t end,
                       const vec3_t mins, const vec3_t maxs,
                       const vec3_t bmin, const vec3_t bmax, edict_t *hit, int contents)
{
    vec3_t lo, hi;
    for (int i = 0; i < 3; i++) { lo[i] = bmin[i] - maxs[i]; hi[i] = bmax[i] - mins[i]; }
    int inside_start = 1, inside_end = 1;
    for (int i = 0; i < 3; i++) {
        if (!(start[i] > lo[i] && start[i] < hi[i])) inside_start = 0;
        if (!(end[i] > lo[i] && end[i] < hi[i])) inside_end = 0;
    }
    if (inside_start) {
        tr->startsolid = 1;
        if (inside_end) { tr->allsolid = 1; tr->fraction = 0; tr->ent = hit; tr->contents = contents; }
        return;
    }
    /* slab entry */
    float tenter = -1, texit = 2; int axis = -1; float sign = 0;
    for (int i = 0; i < 3; i++) {
        float d = end[i] - start[i];
        if (d == 0) {
            if (start[i] <= lo[i] || start[i] >= hi[i]) return;
            continue;
        }
        float t0 = (lo[i] - start[i]) / d, t1 = (hi[i] - start[i]) / d;
        float s = -1;
        if (t0 > t1) { float t = t0; t0 = t1; t1 = t; s = 1; }
        if (t0 > tenter) { tenter = t0; axis = i; sign = s; }
        if (t1 < texit) texit = t1;
    }
    if (axis < 0 || tenter >= texit || tenter < 0 || tenter > 1) return;
    vec3_t delta; VectorSubtract(end, start, delta);
    float len = sqrtf(DotProduct(delta, delta));
    float f = tenter - (len > 0 ? 0.03125f / len : 0);
    if (f < 0) f = 0;
    if (f < tr->fraction) {
        tr->fraction = f;
        VectorClear(tr->plane.normal);
        tr->plane.normal[axis] = sign;
        tr->ent = hit;
        tr->contents = contents;
    }
}

static trace_t imp_trace(const vec3_t start, const vec3_t mins_in, const vec3_t maxs_in,
                         const vec3_t end, edict_t *passent, int mask)
{
#ifdef HARNESS_ENGINE
    return he_trace(start, mins_in, maxs_in, end, passent, mask);
#endif
    static const vec3_t zero = {0, 0, 0};
    const float *mins = mins_in ? mins_in : zero, *maxs = maxs_in ? maxs_in : zero;
    trace_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.fraction = 1;
    tr.surface = &h_nullsurf;
    if (mask & CONTENTS_SOLID)
        for (int i = 0; i < h_nworld; i++)
            h_clip_box(&tr, start, end, mins, maxs, h_world[i].mins, h_world[i].maxs, EDICT(0), CONTENTS_SOLID);
    for (int n = 1; n < ge->num_edicts; n++) {
        edict_t *e = EDICT(n);
        if (!e->inuse || !h_linked[n] || e == passent) continue;
        if (e->solid != SOLID_BSP && e->solid != SOLID_BBOX) continue;
        if (passent && (e->owner == passent || passent->owner == e)) continue;
        int contents = h_entity_contents(e);
        if (!(contents & mask)) continue;
        vec3_t bmin, bmax;
        h_solid_box(e, bmin, bmax);
        h_clip_box(&tr, start, end, mins, maxs, bmin, bmax, e, contents);
    }
    if (!tr.ent) tr.ent = EDICT(0);
    for (int i = 0; i < 3; i++) tr.endpos[i] = start[i] + tr.fraction * (end[i] - start[i]);
    return tr;
}

static int imp_pointcontents(const vec3_t p)
{
#ifdef HARNESS_ENGINE
    return he_pointcontents(p);
#endif
    for (int i = 0; i < h_nworld; i++) {
        const hbox_t *b = &h_world[i];
        if (p[0] > b->mins[0] && p[0] < b->maxs[0] && p[1] > b->mins[1] && p[1] < b->maxs[1] &&
            p[2] > b->mins[2] && p[2] < b->maxs[2])
            return CONTENTS_SOLID;
    }
    return 0;
}

static qboolean imp_inPVS(const vec3_t a, const vec3_t b) { (void)a; (void)b; return 1; }
static void imp_SetAreaPortalState(int n, qboolean open) { (void)n; (void)open; }
static qboolean imp_AreasConnected(int a, int b) { (void)a; (void)b; return 1; }

static int h_boxes_touch(const vec3_t amin, const vec3_t amax, const vec3_t bmin, const vec3_t bmax)
{
    for (int i = 0; i < 3; i++)
        if (amin[i] > bmax[i] || amax[i] < bmin[i]) return 0;
    return 1;
}

static int imp_BoxEdicts(const vec3_t mins, const vec3_t maxs, edict_t **list, int maxcount, int areatype)
{
#ifdef HARNESS_ENGINE
    return he_boxedicts(mins, maxs, list, maxcount, areatype);
#endif
    int count = 0;
    for (int n = 1; n < ge->num_edicts && count < maxcount; n++) {
        edict_t *e = EDICT(n);
        if (!e->inuse || !h_linked[n]) continue;
        int trig = e->solid == SOLID_TRIGGER;
        if (areatype == AREA_TRIGGERS ? !trig : (e->solid != SOLID_BBOX && e->solid != SOLID_BSP)) continue;
        if (!h_boxes_touch(mins, maxs, e->absmin, e->absmax)) continue;
        list[count++] = e;
    }
    return count;
}

/* Pmove: no movement (not the subject here); contact list comes from the
 * linked boxes touching the player box. Engine builds run the engine's
 * Pmove; a scripted placement is applied first (at rest, snapped out of
 * contact as the engine does after a teleport). */
static edict_t *h_pmove_self;
static edict_t *h_pm_move_ent;   /* scripted placement: next Pmove of this player ends at h_pm_to */
static vec3_t h_pm_to;

static void imp_Pmove(pmove_t *pm)
{
    static const vec3_t pmins = {-16, -16, -24}, pmaxs = {16, 16, 32};
    vec3_t origin, amin, amax;
    if (h_pm_move_ent && h_pm_move_ent == h_pmove_self) {
        for (int i = 0; i < 3; i++) pm->s.origin[i] = (short)(h_pm_to[i] * 8);
#ifdef HARNESS_ENGINE
        VectorClear(pm->s.velocity);
        pm->snapinitial = 1;        /* as after a game-side teleport */
#endif
        h_pm_move_ent = NULL;
    }
#ifdef HARNESS_ENGINE
    he_pmove(pm);
    return;
#endif
    for (int i = 0; i < 3; i++) {
        origin[i] = pm->s.origin[i] * 0.125f;
        pm->viewangles[i] = SHORT2ANGLE(pm->cmd.angles[i] + pm->s.delta_angles[i]);
    }
    VectorCopy(pmins, pm->mins);
    VectorCopy(pmaxs, pm->maxs);
    pm->viewheight = 22;
    pm->groundentity = EDICT(0);
    pm->s.pm_flags |= PMF_ON_GROUND;
    pm->waterlevel = 0;
    pm->watertype = 0;
    pm->numtouch = 0;
    for (int i = 0; i < 3; i++) { amin[i] = origin[i] + pmins[i] - 1; amax[i] = origin[i] + pmaxs[i] + 1; }
    for (int n = 1; n < ge->num_edicts && pm->numtouch < MAXTOUCH; n++) {
        edict_t *e = EDICT(n);
        if (!e->inuse || !h_linked[n] || e == h_pmove_self) continue;
        if (e->solid != SOLID_BSP && e->solid != SOLID_BBOX) continue;
        vec3_t bmin, bmax;
        h_solid_box(e, bmin, bmax);
        if (h_boxes_touch(amin, amax, bmin, bmax))
            pm->touchents[pm->numtouch++] = e;
    }
}

/* ------------------------------------------------------------------ */
/* messages and sounds */

typedef struct {
    int frame;          /* h_frames when emitted (0-based: during RunFrame #frame+1 or before it) */
    int svc, sub;       /* svc byte and the first sub-type byte (MZ_* / TE_*) */
    int entnum;
} hmsg_t;

typedef struct {
    int frame, entnum, soundindex;
} hsound_t;

static byte h_wbuf[4096];
static int h_wlen;
static hmsg_t h_msgs[8192];
static int h_nmsgs;
static hsound_t h_sounds_log[8192];
static int h_nsounds;

static void h_w(int c) { if (h_wlen < (int)sizeof(h_wbuf)) h_wbuf[h_wlen++] = (byte)c; }
static void imp_WriteChar(int c) { h_w(c); }
static void imp_WriteByte(int c) { h_w(c); }
static void imp_WriteShort(int c) { h_w(c & 255); h_w((c >> 8) & 255); }
static void imp_WriteLong(int c) { imp_WriteShort(c & 0xffff); imp_WriteShort((c >> 16) & 0xffff); }
static void imp_WriteFloat(float f) { union { float f; int i; } u; u.f = f; imp_WriteLong(u.i); }
static void imp_WriteString(const char *s) { while (s && *s) h_w(*s++); h_w(0); }
static void imp_WritePosition(const vec3_t p) { for (int i = 0; i < 3; i++) imp_WriteShort((int)(p[i] * 8)); }
static void imp_WriteDir(const vec3_t p) { (void)p; h_w(0); }
static void imp_WriteAngle(float f) { h_w((int)(f * 256 / 360) & 255); }

static void h_flush_to(int to)
{
    if (h_wlen > 1 && (h_wbuf[0] == H_SVC_STUFFTEXT || h_wbuf[0] == H_SVC_LAYOUT)) {
        h_wbuf[h_wlen < (int)sizeof(h_wbuf) ? h_wlen : (int)sizeof(h_wbuf) - 1] = 0;
        h_log_long(to, h_wbuf[0], (const char *)h_wbuf + 1);
    }
    if (h_wlen && h_nmsgs < (int)(sizeof(h_msgs) / sizeof(h_msgs[0]))) {
        hmsg_t *m = &h_msgs[h_nmsgs++];
        m->frame = h_frames;
        m->svc = h_wbuf[0];
        m->entnum = -1;
        m->sub = -1;
        if (m->svc == H_SVC_MUZZLEFLASH && h_wlen >= 4) {
            m->entnum = h_wbuf[1] | (h_wbuf[2] << 8);
            m->sub = h_wbuf[3];
        } else if (m->svc == H_SVC_TEMP_ENTITY && h_wlen >= 2) {
            m->sub = h_wbuf[1];
        }
    }
    h_wlen = 0;
}
static void h_flush(void) { h_flush_to(-1); }
static void imp_multicast(const vec3_t o, multicast_t to) { (void)o; (void)to; h_flush(); }
static void imp_unicast(edict_t *ent, qboolean reliable) { (void)reliable; h_flush_to(NUM(ent)); }

static void imp_sound(edict_t *ent, int channel, int idx, float vol, float att, float ofs)
{
    (void)channel; (void)vol; (void)att; (void)ofs;
    if (h_nsounds < (int)(sizeof(h_sounds_log) / sizeof(h_sounds_log[0])))
        h_sounds_log[h_nsounds++] = (hsound_t){ h_frames, ent ? NUM(ent) : -1, idx };
}
static void imp_positioned_sound(const vec3_t o, edict_t *ent, int channel, int idx, float vol, float att, float ofs)
{
    (void)o;
    imp_sound(ent, channel, idx, vol, att, ofs);
}

/* ------------------------------------------------------------------ */
/* memory */

typedef struct hblock_s { struct hblock_s *prev, *next; unsigned tag; } hblock_t;
static hblock_t h_blocks = { &h_blocks, &h_blocks, 0 };

static void *imp_TagMalloc(unsigned size, unsigned tag)
{
    hblock_t *b = calloc(1, sizeof(*b) + size);
    b->tag = tag;
    b->next = h_blocks.next; b->prev = &h_blocks;
    h_blocks.next->prev = b; h_blocks.next = b;
    return b + 1;
}
static void imp_TagFree(void *p)
{
    if (!p) return;
    hblock_t *b = (hblock_t *)p - 1;
    b->prev->next = b->next; b->next->prev = b->prev;
    free(b);
}
static void imp_FreeTags(unsigned tag)
{
    for (hblock_t *b = h_blocks.next, *n; b != &h_blocks; b = n) {
        n = b->next;
        if (b->tag == tag) imp_TagFree(b + 1);
    }
}

/* ------------------------------------------------------------------ */
/* command arguments */

static char h_argbuf[1024];
static char *h_argv[16];
static int h_argc;
static char h_args[1024];

static void h_tokenize(const char *text)
{
    snprintf(h_argbuf, sizeof(h_argbuf), "%s", text);
    h_argc = 0;
    for (char *p = strtok(h_argbuf, " "); p && h_argc < 16; p = strtok(NULL, " "))
        h_argv[h_argc++] = p;
    const char *sp = strchr(text, ' ');
    snprintf(h_args, sizeof(h_args), "%s", sp ? sp + 1 : "");
}
static int imp_argc(void) { return h_argc; }
static char *imp_argv(int n) { static char empty[1]; return n < h_argc ? h_argv[n] : empty; }
static char *imp_args(void) { return h_args; }

static char h_cmdtext[4096];
static void imp_AddCommandString(const char *text)
{
    if (h_verbose) printf("    [cmd] %s", text);
    strncat(h_cmdtext, text, sizeof(h_cmdtext) - strlen(h_cmdtext) - 1);
}
static void imp_DebugGraph(float v, int c) { (void)v; (void)c; }

/* ------------------------------------------------------------------ */
/* game driving */

static game_import_t h_imports = {
    .bprintf = imp_bprintf, .dprintf = imp_dprintf, .cprintf = imp_cprintf,
    .centerprintf = imp_centerprintf, .sound = imp_sound, .positioned_sound = imp_positioned_sound,
    .configstring = imp_configstring, .error = imp_error, .modelindex = imp_modelindex,
    .soundindex = imp_soundindex, .imageindex = imp_imageindex, .setmodel = imp_setmodel,
    .trace = imp_trace, .pointcontents = imp_pointcontents, .inPVS = imp_inPVS, .inPHS = imp_inPVS,
    .SetAreaPortalState = imp_SetAreaPortalState, .AreasConnected = imp_AreasConnected,
    .linkentity = imp_linkentity, .unlinkentity = imp_unlinkentity, .BoxEdicts = imp_BoxEdicts,
    .Pmove = imp_Pmove, .multicast = imp_multicast, .unicast = imp_unicast,
    .WriteChar = imp_WriteChar, .WriteByte = imp_WriteByte, .WriteShort = imp_WriteShort,
    .WriteLong = imp_WriteLong, .WriteFloat = imp_WriteFloat, .WriteString = imp_WriteString,
    .WritePosition = imp_WritePosition, .WriteDir = imp_WriteDir, .WriteAngle = imp_WriteAngle,
    .TagMalloc = imp_TagMalloc, .TagFree = imp_TagFree, .FreeTags = imp_FreeTags,
    .cvar = imp_cvar, .cvar_set = imp_cvar_set, .cvar_forceset = imp_cvar_set,
    .argc = imp_argc, .argv = imp_argv, .args = imp_args,
    .AddCommandString = imp_AddCommandString, .DebugGraph = imp_DebugGraph,
};

game_export_t *GetGameAPI(game_import_t *import);

static void h_start(const char *entities)
{
    char rate[16];
    snprintf(rate, sizeof(rate), "%d", h_rate);
    imp_cvar("sv_fps", rate, 0);
    /* a variable-FPS engine advertises GMF_VARIABLE_FPS (OpenFFA reads
     * sv_fps only then); the 10 Hz baseline keeps sv_features unset */
    if (h_rate != 10) imp_cvar("sv_features", va("%d", GMF_VARIABLE_FPS), 0);
    imp_cvar("deathmatch", "1", 0);
    imp_cvar("cheats", "1", 0);
    imp_cvar("maxclients", "4", 0);
    imp_cvar("game", HARNESS_MOD, 0);
    imp_cvar("gamedir", HARNESS_MOD, 0);
#ifdef HARNESS_ENGINE
    /* modelindex of "*N" is N + 1 (the engine hull lookup), 1 is the map */
    he_load_map(&h_world[0].mins[0], h_nworld, &h_inline[0].mins[0], h_ninline);
    h_models[1] = strdup("maps/harness.bsp");
    for (int k = 1; k <= h_ninline; k++) h_models[k + 1] = strdup(va("*%d", k));
#endif
    ge = GetGameAPI(&h_imports);
    if (ge->apiversion != 3) imp_error("apiversion %d", ge->apiversion);
    ge->Init();
#ifdef HARNESS_ENGINE
    he_set_game(ge);
#endif
    ge->SpawnEntities("harness", entities, "");
    /* precondition: the module was built with variable FPS and runs at the
     * harness rate (otherwise a 60 Hz result would silently be 10 Hz) */
    cvar_t *f = h_cvar_find("g_features");
    if (h_rate != 10 && (!f || !((int)f->value & GMF_VARIABLE_FPS))) imp_error("module does not export GMF_VARIABLE_FPS");
    if (H_MOD_RATE != h_rate) imp_error("module world rate %d != harness rate %d", (int)H_MOD_RATE, h_rate);
}

static int h_falls;     /* EV_FALLSHORT/EV_FALL/EV_FALLFAR of clients so far */

static void h_frame(void)
{
    ge->RunFrame();
    h_frames++;
    /* engine SV_PrepWorldFrame: events last one frame */
    for (int n = 1; n < ge->num_edicts; n++) {
        int ev = EDICT(n)->s.event;
        h_falls += EDICT(n)->client && (ev == EV_FALLSHORT || ev == EV_FALL || ev == EV_FALLFAR);
        EDICT(n)->s.event = 0;
    }
}

static void h_frames_n(int n) { while (n-- > 0) h_frame(); }

static void h_client_command(edict_t *ent, const char *text)
{
    h_tokenize(text);
    ge->ClientCommand(ent);
}

static edict_t *h_connect(int slot, const char *name)
{
    edict_t *ent = EDICT(slot + 1);
    char userinfo[MAX_INFO_STRING];
    snprintf(userinfo, sizeof(userinfo),
             "\\name\\%s\\skin\\male/grunt\\hand\\0\\fov\\90\\rate\\25000\\msg\\0\\ip\\127.0.0.%d:27901",
             name, slot + 2);
    if (!ge->ClientConnect(ent, userinfo)) imp_error("ClientConnect rejected %s", name);
    ge->ClientBegin(ent);
    return ent;
}

static void h_think(edict_t *ent, int buttons, int msec)
{
    usercmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.msec = (byte)msec;
    cmd.buttons = (byte)buttons;
    h_pmove_self = ent;
    h_cmds++;
    h_cmd_msec += msec;
    if (msec < h_cmd_min) h_cmd_min = msec;
    if (msec > h_cmd_max) h_cmd_max = msec;
    ge->ClientThink(ent, &cmd);
}

/* One world tick with n usercmds of the given buttons before it (as the
 * engine runs the packets of a tick before RunFrame), together lasting the
 * tick (h_tick_ms difference). */
static void h_tick(edict_t *ent, int buttons, int ncmds)
{
    int d = h_tick_ms(h_frames + 1, h_rate) - h_tick_ms(h_frames, h_rate);
    for (int i = 0; i < ncmds; i++) h_think(ent, buttons, d * (i + 1) / ncmds - d * i / ncmds);
    h_frame();
}

/* One world tick with the usercmds of `ent` the schedule h_sched delivers
 * before it (see h_sched) */
static void h_tick_sched(edict_t *ent, int buttons)
{
    long long k = h_frames;
    if (h_sched == H_SCHED_TICK) {
        h_tick(ent, buttons, 1);
        return;
    }
    if (h_sched == H_SCHED_BATCH) {
        if (k % 5 == 0)
            for (long long i = k > 4 ? k - 4 : 0; i <= k; i++)
                h_think(ent, buttons, h_tick_ms(i + 1, h_rate) - h_tick_ms(i, h_rate));
    } else {
        int pps = h_sched == H_SCHED_30 ? 30 : 10;
        for (long long j = k * pps / h_rate + 1; j <= (k + 1) * pps / h_rate; j++)
            h_think(ent, buttons, h_tick_ms(j, pps) - h_tick_ms(j - 1, pps));
    }
    h_frame();
}

/* place the player at `to` through its next usercmd (movement itself is
 * not under test) */
static void h_place(edict_t *ent, float x, float y, float z)
{
    h_pm_move_ent = ent;
    VectorSet(h_pm_to, x, y, z);
}

static int h_stat(edict_t *ent, int stat) { return ent->client->ps.stats[stat]; }
static int h_health(edict_t *ent) { return h_stat(ent, STAT_HEALTH); }
static int h_ammo(edict_t *ent) { return h_stat(ent, STAT_AMMO); }

static int h_count_msgs(int svc, int sub, int from_msg)
{
    int c = 0;
    for (int i = from_msg; i < h_nmsgs; i++)
        if (h_msgs[i].svc == svc && (sub < 0 || (h_msgs[i].sub & ~MZ_SILENCED) == sub)) c++;
    return c;
}

static int h_count_sounds(int idx, int from)
{
    int c = 0;
    for (int i = from; i < h_nsounds; i++)
        if (h_sounds_log[i].soundindex == idx) c++;
    return c;
}

/* client text (cprintf, stufftext or layout) sent to ent from message
 * index `from`; svc 0 is a cprintf */
static const htext_t *h_text_to(edict_t *ent, int svc, const char *needle, int from)
{
    for (int i = from; i < h_ntexts; i++)
        if (h_texts[i].entnum == NUM(ent) && h_texts[i].svc == svc && strstr(h_texts[i].text, needle))
            return &h_texts[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* common scene: floor, one spawn point */

static char h_ents[8192];

static void h_ent(const char *fmt, ...)
{
    size_t len = strlen(h_ents);
    va_list ap; va_start(ap, fmt);
    vsnprintf(h_ents + len, sizeof(h_ents) - len, fmt, ap);
    va_end(ap);
}

static void h_scene_base(void)
{
    h_add_world(-4096, -4096, -64, 4096, 4096, 0);     /* floor, top at z=0 */
    h_ents[0] = 0;
    h_ent("{\n\"classname\" \"worldspawn\"\n}\n");
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"0 0 24\"\n\"angle\" \"0\"\n}\n");
    h_ent("{\n\"classname\" \"info_player_start\"\n\"origin\" \"0 0 24\"\n\"angle\" \"0\"\n}\n");
}

/* Bring a connected client into play. OpenFFA: first attack spawns from
 * pregame. OpenTDM: join a team. Then let the respawn settle. */
static edict_t *h_join(int slot, const char *name)
{
    edict_t *ent = h_connect(slot, name);
    h_frame();
#ifdef HARNESS_OPENTDM
    h_client_command(ent, slot & 1 ? "team b" : "team a");
#else
    h_tick(ent, BUTTON_ATTACK, 1);
#endif
    h_tick(ent, 0, 1);
    h_frames_n(2);
    return ent;
}

static void h_give(edict_t *ent, const char *item, int count)
{
    char text[128];
    if (count > 0) snprintf(text, sizeof(text), "give %s %d", item, count);
    else snprintf(text, sizeof(text), "give %s", item);
    h_client_command(ent, text);
}

/* weapon switch and let the activation finish */
static void h_use(edict_t *ent, const char *weapon)
{
    char text[128];
    snprintf(text, sizeof(text), "use %s", weapon);
    h_client_command(ent, text);
    for (int i = 0; i < 2 * h_rate; i++) h_tick(ent, 0, 1);
}

#include "scenarios.h"

#ifdef HARNESS_ENGINE
#define H_ENGINE 1
#else
#define H_ENGINE 0
#endif

/* `list`: T13/T14 (movers) need the real collision integration and are
 * listed by the engine build only; it also repeats T12 crush/blocked
 * (t12_rotating_touch is a usercmd contact contract: the box Pmove reports
 * contact without movement, the engine Pmove only on a move into the brush;
 * t12_rotating_ride is the same contract through the engine Pmove, whose
 * ground trace lists the platform, and is engine-only: on the box the
 * fixture adds one hit per world tick, see sc_t12_rotating_ride) */
static int h_listed(const char *name)
{
    int mover = !strncmp(name, "t13_", 4) || !strncmp(name, "t14_", 4);
    int crush = !strncmp(name, "t12_", 4) && strcmp(name, "t12_rotating_touch");
    if (h_sched != H_SCHED_TICK) return H_ENGINE && mover;     /* the path ticks follow the schedule */
    return H_ENGINE ? mover || crush : !mover && strcmp(name, "t12_rotating_ride");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <scenario>|list\n", argv[0]); return 2; }
    h_verbose = getenv("HARNESS_VERBOSE") != NULL;
    if (getenv("HARNESS_RATE")) h_rate = atoi(getenv("HARNESS_RATE"));
    if (h_rate < 10 || h_rate % 10) { fprintf(stderr, "HARNESS_RATE must be a multiple of 10\n"); return 2; }
    h_div = h_rate / 10;
    const char *sched = getenv("HARNESS_SCHED");
    if (sched) {
        for (h_sched = 0; h_sched < 4 && strcmp(sched, h_sched_names[h_sched]); h_sched++) {}
        if (h_sched == 4) { fprintf(stderr, "HARNESS_SCHED must be tick, 30, 10 or batch\n"); return 2; }
    }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; h_scenarios[i].name; i++)
            if (h_listed(h_scenarios[i].name)) printf("%s\n", h_scenarios[i].name);
        return 0;
    }
    for (int i = 0; h_scenarios[i].name; i++) {
        if (strcmp(h_scenarios[i].name, argv[1])) continue;
        printf("%s %s @%d Hz%s%s%s\n", HARNESS_MOD, argv[1], h_rate, H_ENGINE ? " (engine collision)" : "",
               h_sched ? ", usercmd schedule " : "", h_sched ? h_sched_names[h_sched] : "");
        if (setjmp(h_abort)) {
            printf("  FAIL game error: %s\n", h_errmsg);
            return 1;
        }
#ifdef HARNESS_ENGINE
        he_init();
#endif
        h_scenarios[i].run();
        printf("%s %s: %s\n", HARNESS_MOD, argv[1], h_failures ? "FAIL" : "PASS");
        return h_failures ? 1 : 0;
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}
