/*
 * Client timeline harness: T02/T07 (and T08 demo/MVD) on a real listen
 * server + client over the real loopback netchan and parser.
 *
 * Links the unmodified object files of a plain native q2pro build
 * (system.c.o gets main renamed). Linker wraps:
 *   Sys_Milliseconds  -> virtual clock
 *   NET_Sleep         -> real NET_Sleep(0) (no wall-clock waits)
 *   CL_InitRefresh    -> registers the cvars/commands of the 2D layer only
 *   SCR_UpdateScreen, SCR_BeginLoadingPlaque -> no-op (no renderer)
 *   NET_SendPacket    -> server->client loopback faults: drop, duplicate
 *                        (N copies: queue pressure), reorder; client->server
 *                        delay (history boundary); everything else passes
 *   CL_DeltaFrame     -> calls the real one, then records the accepted frame
 * The game is a real dlopen'd library (testgame.c) loaded with
 * sys_forcegamelib; the driver moves one entity and the player from RunFrame.
 *   SV_Frame          -> calls the real one and records the world steps of
 *                        one host callback (T05 batch <= 5, command boundary,
 *                        pause, error in a batch; T06 loopback capacity/burst)
 *
 *   SV_ClientAddMessage -> counts the setting replies per client and can
 *                        put a large reliable in front of the capability reply
 * The rational timeline is negotiated by the fork's capability
 * (CLS_EXACTFPS / SVS_EXACTFPS); the driver only observes it. CL_KIND says
 * which peer the build is and so what to expect (README, expected rates):
 *   new     this client and server (default): exact timeline, the fork rate
 *   legacy  the client .c files of master (no capability): the upstream
 *           client rate
 *   oldsv   this client, server user.c of master (no reply): legacy scale,
 *           the upstream GCD rates
 *   nofpssv this client against a server built without USE_FPS
 * A build without USE_FPS is the client without FPS (always 10 Hz).
 * CL_SERVE=PORT / CL_CONNECT=ADDR run a real-time listen server on UDP or a
 * client of one (several processes, T03/T07 mixed fleet).
 *
 * usage: driver <scenario>|list   (env CL_GAME, CL_BASE, CL_RATE,
 *                                  CL_UPDATERATE, CL_KIND, CL_SERVE, CL_CONNECT,
 *                                  CL_NAME, CL_QPORT, CL_PEERS, CL_PEER_DEMO,
 *                                  CL_PEER_LEAVE)
 */
#include <unistd.h>
#include "src/client/client.h"
#include "src/server/server.h"


int q2pro_real_main(int argc, char **argv);

static int h_failures;
static int h_rate = 10;     /* CL_RATE: sv_fps */
static int h_update;        /* CL_UPDATERATE: cl_updaterate (0 = server rate) */
enum { K_NEW = 1, K_LEGACY = 2, K_OLDSV = 4, K_NOFPSSV = 8 };
static int h_kind = K_NEW;  /* CL_KIND */
static bool h_real;         /* CL_SERVE / CL_CONNECT: real time over UDP */
#define CHECK(cond, ...) do { \
        if (cond) { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } \
        else { h_failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)
#define INFO(...) do { printf("  info "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* Q2_MAP: a deathmatch map of the game data (default q2dm1) */
static const char *h_map(void)
{
    const char *map = getenv("Q2_MAP");
    return map && *map ? map : "q2dm1";
}

#if USE_FPS
#define H_DIV   cl.frametime.div
#else
#define H_DIV   1
#endif
#define H_CRATE (H_DIV * BASE_FRAMERATE)    /* client frame rate */
#define H_ENT   10                          /* driver-moved entity */

#if USE_FPS
#define H_EXACT cl.frametime_exact
#else
#define H_EXACT 0
#endif

/* reference frame time in the client's own scale: T(n) of utils.h when the
 * exact timeline is agreed, the legacy integer period otherwise */
static int64_t H_T(int64_t n)
{
#if USE_FPS
    if (!cl.frametime_exact)
        return n * cl.frametime.time;
#endif
    return Com_FrameTime(n, H_CRATE);
}

static int h_gcd(int a, int b) { return b ? h_gcd(b, a % b) : a; }

/* effective rate expected for cl_updaterate u at a server of h_rate Hz.
 * A client on the exact timeline (and any client of an upstream server)
 * gets gcd(server rate, request), the request clipped to 10..60 and 0 being
 * the server rate. A client without the capability on a fork server gets
 * the highest rate below that which divides the server rate and keeps a
 * whole period in ms (1000 % rate == 0). Requests 0/10/20/30/40/50/60:
 *   60 Hz  exact 60 10 20 30 20 10 60   legacy 20 10 20 20 20 10 20
 *   30 Hz  exact 30 10 10 30 10 10 30   legacy 10 10 10 10 10 10 10
 *   20, 40, 50 Hz: legacy = exact, every divisor has a whole period */
static int h_expect_rate(int u)
{
#if USE_FPS
    if (h_rate == BASE_FRAMERATE || h_kind == K_NOFPSSV)
        return BASE_FRAMERATE;
    int rate = h_gcd(h_rate, Q_clip((u ? u : h_rate) / BASE_FRAMERATE, 1, MAX_FRAMEDIV) * BASE_FRAMERATE);
    while (h_kind == K_LEGACY && (h_rate % rate || 1000 % rate))
        rate -= BASE_FRAMERATE;
    return rate;
#else
    (void)u;
    return BASE_FRAMERATE;
#endif
}

/* a cl_updaterate whose effective rate differs from that of the current
 * request: `pref` when it does, else the first such of 10..60 */
static int h_other_request(int pref)
{
    int cur = h_expect_rate(h_update);
    if (h_expect_rate(pref) != cur)
        return pref;
    for (int u = BASE_FRAMERATE; u <= MAX_FRAMEDIV * BASE_FRAMERATE; u += BASE_FRAMERATE)
        if (h_expect_rate(u) != cur)
            return u;
    return pref;
}

/* the capability is checked at a rated server only: at 10 Hz both scales
 * give the same times and the 10 Hz stages stay the upstream baseline */
static bool h_bad_reply;    /* the server's SVS_EXACTFPS carries 0, not the expected 1 */
static bool h_expect_exact(void) { return USE_FPS && h_kind == K_NEW && h_rate != BASE_FRAMERATE && !h_bad_reply; }
static bool h_exact_ok(int exact) { return h_rate == BASE_FRAMERATE || exact == h_expect_exact(); }

/* ---------------------------------------------------------------- wraps */

static unsigned h_now = 1000000;
unsigned __real_Sys_Milliseconds(void);
unsigned __wrap_Sys_Milliseconds(void) { return h_real ? __real_Sys_Milliseconds() : h_now; }

int __real_NET_Sleep(int msec);
int __wrap_NET_Sleep(int msec) { (void)msec; return __real_NET_Sleep(0); }

extern cvar_t *gl_modulate_world, *gl_modulate_entities, *gl_brightness;
void __wrap_CL_InitRefresh(void)
{
    gl_modulate_world = Cvar_Get("gl_modulate_world", "1", 0);
    gl_modulate_entities = Cvar_Get("gl_modulate_entities", "1", 0);
    gl_brightness = Cvar_Get("gl_brightness", "0", 0);
    V_Init();
    SCR_Init();
}

void __wrap_SCR_UpdateScreen(void) {}
void __wrap_SCR_BeginLoadingPlaque(void) {}

static int h_drop, h_dup, h_hold;   /* pending server->client faults */
static int h_dup_copies = 1;        /* copies of a duplicated datagram (queue pressure) */
static int h_lp_sends;              /* server -> loopback datagrams in the current call */
static int h_dropped, h_duped, h_reordered;
static byte h_held[MAX_PACKETLEN];
static size_t h_held_len;
static netadr_t h_held_to;

/* server-side frame overflow (send.c, write_datagram_new/_old): with
 * h_overflow armed, the next frame written while a new reliable waits to go
 * out is lost and only the reliable is sent, as when the frame does not fit */
static int h_overflow, h_overflowed;
bool __real_SV_WriteFrameToClient_Enhanced(client_t *client, unsigned maxsize);
bool __wrap_SV_WriteFrameToClient_Enhanced(client_t *client, unsigned maxsize)
{
    if (h_overflow && (client->netchan.message.cursize || !LIST_EMPTY(&client->msg_reliable_list))) {
        h_overflow = 0;
        h_overflowed++;
        return false;
    }
    return __real_SV_WriteFrameToClient_Enhanced(client, maxsize);
}

/* setting replies seen by SV_ClientAddMessage, per client slot */
static int h_rep_cap[MAX_CLIENTS], h_rep_fps[MAX_CLIENTS], h_rep_last[MAX_CLIENTS];
static int h_rep_order_bad;     /* an SVS_FPS not right after SVS_EXACTFPS (new peer) */
static int h_fps_at[MAX_CLIENTS];  /* h_nframes at the first SVS_FPS after a reset to -1 */
static int h_nframes;
static int h_fat;               /* put a reliable that fills the packet before the next reply */
static bool h_frag_carrier;     /* the carrier to drop is the next fragment */

void __real_SV_ClientAddMessage(client_t *client, int flags);
static void h_fill_reliable(client_t *client)
{
    byte save[16];
    size_t n = msg_write.cursize;
    memcpy(save, msg_write.data, n);
    SZ_Clear(&msg_write);
    /* reliable + the frame of the same datagram exceed maxpacketlen: fragments */
    int room = client->netchan.maxpacketlen - client->netchan.message.cursize - 2 * 9 - 6;
    while (room > 3) {
        int k = min(room - 3, 1000);
        MSG_WriteByte(svc_print);
        MSG_WriteByte(PRINT_LOW);
        for (int i = 0; i < k; i++)
            MSG_WriteByte(i == k - 1 ? '\n' : 'x');
        MSG_WriteByte(0);
        room -= k + 3;
    }
    __real_SV_ClientAddMessage(client, MSG_RELIABLE | MSG_CLEAR);
    memcpy(msg_write.data, save, n);
    msg_write.cursize = n;
}

void __wrap_SV_ClientAddMessage(client_t *client, int flags)
{
    if (msg_write.cursize == 9 && msg_write.data[0] == svc_setting && client->number >= 0 && client->number < MAX_CLIENTS) {
        int idx = RL32(msg_write.data + 1), n = client->number;
        if (idx == SVS_EXACTFPS) {
            h_rep_cap[n]++;
            if (h_bad_reply)
                WL32(msg_write.data + 5, 0);
            if (h_fat) {
                h_fat = 0;
                h_fill_reliable(client);
            }
        }
        if (idx == SVS_FPS) {
            h_rep_fps[n]++;
            if (h_rep_cap[n] && h_rep_last[n] != SVS_EXACTFPS)
                h_rep_order_bad++;
            if (h_fps_at[n] < 0)
                h_fps_at[n] = h_nframes;
        }
        h_rep_last[n] = idx;
    }
    __real_SV_ClientAddMessage(client, flags);
}

/* the server datagram carrying the capability reply: the reliable bytes
 * svc_setting SVS_EXACTFPS 1, or with h_fat any fragment */
static int h_carrier_drop, h_carrier_dropped, h_frag_sent;
static bool h_is_carrier(const byte *data, size_t len)
{
    if (h_frag_carrier)
        return len >= 4 && (RL32(data) & BIT(30));
    byte pat[9] = { svc_setting };
    WL32(pat + 1, SVS_EXACTFPS);
    WL32(pat + 5, 1);
    return memmem(data, len, pat, sizeof(pat)) != NULL;
}

bool __real_NET_SendPacket(netsrc_t sock, const void *data, size_t len, const netadr_t *to);

/* client -> server delay (T07, history boundary): with h_delay set the
 * client's loopback datagrams leave h_delay ms after they were sent, in
 * order, at most one per tick (h_delay_release) */
static int h_delay;
static struct { unsigned due; size_t len; byte data[MAX_PACKETLEN]; } h_dq[4096];
static int h_dq_head, h_dq_n;
static netadr_t h_dq_to;

static void h_delay_push(const void *data, size_t len, const netadr_t *to)
{
    if (h_dq_n == (int)q_countof(h_dq) || len > MAX_PACKETLEN) {
        printf("  FAIL delay queue full\n");
        fflush(stdout);
        _exit(1);
    }
    int i = (h_dq_head + h_dq_n++) % (int)q_countof(h_dq);
    h_dq[i].due = h_now + h_delay;
    h_dq[i].len = len;
    memcpy(h_dq[i].data, data, len);
    h_dq_to = *to;
}

static void h_delay_release(void)
{
    if (!h_dq_n || h_dq[h_dq_head].due > h_now)
        return;
    __real_NET_SendPacket(NS_CLIENT, h_dq[h_dq_head].data, h_dq[h_dq_head].len, &h_dq_to);
    h_dq_head = (h_dq_head + 1) % (int)q_countof(h_dq);
    h_dq_n--;
}

bool __wrap_NET_SendPacket(netsrc_t sock, const void *data, size_t len, const netadr_t *to)
{
    if (sock == NS_CLIENT && to->type == NA_LOOPBACK && (h_delay || h_dq_n)) {
        h_delay_push(data, len, to);
        return true;
    }
    if (sock != NS_SERVER || to->type != NA_LOOPBACK)
        return __real_NET_SendPacket(sock, data, len, to);
    h_lp_sends++;
    if (len >= 4 && (RL32(data) & BIT(30)))
        h_frag_sent++;
    if (h_carrier_drop > 0 && h_is_carrier(data, len)) {
        h_carrier_drop--;
        h_carrier_dropped++;
        return true;
    }
    if (h_drop > 0) {
        h_drop--;
        h_dropped++;
        return true;
    }
    if (h_hold == 1 && len <= sizeof(h_held)) {
        memcpy(h_held, data, len);
        h_held_len = len;
        h_held_to = *to;
        h_hold = 2;
        return true;
    }
    bool ret = __real_NET_SendPacket(sock, data, len, to);
    if (h_hold == 2) {      /* the held one arrives after its successor */
        __real_NET_SendPacket(sock, h_held, h_held_len, &h_held_to);
        h_hold = 0;
        h_reordered++;
    }
    if (h_dup > 0) {
        h_dup--;
        h_duped++;
        for (int k = 0; k < h_dup_copies; k++)
            __real_NET_SendPacket(sock, data, len, to);
    }
    return ret;
}

/* accepted frames, as CL_DeltaFrame left the client state */
typedef struct {
    int number, delta, flags, serverdelta, servertime, keyservertime, div;
    int oldnumber, epoch, playback, exact;
    int ent_frame, anim_start, ent_lerp, ent_x, ent_prev_x;
    bool ps_dup;
} h_frame_t;
static h_frame_t h_frames[1 << 17];
static int h_epoch;
static int h_bad_time, h_bad_key, h_bad_anim;   /* contract mismatches */
static char h_first_bad[256];

static void h_bad(int *counter, const char *fmt, ...)
{
    (*counter)++;
    if (!h_first_bad[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(h_first_bad, sizeof(h_first_bad), fmt, ap);
        va_end(ap);
    }
}

void __real_CL_DeltaFrame(void);
void __wrap_CL_DeltaFrame(void)
{
    centity_t *ent = &cl_entities[H_ENT];
    __real_CL_DeltaFrame();

    h_frame_t *f = &h_frames[h_nframes & (q_countof(h_frames) - 1)];
    int fn = cl.frame.number - cl.serverdelta;
    *f = (h_frame_t){
        .number = cl.frame.number, .delta = cl.frame.delta, .flags = cl.frameflags,
        .serverdelta = cl.serverdelta, .servertime = cl.servertime, .div = H_DIV,
        .oldnumber = cl.oldframe.number, .epoch = h_epoch, .playback = cls.demo.playback, .exact = H_EXACT,
        .ent_frame = ent->current.frame, .ent_x = ent->current.origin[0], .ent_prev_x = ent->prev.origin[0],
        /* lerped update (entity_update_old): prev is the last state; a no-lerp
         * case copies the new state into prev (the entity steps its frame) */
        .ent_lerp = ent->serverframe == cl.frame.number && ent->prev.frame != ent->current.frame,
        .ps_dup = !memcmp(&cl.oldframe.ps, &cl.frame.ps, sizeof(cl.frame.ps)),
    };
#if USE_FPS
    f->keyservertime = cl.keyservertime;
    f->anim_start = ent->anim_start;
    if ((int64_t)cl.keyservertime != (fn / H_DIV) * BASE_FRAMETIME)
        h_bad(&h_bad_key, "frame %d: keyservertime %d, expected %d", fn, cl.keyservertime, (fn / H_DIV) * BASE_FRAMETIME);
    if (f->ent_lerp && ent->anim_start != H_T(fn - 1))
        h_bad(&h_bad_anim, "frame %d: anim_start %d, expected T(n-1) = %" PRId64, fn, ent->anim_start, H_T(fn - 1));
#endif
    if (cl.servertime != H_T(fn))
        h_bad(&h_bad_time, "frame %d at %d Hz: servertime %d, expected T(n) = %" PRId64, fn, H_CRATE, cl.servertime, H_T(fn));
    h_nframes++;
}

static int h_clear_exact;   /* timeline mode left by the last CL_ClearState */
void __real_CL_ClearState(void);
void __wrap_CL_ClearState(void)     /* serverdata: a new epoch */
{
    __real_CL_ClearState();
    h_epoch++;
    h_clear_exact = H_EXACT;
}

static h_frame_t *h_last(int back) { return &h_frames[(h_nframes - 1 - back) & (q_countof(h_frames) - 1)]; }

/* ------------------------------------------- batches */

/* world steps one external call may run: a variable FPS build batches up
 * to 5; a fixed FPS build keeps one step per call */
#define H_BATCH (USE_FPS ? 5 : 1)

static bool h_in_svframe;           /* inside SV_Frame (RunFrame there is a world step) */
static int h_call_steps;            /* world steps of the current SV_Frame */
static int h_steps_total;           /* world steps in SV_Frame, all calls */
static uint64_t h_c_in, h_c_out;    /* sv.framecredit around the last SV_Frame */
static unsigned h_c_msec;
static bool h_c_done;               /* the last SV_Frame returned (no longjmp) */
static int h_pending, h_pending_bad, h_queued, h_wait_at_queue;
static int h_thinks, h_game_map, h_game_error, h_error_fired;

unsigned __real_SV_Frame(unsigned msec);
unsigned __wrap_SV_Frame(unsigned msec)
{
    h_c_in = sv.framecredit;
    h_c_msec = msec;
    h_call_steps = 0;
    h_c_done = false;
    h_in_svframe = true;
    unsigned ret = __real_SV_Frame(msec);
    h_in_svframe = false;
    h_c_out = sv.framecredit;
    h_c_done = true;
    return ret;
}

/* game side of a step: a step while a queued command waits is a violation;
 * armed triggers queue a map change or throw from a step of the batch */
static void h_game_step(game_import_t *gi)
{
    if (!h_in_svframe)
        return;     /* settle frames of SV_SpawnServer */
    h_call_steps++;
    h_steps_total++;
    if (h_pending)
        h_pending_bad++;
    if (h_game_map && h_call_steps == 1) {
        h_game_map = 0;
        h_pending = 1;
        h_queued++;
        gi->AddCommandString(va("h_ack\nmap %s\n", h_map()));
    }
    if (h_game_error && h_call_steps == h_game_error) {
        h_game_error = 0;
        h_error_fired = h_call_steps;
        gi->error("h_batch_error");
    }
}

/* client command "h_q TEXT": the game queues TEXT from packet processing,
 * as a vote result would (h_ack marks when it runs) */
void h_game_command(game_import_t *gi, edict_t *ent)
{
    (void)ent;
    if (strcmp(gi->argv(0), "h_q"))
        return;
    h_wait_at_queue = cmd_buffer.waitCount;
    h_pending = 1;
    h_queued++;
    gi->AddCommandString(va("h_ack\n%s\n", gi->args()));
}

void h_game_think(edict_t *ent) { (void)ent; h_thinks++; }

static void h_ack_f(void) { h_pending = 0; }

/* ------------------------------------------------------- game callbacks */

static int h_ent_x_base, h_teleport;
static int h_game_frames;

void h_game_runframe(game_import_t *gi, edict_t *edicts, int frame)
{
    edict_t *e = &edicts[H_ENT];
    h_game_frames = frame;
    h_game_step(gi);
    e->inuse = qtrue;
    e->s.modelindex = 1;
    e->s.frame = frame % 30000;     /* world step counter, for demo/MVD durations */
    e->s.origin[0] = h_ent_x_base + frame % 400;
    if (h_teleport) {
        h_ent_x_base = h_ent_x_base ? 0 : 1000;
        e->s.origin[0] = h_ent_x_base + frame % 400;
        h_teleport = 0;
    }
    gi->linkentity(e);
    gclient_new_t *c = (gclient_new_t *)edicts[1].client;
    if (c)
        c->ps.pmove.origin[0] = (frame % 400) * 8;
}

/* -------------------------------------------------------------- engine */

static int h_bad_lerp, h_bad_mono, h_invalid;
static int h_last_time = INT_MIN, h_last_epoch = -1;

static void h_check_tick(void)
{
    if (cls.state != ca_active || sv_paused->integer || !cl.frame.valid)
        return;
    int fn = cl.frame.number - cl.serverdelta;
    int64_t s = cl.servertime, p = H_T(fn - 1);
    if (s != H_T(fn))
        return;     /* already counted per frame */
    if (h_last_epoch == h_epoch && cl.time < h_last_time)
        h_bad(&h_bad_mono, "cl.time went back %d -> %d", h_last_time, cl.time);
    h_last_time = cl.time;
    h_last_epoch = h_epoch;
    /* the legacy scale keeps upstream's reciprocal div / 100 ms (not 1 / period
     * at 30/60 Hz): the old scale for a peer without the capability */
    float inv = H_EXACT ? 1.0f / (float)(s - p) : H_DIV * BASE_1_FRAMETIME;
    float want = cl.time >= s ? 1.0f : (float)(cl.time - p) * inv;
    if (cl.time == s && fabsf(cl.lerpfrac - (float)(s - p) * inv) <= 1e-4f)
        want = cl.lerpfrac;     /* at s unclamped (legacy: div * 0.01 * period < 1) */
    if (cl.time < p || cl.time > s || fabsf(cl.lerpfrac - want) > 1e-4f)
        h_bad(&h_bad_lerp, "frame %d: time %d in [%" PRId64 ", %" PRId64 "], lerpfrac %.5f, expected %.5f",
              fn, cl.time, p, s, cl.lerpfrac, want);
}

/* per-call record against the batch policy: a call runs the owed
 * whole frames up to H_BATCH (fewer only at a command boundary, h_hold_ok)
 * and carries the rest; owed > 250 ms after the batch becomes 100 ms */
static bool h_calls_on, h_hold_ok;
static int h_call_max, h_call_hist[7], h_call_bad, h_call_held, h_lp_max;
static int h_last_steps;            /* world steps of the last call, also when it threw */
static char h_call_first[200];
static unsigned h_x_resets, h_x_reset_ms;  /* debt resets (> 250 ms owed -> 100 ms) the policy gives */

static void h_calls_reset(void)
{
    h_call_max = h_call_bad = h_call_held = h_lp_max = 0;
    h_x_resets = h_x_reset_ms = 0;
    memset(h_call_hist, 0, sizeof(h_call_hist));
    h_call_first[0] = 0;
}

static void h_note_call(void)
{
    int n = h_last_steps;
    h_call_hist[n < 6 ? n : 6]++;
    h_call_max = max(h_call_max, n);
    h_lp_max = max(h_lp_max, h_lp_sends);
    if (!h_c_done)
        return;
    uint64_t r = SV_FRAMERATE, c = h_c_in + (uint64_t)h_c_msec * r, owed = c / FRAME_CREDIT;
    uint64_t want = min(owed, (uint64_t)H_BATCH), left = c - (uint64_t)n * FRAME_CREDIT;
    uint64_t want_left = left >= FRAME_CREDIT && left > 250 * r ? 100 * r : left;
    if (want_left != left) {
        h_x_resets++;
        h_x_reset_ms += left / r - 100;
    }
    h_call_held += (uint64_t)n < want;
    if ((uint64_t)n > want || ((uint64_t)n < want && !h_hold_ok) || h_c_out != want_left) {
        if (!h_call_bad++)
            snprintf(h_call_first, sizeof(h_call_first), "call of %u ms with credit %" PRIu64 ": %d steps, credit %" PRIu64
                     " (expected %" PRIu64 " steps, credit %" PRIu64 ")", h_c_msec, h_c_in, n, h_c_out, want, want_left);
    }
}

static void h_tick(int ms)
{
    if (h_real)
        usleep(ms * 1000);
    else
        h_now += ms;
    h_delay_release();
    h_c_done = false;
    h_lp_sends = h_call_steps = 0;
    Qcommon_Frame();
    h_in_svframe = false;   /* also after a longjmp out of SV_Frame */
    h_last_steps = h_call_steps;
    if (h_calls_on)
        h_note_call();
    if (cls.state == ca_active && !cl.frame.valid)
        h_invalid++;
    h_check_tick();
}

/* frames of the current epoch parsed before the first one in the exact
 * timeline (-1: none in it yet) */
static int h_epoch_legacy_frames(int *first_exact)
{
    int legacy = 0;
    *first_exact = -1;
    for (int i = 0; i < h_nframes; i++) {
        h_frame_t *f = &h_frames[i & (q_countof(h_frames) - 1)];
        if (f->epoch != h_epoch || f->playback) continue;
        if (f->exact) { *first_exact = i; break; }
        legacy++;
    }
    return legacy;
}

static void h_run(int ms) { for (int i = 0; i < ms; i++) h_tick(1); }

static void h_cmd(const char *text)
{
    Cbuf_AddText(&cmd_buffer, text);
    Cbuf_AddText(&cmd_buffer, "\n");
}

static int h_want_div(void) { return h_expect_rate(h_update) / BASE_FRAMERATE; }

static bool h_wait_active(int max_ms)
{
    for (int i = 0; i < max_ms; i++) {
        if (cls.state == ca_active && H_DIV == h_want_div() && cl.frame.valid && h_exact_ok(H_EXACT))
            return true;
        h_tick(1);
    }
    return false;
}

static bool h_mvd = true;   /* sv_mvd_enable: the MVD dummy client takes a slot */

static void h_init(void)
{
    static char rate[16], update[16], *argv[80];
    const char *game = getenv("CL_GAME"), *base = getenv("CL_BASE");
    const char *serve = getenv("CL_SERVE"), *connect = getenv("CL_CONNECT");
    int argc = 0;

    if (!game || !base) {
        fprintf(stderr, "CL_GAME and CL_BASE are required\n");
        exit(2);
    }
    snprintf(rate, sizeof(rate), "%d", h_rate);
    snprintf(update, sizeof(update), "%d", h_update);
#define ARG(...) do { const char *v_[] = { __VA_ARGS__ }; \
        for (size_t i_ = 0; i_ < q_countof(v_); i_++) argv[argc++] = (char *)v_[i_]; } while (0)
    ARG("q2pro", "+set", "basedir", base, "+set", "homedir", base, "+set", "sys_forcegamelib", game,
        "+set", "sys_console", getenv("CL_CONSOLE") ? "1" : "0", "+set", "logfile", "0",
        "+set", "developer", getenv("CL_CONSOLE") ? getenv("CL_CONSOLE") : "0",
        "+set", "sv_novis", "1", "+set", "sv_mvd_enable", h_mvd ? "1" : "0", "+set", "maxclients", "4",
        // players are PM_FREEZE (not captured): keep MVD streams unsuspended
        "+set", "sv_mvd_suspend_time", "0", "+set", "cl_updaterate", update,
        "+set", "name", getenv("CL_NAME") ? getenv("CL_NAME") : "h_client");
    if (connect) {
        /* the paks are local; qport from the fleet (the default is the start
         * time, the same for processes started together) */
        if (getenv("CL_QPORT"))
            ARG("+set", "qport", getenv("CL_QPORT"));
        if (getenv("CL_PROTOCOL"))  /* 34: a client without the Q2PRO protocol */
            ARG("+set", "cl_protocol", getenv("CL_PROTOCOL"));
        ARG("+set", "allow_download", "0", "+connect", connect);
    } else {
        if (h_rate != 10)
            ARG("+set", "sv_fps", rate);
        if (serve)  /* deathmatch: maxclients holds and UDP opens; the MVD dummy takes a slot */
            ARG("+set", "deathmatch", "1", "+set", "maxclients", "8", "+set", "net_ip", "127.0.0.1", "+set", "net_port", serve,
                "+set", "sv_iplimit", "0");    /* all peers come from 127.0.0.1 */
        ARG("+map", h_map());
    }
#undef ARG
    argv[argc] = NULL;
    Qcommon_Init(argc, argv);
    Cmd_AddCommand("h_ack", h_ack_f);
    cls.active = ACT_ACTIVATED;     /* no window: the ACT_MINIMIZED 10 fps sleep loop otherwise */
    CL_UpdateFrameTimes();
}

/* connect; with !late the capability reply must precede the first frame */
static bool h_join_ex(bool late)
{
    h_init();
    bool ok = h_wait_active(h_real ? 20000 : 5000);
    if (cls.state != ca_active) {   /* not connecting at all is no rate or timeline finding */
        printf("  FAIL precondition: client not active (state %d)\n", cls.state);
        fflush(stdout);
        _exit(1);
    }
    CHECK(ok, "client active at %d Hz (div %d, state %d, exact timeline %d)", H_CRATE, H_DIV, cls.state, H_EXACT);
    if (!getenv("CL_CONNECT") && SV_FRAMERATE != h_rate) {
        printf("  FAIL precondition: server runs %d Hz, requested %d Hz\n", (int)SV_FRAMERATE, h_rate);
        fflush(stdout);
        _exit(1);
    }
    CHECK(h_exact_ok(H_EXACT), "exact timeline %d, expected %d for this peer", H_EXACT, h_expect_exact());
    if (!late && h_expect_exact()) {
        int first_exact, legacy = h_epoch_legacy_frames(&first_exact);
        CHECK(!h_expect_exact() || !legacy, "capability reply parsed before the first frame (%d frames in the old scale)", legacy);
    }
    h_run(200);
    return ok;
}

static bool h_join(void) { return h_join_ex(false); }

/* server slot of the local client */
static client_t *h_sv_client(void)
{
    for (int i = 0; svs.client_pool && i < sv_maxclients->integer; i++)
        if (svs.client_pool[i].state == cs_spawned && svs.client_pool[i].netchan.remote_address.type == NA_LOOPBACK)
            return &svs.client_pool[i];
    return NULL;
}

static void h_report_contract(const char *what)
{
    CHECK(!h_bad_time && !h_bad_key && !h_bad_anim && !h_bad_lerp && !h_bad_mono,
          "%s: frame times T(n), keyframes, anim start, lerp and monotonic cl.time hold "
          "(bad: time %d key %d anim %d lerp %d mono %d; first: %s)",
          what, h_bad_time, h_bad_key, h_bad_anim, h_bad_lerp, h_bad_mono, h_first_bad);
}

/* ------------------------------------------------------------ scenarios */

/* T02: parsed frames keep T(n), keyframes, interpolation bounds; a virtual
 * minute of frames is exactly 60000 ms; neighbours differ by 1000/R rounded */
static void t02_timeline(void)
{
    if (!h_join()) return;
    int first = h_nframes, w0 = sv.framenum;
    uint64_t c0 = sv.framecredit;
    h_run(61000);
    int last = h_nframes - 1, e = h_expect_rate(h_update);
    /* frames per unit of time: tells an exact 30 Hz (1830 frames in 61 s)
     * from a 33 ms period (1848), which the per-frame checks below cannot */
    CHECK(abs((last - first + 1) - 61 * e) <= 2, "61 s: %d frames at %d Hz (want %d)", last - first + 1, e, 61 * e);
    CHECK((uint64_t)(sv.framenum - w0) * FRAME_CREDIT + sv.framecredit == c0 + 61000ull * SV_FRAMERATE,
          "world steps account for 61000 ms at %d Hz exactly (%d steps)", (int)SV_FRAMERATE, sv.framenum - w0);
    h_frame_t *a = &h_frames[first], *b = &h_frames[last];
    int r = H_CRATE, lo = 1000 / r, hi = (1000 + r - 1) / r, odd = 0, gaps = 0;
    for (int i = first + 1; i <= last; i++) {
        h_frame_t *x = &h_frames[i - 1], *y = &h_frames[i];
        int d = y->servertime - x->servertime;
        if (y->number - x->number != 1) { gaps++; continue; }
        if (d != lo && d != hi) odd++;
    }
    CHECK(!odd, "neighbouring frames are %d/%d ms apart at %d Hz (%d other, %d gaps)", lo, hi, r, odd, gaps);
    int fa = a->number - a->serverdelta, minute = -1;
    for (int i = first; i <= last; i++)
        if (h_frames[i].number - h_frames[i].serverdelta == fa + 60 * r) { minute = i; break; }
    CHECK(minute >= 0 && h_frames[minute].servertime - a->servertime == 60000,
          "a minute of frames (%d frames) spans exactly 60000 ms (%d)", 60 * r,
          minute >= 0 ? h_frames[minute].servertime - a->servertime : -1);
    INFO("%d frames, servertime %d..%d, cl.time %d", last - first + 1, a->servertime, b->servertime, cl.time);
    h_report_contract("61 s");
}

/* T02 no-lerp: dropped frame keeps the full distance, teleport duplicates
 * the state, the player state is not lerped across a gap */
static void t02_nolerp(void)
{
    if (!h_join()) return;
    h_run(500);
    h_frame_t *f = h_last(0);
    CHECK(f->ent_lerp && f->ent_x != f->ent_prev_x, "moving entity lerps between consecutive frames (%d -> %d)",
          f->ent_prev_x, f->ent_x);

    int before = h_nframes, n0 = h_last(0)->number;
    h_drop = 1;
    h_run(300);
    int gap = -1;
    for (int i = before; i < h_nframes; i++)
        if (h_frames[i].number - h_frames[i - 1].number > 1) { gap = i; break; }
    CHECK(h_dropped == 1 && gap >= 0, "one server packet dropped, a frame gap follows (after %d)", n0);
    if (gap >= 0) {
        h_frame_t *x = &h_frames[gap - 1], *y = &h_frames[gap];
        int fx = x->number - x->serverdelta, fy = y->number - y->serverdelta;
        CHECK(y->servertime - x->servertime == H_T(fy) - H_T(fx),
              "frames %d -> %d: servertime advances the full distance %d ms (%" PRId64 ")",
              fx, fy, y->servertime - x->servertime, H_T(fy) - H_T(fx));
        CHECK(y->ps_dup, "player state not lerped across the gap (oldframe %d != n-1)", y->oldnumber);
        CHECK(!y->ent_lerp, "entity restarts as new across the gap (no anim lerp; origin from old_origin %d to %d)",
              y->ent_prev_x, y->ent_x);
    }

    h_teleport = 1;
    h_run(300);
    int tele = -1;
    for (int i = h_nframes - 1; i > 0; i--)
        if (abs(h_frames[i].ent_x - h_frames[i - 1].ent_x) > 512) { tele = i; break; }
    CHECK(tele >= 0 && !h_frames[tele].ent_lerp && h_frames[tele].ent_prev_x == h_frames[tele].ent_x &&
          h_frames[tele + 1].ent_lerp,
          "teleported entity is not lerped (prev %d, current %d)",
          tele >= 0 ? h_frames[tele].ent_prev_x : 0, tele >= 0 ? h_frames[tele].ent_x : 0);
    h_report_contract("drop/teleport");
}

/* T07: real writer -> netchan -> parser: full first frame, delta chain,
 * duplicate and reordered packets, outage beyond the history, missing base */
static void t07_parser(void)
{
    if (!h_join()) return;
    CHECK(h_frames[0].delta < 0, "first frame is a full frame (delta %d)", h_frames[0].delta);
    h_frame_t *f = h_last(0);
    // the server deltas from the last acked frame, which lags at 60 Hz
    CHECK(f->delta > 0 && f->number - f->delta < UPDATE_BACKUP,
          "steady state: delta-compressed from a recent base (%d from %d)", f->number, f->delta);

    int before = h_nframes, uniq = 1;
    h_dup = 5;
    for (int i = 0; i < 5000 && h_dup; i++) h_tick(1);
    h_run(300);
    for (int i = before; i < h_nframes; i++)
        if (h_frames[i].number <= h_frames[i - 1].number) uniq = 0;
    CHECK(h_duped == 5 && uniq && h_nframes - before >= 5, "5 duplicated packets: each frame parsed once, in order (%d frames)",
          h_nframes - before);

    before = h_nframes;
    h_hold = 1;
    for (int i = 0; i < 5000 && h_hold; i++) h_tick(1);
    h_run(300);
    uniq = 1;
    int gap = 0;
    for (int i = before; i < h_nframes; i++) {
        if (h_frames[i].number <= h_frames[i - 1].number) uniq = 0;
        if (h_frames[i].number - h_frames[i - 1].number > 1 && h_frames[i].delta == h_frames[i - 1].number) gap++;
    }
    CHECK(h_reordered == 1 && uniq && gap == 1, "reordered packet: the late one is discarded, the next is a delta over the gap");

    /* outage longer than the history: the server falls back to a full frame */
    int n0 = h_last(0)->number;
    h_drop = UPDATE_BACKUP + 4;
    for (int i = 0; i < 10000 && h_drop; i++) h_tick(1);
    h_run(500);
    int rec = -1;
    for (int i = h_nframes - 1; i >= 0 && h_frames[i].number > n0; i--)
        rec = i;
    CHECK(rec >= 0 && h_frames[rec].delta < 0 && h_frames[rec].number - n0 > UPDATE_BACKUP,
          "outage of %d packets: recovery by a full frame %d (delta %d)", UPDATE_BACKUP + 4,
          rec >= 0 ? h_frames[rec].number : 0, rec >= 0 ? h_frames[rec].delta : 0);
    CHECK(cls.state == ca_active && cl.frame.valid && h_last(0)->delta > 0, "delta frames again after the outage");

    /* missing base: the next delta refers to a frame the client no longer
     * has. The server deltas from the last acked frame, one or two behind
     * the current one depending on the packet phase, so the whole history goes */
    before = h_nframes;
    int invalid = 0;
    for (int i = 0; i < UPDATE_BACKUP; i++)
        cl.frames[i].number = -1;
    for (int i = 0; i < 1000; i++) {
        h_tick(1);
        if (cls.state == ca_active && !cl.frame.valid) invalid++;
    }
    int full = -1;
    for (int i = before; i < h_nframes; i++)
        if (h_frames[i].delta < 0) { full = i; break; }
    CHECK(invalid > 0, "delta from a missing base is rejected as invalid");
    CHECK(full >= 0, "recovery by a full frame after the invalid one");
    CHECK(cls.state == ca_active && cl.frame.valid && h_last(0)->delta > 0, "delta frames again after recovery");
    h_report_contract("parser faults");
}

/* T02: cl_updaterate changes rebase the anchor: servertime keeps going,
 * no reset to zero, the 100 ms keyframe phase is kept */
static void t02_rate_change(void)
{
    if (!h_join()) return;
    static const char *const rates[] = { "20", "30", "10", "60", "30", "60" };
    int bad = 0, worst = 0;
    for (size_t k = 0; k < q_countof(rates); k++) {
        h_run(137 + 31 * k);    /* arbitrary phase of the change */
        int before = h_nframes, t0 = cl.servertime, want = h_expect_rate(atoi(rates[k])) / BASE_FRAMERATE;
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "cl_updaterate %s", rates[k]);
        h_cmd(cmd);
        for (int i = 0; i < 2000 && H_DIV != want; i++) h_tick(1);
        h_run(400);
        CHECK(H_DIV == want, "cl_updaterate %s: client div %d", rates[k], H_DIV);
        for (int i = before; i < h_nframes; i++) {
            h_frame_t *x = &h_frames[i - 1], *y = &h_frames[i];
            int d = y->servertime - x->servertime;
            if (d < 0 || d > 200) { bad++; if (abs(d) > abs(worst)) worst = d; }
            int fn = y->number - y->serverdelta;
            if (y->keyservertime != y->servertime - y->servertime % BASE_FRAMETIME || (fn % y->div == 0) != (y->servertime % BASE_FRAMETIME == 0)) {
                bad++;
                INFO("frame %d div %d: servertime %d keyservertime %d", fn, y->div, y->servertime, y->keyservertime);
            }
        }
        CHECK(cl.servertime > t0, "servertime continues across the change (%d -> %d)", t0, cl.servertime);
    }
    CHECK(!bad, "rebase keeps servertime monotonic without jumps > 200 ms and the keyframe phase (bad %d, worst %d)", bad, worst);
    h_report_contract("rate changes");
}

/* T02, this client in the legacy scale (server without the capability):
 * cl_updaterate 10 -> 20 whose SVS_FPS comes in a packet without a frame;
 * until the next frame the lerp stays in [servertime - new period,
 * servertime] as upstream */
static void t02_legacy_setting_overflow(void)
{
    h_update = 10;
    if (!h_join()) return;
    h_run(537);
    h_overflow = 1;
    int u = h_other_request(20), to = h_expect_rate(u) / BASE_FRAMERATE;
    h_cmd(va("cl_updaterate %d", u));
    int bare = 0;
    for (int i = 0; i < 2000 && H_DIV != to; i++) {
        int n = h_nframes;
        h_tick(1);
        bare = h_nframes == n;
    }
    h_run(300);
    CHECK(H_DIV == to && h_overflowed == 1 && bare,
          "SVS_FPS (div %d) parsed from a packet whose frame overflowed (div %d, overflowed %d, no frame %d)",
          to, H_DIV, h_overflowed, bare);
    h_report_contract("legacy setting without a frame");
}

/* T02: large valid frame numbers stay exact; the overflow guard fires
 * where T(n) leaves int */
static void t02_large_framenum(void)
{
    if (!h_join()) return;
    int r = H_CRATE, div = H_DIV;
    int64_t limit = (int64_t)INT_MAX * r / 1000;    /* largest n with T(n) <= INT_MAX, about */
    int target = (int)(limit - 10 * r) / div * div;
    cl.serverdelta = Q_align_down(cl.frame.number - target, div);
    h_run(300);
    h_frame_t *f = h_last(0);
    int fn = f->number - f->serverdelta;
    CHECK(cls.state == ca_active && fn > target && f->servertime == H_T(fn),
          "frame %d (T = %" PRId64 " ms): servertime %d is exact", fn, H_T(fn), f->servertime);
    h_report_contract("large frame numbers");

    target = (int)((limit + 2 * r) / div * div);
    cl.serverdelta = Q_align_down(cl.frame.number - target, div);
    int64_t t = H_T(cl.frame.number - cl.serverdelta);
    h_run(300);
    CHECK(cls.state != ca_active, "frame %d (T = %" PRId64 " > INT_MAX): \"server time overflowed\" drops the connection (state %d, servertime %d)",
          target, t, cls.state, cl.servertime);
}

/* map/serverdata and disconnect start a new epoch: no old anchor, mode
 * or server debt survives */
static void t02_epoch_reset(void)
{
    if (!h_join()) return;
    h_run(1000);
    int epoch = h_epoch, t_old = cl.servertime;
    h_cmd(va("map %s", h_map()));
    h_tick(1);
    for (int i = 0; i < 2000 && cls.state == ca_active; i++) h_tick(1);
    CHECK(sv.framecredit < FRAME_CREDIT && sv.framenum <= 2, "new map: server debt cleared (credit %" PRIu64 ", framenum %d)",
          sv.framecredit, sv.framenum);
    CHECK(h_wait_active(5000), "client active again on the new map");
    CHECK(h_epoch > epoch, "serverdata started a new client epoch");
    h_frame_t *f = h_last(0);
    int first_exact, legacy = h_epoch_legacy_frames(&first_exact);
    CHECK(!h_clear_exact && h_exact_ok(f->exact) && (!h_expect_exact() || !legacy),
          "timeline mode reset by serverdata (exact %d after it) and agreed again before the first frame "
          "(exact %d, %d frames in the old scale)", h_clear_exact, f->exact, legacy);
    CHECK(f->epoch == h_epoch && f->servertime < 1000 && f->servertime < t_old,
          "first frames of the new map: servertime from the new anchor (%d, was %d)", f->servertime, t_old);
    h_run(1000);
    h_report_contract("after map");

    h_cmd("disconnect");
    h_run(50);
    CHECK(cls.state == ca_disconnected, "disconnected (state %d)", cls.state);
    CHECK(cl.servertime == 0 && cl.serverdelta == 0 && cl.time <= 50, "client clocks cleared (%d %d %d)",
          cl.servertime, cl.serverdelta, cl.time);
#if USE_FPS
    CHECK(!cl.frametime_exact && !cl.frametime.div, "timeline mode cleared (exact %d div %d)", cl.frametime_exact, cl.frametime.div);
#endif
    CHECK(SV_FRAMERATE == BASE_FRAMERATE, "server reset to the base rate (%d)", (int)SV_FRAMERATE);
}

/* T08: a demo recorded from the R Hz world keeps the 10 Hz time basis,
 * the duration and order; seek and the return to live restore each scale */
static void t08_demo(void)
{
    char name[32], cmd[64];
    snprintf(name, sizeof(name), "h_demo_%d_%d", h_rate, h_update);
    if (!h_join()) return;
    snprintf(cmd, sizeof(cmd), "record %s", name);
    h_cmd(cmd);
    h_run(50);
    int g0 = h_game_frames;
    h_run(3000);
    int g1 = h_game_frames;
    h_cmd("stop");
    h_run(50);

    snprintf(cmd, sizeof(cmd), "demo %s", name);
    h_cmd(cmd);
    int first = h_nframes;
    for (int i = 0; i < 5000 && !(cls.demo.playback && cls.state == ca_active); i++) h_tick(1);
    CHECK(cls.demo.playback && cls.state == ca_active, "demo playback active");
#if USE_FPS
    CHECK(H_DIV == 1 && !cl.frametime_exact, "playback on the 10 Hz base, legacy scale (div %d, exact %d)",
          H_DIV, cl.frametime_exact);
#endif
    h_run(1000);
    h_cmd("seek +1");
    for (int i = 0; i < 6000 && cls.demo.playback; i++) h_tick(1);

    int n = 0, step_bad = 0, frame_bad = 0, span_t = 0, span_g = 0, seeks = 0, seek_bad = 0;
    h_frame_t *p = NULL;
    for (int i = first; i < h_nframes; i++) {
        h_frame_t *x = &h_frames[i];
        if (!x->playback) continue;
        if (p && x->number > p->number + 1) {     /* the seek: content jumps, the clock goes on */
            seeks++;
            if (x->servertime != p->servertime ||
                x->ent_frame - p->ent_frame != (x->number - p->number) * (h_rate / BASE_FRAMERATE))
                seek_bad++;
            INFO("seek: frame %d -> %d, servertime %d -> %d, world step %d -> %d", p->number, x->number,
                 p->servertime, x->servertime, p->ent_frame, x->ent_frame);
        }
        if (p && x->number == p->number + 1) {
            if (x->servertime - p->servertime != BASE_FRAMETIME) step_bad++;
            if (x->ent_frame - p->ent_frame != h_rate / BASE_FRAMERATE) frame_bad++;
            span_t += BASE_FRAMETIME;
            span_g += x->ent_frame - p->ent_frame;
        }
        p = x;
        n++;
    }
    INFO("recorded %d world steps; played %d frames", g1 - g0, n);
    CHECK(!step_bad, "demo frames are 100 ms apart (%d other)", step_bad);
    CHECK(!frame_bad, "each demo frame advances the world by %d steps (%d other)", h_rate / BASE_FRAMERATE, frame_bad);
    CHECK(span_g * BASE_FRAMETIME == span_t * (h_rate / BASE_FRAMERATE),
          "played world time matches played demo time (%d steps over %d ms)", span_g, span_t);
    CHECK(seeks == 1 && !seek_bad, "seek +1 skips 10 demo frames of content, the clock is kept (%d seeks, %d bad)",
          seeks, seek_bad);
    CHECK(n >= 15, "demo played (%d frames)", n);

    h_cmd(va("map %s", h_map()));
    CHECK(h_wait_active(5000), "back to live play");
    h_run(500);
    CHECK(!cls.demo.playback && H_DIV == h_want_div(), "live scale restored (div %d)", H_DIV);
    h_report_contract("demo and back to live");
}

/* T08 MVD: recorded from the R Hz world, played on the 10 Hz MVD server
 * through the waiting room; no debt or rate of the game survives */
static void t08_mvd(void)
{
    char name[32], cmd[64];
    snprintf(name, sizeof(name), "h_mvd_%d_%d", h_rate, h_update);
    if (!h_join()) return;
    snprintf(cmd, sizeof(cmd), "mvdrecord %s", name);
    h_cmd(cmd);
    h_run(3000);
    h_cmd("mvdstop");
    h_run(50);
    snprintf(cmd, sizeof(cmd), "mvdplay %s.mvd2", name);
    h_cmd(cmd);
    for (int i = 0; i < 1000 && sv.state != ss_broadcast; i++) h_tick(1);
    CHECK(sv.state == ss_broadcast, "MVD playback started (server state %d)", sv.state);
    CHECK(SV_FRAMERATE == BASE_FRAMERATE && sv.framecredit < FRAME_CREDIT,
          "MVD server on the 10 Hz base without carried debt (rate %d, credit %" PRIu64 ")",
          (int)SV_FRAMERATE, sv.framecredit);
    int first = h_nframes, epoch = h_epoch;
    h_run(4000);
    CHECK(h_epoch > epoch, "client reconnected to the MVD server (epoch %d -> %d)", epoch, h_epoch);
    int n = 0, step_bad = 0, frame_bad = 0, withent = 0;
    for (int i = first + 1; i < h_nframes; i++) {
        h_frame_t *x = &h_frames[i - 1], *y = &h_frames[i];
        if (y->epoch == epoch || y->epoch != x->epoch || y->number != x->number + 1) continue;
        n++;
        if (y->servertime - x->servertime != BASE_FRAMETIME) step_bad++;
        if (y->ent_frame && x->ent_frame) {
            withent++;
            if (y->ent_frame - x->ent_frame != h_rate / BASE_FRAMERATE) frame_bad++;
        }
    }
    CHECK(n > 10 && withent > 10, "client follows the MVD game (%d consecutive frames, %d with the entity, state %d, div %d)",
          n, withent, cls.state, H_DIV);
    CHECK(!step_bad && !frame_bad, "MVD frames 100 ms apart, %d world steps each (%d/%d other)",
          h_rate / BASE_FRAMERATE, step_bad, frame_bad);
}


/* ------------------------------------------- T03/T07: timing agreement */

/* T03: requests one after another give the expected rate for this
 * peer (client scale, server setting, frames accepted per second) and keep
 * the capability state; a new peer gets one SVS_EXACTFPS before every
 * SVS_FPS, the others none */
static void t03_rates(void)
{
    if (!h_join()) return;
    client_t *sc = h_sv_client();
    CHECK(sc != NULL, "server slot of the client");
    if (!sc) return;
    int n = sc->number;
    if (h_rate != BASE_FRAMERATE)
        CHECK(h_expect_exact() ? h_rep_cap[n] == 1 : !h_rep_cap[n], "join: %d capability replies", h_rep_cap[n]);
    static const int req[] = { 10, 20, 30, 40, 50, 60, 0 };    /* 0 was the join */
    for (size_t k = 0; k < q_countof(req); k++) {
        int u = req[k], want = h_expect_rate(u), cap0 = h_rep_cap[n], fps0 = h_rep_fps[n];
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "cl_updaterate %d", u);
        h_cmd(cmd);
        int sent = !(k == 0 && u == h_update);  /* an unchanged cvar sends no setting */
        for (int i = 0; sent && i < 2000 && !(H_DIV * BASE_FRAMERATE == want && (h_rep_fps[n] > fps0 || !USE_FPS)); i++)
            h_tick(1);
        h_run(300);
        int f0 = h_nframes;
        h_run(1000);
        int got = h_nframes - f0, srv = BASE_FRAMERATE;
#if USE_FPS
        srv = sv.framerate / sc->framediv;
#endif
        CHECK(H_DIV * BASE_FRAMERATE == want && srv == want && abs(got - want) <= 1 && h_exact_ok(H_EXACT),
              "request %d: %d Hz expected; client %d Hz, server %d Hz, %d frames/s, exact %d",
              u, want, H_DIV * BASE_FRAMERATE, srv, got, H_EXACT);
        if (USE_FPS && h_rate != BASE_FRAMERATE)
            CHECK(h_rep_fps[n] - fps0 == sent && h_rep_cap[n] - cap0 == (sent && h_expect_exact()),
                  "request %d: replies SVS_FPS %d, SVS_EXACTFPS %d (sent %d)", u, h_rep_fps[n] - fps0, h_rep_cap[n] - cap0, sent);
    }
    CHECK(!h_rep_order_bad, "every SVS_FPS right after its SVS_EXACTFPS (%d out of order)", h_rep_order_bad);
    h_report_contract("requests");
}

/* T03: unknown setting indices are ignored both ways (CLS_MAX and 999 to
 * the server, SVS_MAX and 999 to the client); connection, frames, rate and
 * timeline go on. The capability indices lie outside the upstream ranges
 * (CLS_MAX and SVS_MAX as in master), so an old server drops CLS_EXACTFPS
 * (idx >= CLS_MAX) and an old client ignores SVS_EXACTFPS */
static void t03_unknown_setting(void)
{
    CHECK(CLS_MAX == CLS_NOFLARES + 1 && SVS_MAX == SVS_FPS + 1 && CLS_EXACTFPS >= CLS_MAX &&
          CLS_EXACTFPS <= INT16_MAX && SVS_EXACTFPS >= SVS_MAX,
          "capability indices outside the upstream ranges: CLS_EXACTFPS %d (CLS_MAX %d), SVS_EXACTFPS %d (SVS_MAX %d)",
          CLS_EXACTFPS, CLS_MAX, SVS_EXACTFPS, SVS_MAX);
    if (!h_join()) return;
    client_t *sc = h_sv_client();
    static const int clidx[] = { CLS_MAX, 999 }, svidx[] = { SVS_MAX, 999 };
    int div = H_DIV, exact = H_EXACT, inv = h_invalid, nf = h_nframes;
    for (int i = 0; i < 2; i++) {
        MSG_WriteByte(clc_setting);
        MSG_WriteShort(clidx[i]);
        MSG_WriteShort(1);
        MSG_FlushTo(&cls.netchan.message);
        if (sc) {
            MSG_WriteByte(svc_setting);
            MSG_WriteLong(svidx[i]);
            MSG_WriteLong(1);
            SV_ClientAddMessage(sc, MSG_RELIABLE | MSG_CLEAR);
        }
    }
    h_run(1000);
    CHECK(sc && cls.state == ca_active && h_invalid == inv && h_nframes - nf >= h_want_div() * BASE_FRAMERATE - 1 &&
          H_DIV == div && H_EXACT == exact && h_last(0)->delta > 0,
          "unknown clc/svc settings ignored: active, %d frames, %d invalid, div %d -> %d, exact %d -> %d",
          h_nframes - nf, h_invalid - inv, div, H_DIV, exact, H_EXACT);
    h_report_contract("unknown settings");
}

/* T03: a capability reply with a value other than the expected 1 leaves the
 * client in the old scale (the server did answer) */
static void t03_bad_reply(void)
{
    h_bad_reply = true;
    if (!h_join()) return;
    client_t *sc = h_sv_client();
    CHECK(sc && h_rep_cap[sc->number] == 1 && !H_EXACT && cls.state == ca_active,
          "SVS_EXACTFPS 0: %d replies sent, exact timeline %d, state %d", sc ? h_rep_cap[sc->number] : -1, H_EXACT, cls.state);
}

/* the frame where the client took the late setting: servertime goes on from
 * the frame before it (no reset, no jump past the keyframe alignment of the
 * new rate), the keyframe phase holds afterwards */
static void h_check_switch(int s, const char *what)
{
    if (s <= 0) {
        CHECK(0, "%s: no frame in the new scale", what);
        return;
    }
    h_frame_t *x = &h_frames[s - 1], *y = &h_frames[s];
    int rate = y->div * BASE_FRAMERATE, d = y->servertime - x->servertime;
    int bound = (y->number - x->number + y->div - 1) * ((1000 + rate - 1) / rate);
    CHECK(x->epoch == y->epoch && d > 0 && d <= bound,
          "%s: servertime %d -> %d over frames %d -> %d (+%d ms, at most %d)", what, x->servertime, y->servertime,
          x->number, y->number, d, bound);
    int bad = 0;
    for (int i = s; i < h_nframes; i++) {
        h_frame_t *f = &h_frames[i];
        int fn = f->number - f->serverdelta;
        if (f->keyservertime != f->servertime - f->servertime % BASE_FRAMETIME ||
            (fn % f->div == 0) != (f->servertime % BASE_FRAMETIME == 0))
            bad++;
    }
    CHECK(!bad, "%s: keyframe phase after the switch (%d frames off)", what, bad);
}

/* T07 at entry: the datagram with the capability reply is lost three
 * times; frames come in the old scale, the late setting moves the anchor */
static void t07_late_join(void)
{
    h_carrier_drop = 3;
    if (!h_join_ex(true)) return;
    int first_exact, legacy = h_epoch_legacy_frames(&first_exact);
    CHECK(h_carrier_dropped == 3 && legacy >= 1,
          "carrier lost %d times; %d frames parsed in the old scale before the late setting", h_carrier_dropped, legacy);
    h_check_switch(first_exact, "late entry");
    h_run(1000);
    CHECK(!h_invalid && cls.state == ca_active && h_last(0)->delta > 0, "no invalid delta (%d), delta frames go on", h_invalid);
    h_report_contract("late entry");
}

/* T07 on cl_updaterate: CMDS change the rate; the carrier of the
 * replies is lost DROPS times (with FRAG one fragment of a fragmented one) */
static void h_late_change(const char *cmds, int want_rate, int drops, bool frag, int changes)
{
    if (!h_join()) return;
    h_run(500);
    client_t *sc = h_sv_client();
    if (!sc) { CHECK(0, "server slot of the client"); return; }
    int n = sc->number, cap0 = h_rep_cap[n], fps0 = h_rep_fps[n], frag0 = h_frag_sent, inv = h_invalid;
    h_fps_at[n] = -1;
    h_carrier_drop = drops;
    h_frag_carrier = frag;
    h_fat = frag;
    h_cmd(cmds);
    for (int i = 0; i < 3000 && H_DIV * BASE_FRAMERATE != want_rate; i++) h_tick(1);
    h_run(1000);
    int s = -1;
    for (int i = h_fps_at[n] < 0 ? h_nframes : h_fps_at[n]; i < h_nframes; i++)
        if (h_frames[i].div * BASE_FRAMERATE == want_rate) { s = i; break; }
    int late = s < 0 ? 0 : s - h_fps_at[n];
    CHECK(h_carrier_dropped == drops && (!frag || h_frag_sent - frag0 >= 2),
          "carrier lost %d times%s (fragments sent %d)", h_carrier_dropped, frag ? ", a fragment" : "", h_frag_sent - frag0);
    CHECK(late >= 1, "%d frames parsed in the old scale after the server switched", late);
    CHECK(H_DIV * BASE_FRAMERATE == want_rate && H_EXACT && h_rep_cap[n] - cap0 == changes && h_rep_fps[n] - fps0 == changes,
          "late setting taken: %d Hz (want %d), exact %d, replies %d/%d for %d changes", H_DIV * BASE_FRAMERATE, want_rate,
          H_EXACT, h_rep_cap[n] - cap0, h_rep_fps[n] - fps0, changes);
    h_check_switch(s, "late rate change");
    CHECK(h_invalid == inv && cls.state == ca_active && h_last(0)->delta > 0,
          "no invalid delta (%d), delta frames go on", h_invalid - inv);
    h_report_contract("late rate change");
}

static void t07_late_rate(void)
{
    int u = h_other_request(h_update == 30 ? 60 : 30);
    char cmd[32];
    Q_snprintf(cmd, sizeof(cmd), "cl_updaterate %d", u);
    h_late_change(cmd, h_expect_rate(u), 3, false, 1);
}
static void t07_late_fragment(void)
{
    int u = h_other_request(20);
    char cmd[32];
    Q_snprintf(cmd, sizeof(cmd), "cl_updaterate %d", u);
    h_late_change(cmd, h_expect_rate(u), 1, true, 1);
}
/* several changes before the first reply, their carrier lost once */
static void t03_multi_change(void)
{
    int u = h_other_request(h_update == 30 ? 60 : 30);
    char cmd[64];
    /* every step changes the cvar, so each one sends a setting */
    Q_snprintf(cmd, sizeof(cmd), "cl_updaterate %d\ncl_updaterate %d\ncl_updaterate %d",
               h_update == 20 ? 50 : 20, u == 60 ? 30 : 60, u);
    h_late_change(cmd, h_expect_rate(u), 1, false, 3);
}

/* ------------------------------------------ T03/T07 mixed fleet */

/* CL_SERVE: real-time listen server on UDP with this client; CL_PEERS lists
 * the other processes as name:rate:replies (replies 1 = at least one
 * capability reply, 0 = none). All peers must be in the game together, each
 * written frames at its expected rate */
typedef struct { char name[32]; int rate, replies, got_rate, got_replies; } h_peer_t;

static int h_slot(const char *name)
{
    for (int i = 0; i < sv_maxclients->integer; i++)
        if (svs.client_pool[i].state == cs_spawned && !strcmp(svs.client_pool[i].name, name))
            return i;
    return -1;
}

static void fleet_host(void)
{
    h_peer_t peers[8] = { 0 };
    int np = 0;
    char list[256];
    Q_strlcpy(list, getenv("CL_PEERS") ? getenv("CL_PEERS") : "", sizeof(list));
    for (char *tok = strtok(list, ","); tok && np < 8; tok = strtok(NULL, ","), np++)
        if (sscanf(tok, "%31[^:]:%d:%d", peers[np].name, &peers[np].rate, &peers[np].replies) != 3) {
            printf("  FAIL bad CL_PEERS entry %s\n", tok);
            return;
        }
    if (!h_join()) return;
    int together = 0;
    for (unsigned t_end = Sys_Milliseconds() + 60000; together < 8 && Sys_Milliseconds() < t_end; ) {
        /* frames_sent counts the frames built for a client, every protocol */
        unsigned sent0[MAX_CLIENTS];
        int in0 = 0, in1 = 0, slot[8];
        for (int i = 0; i < sv_maxclients->integer; i++) sent0[i] = svs.client_pool[i].frames_sent;
        for (int i = 0; i < np; i++) in0 += h_slot(peers[i].name) >= 0;
        unsigned w0 = Sys_Milliseconds();
        h_run(1000);
        unsigned dt = Sys_Milliseconds() - w0;
        for (int i = 0; i < np; i++) in1 += (slot[i] = h_slot(peers[i].name)) >= 0;
        if (in0 == np && in1 == np) {
            together++;
            for (int i = 0; i < np; i++) {
                peers[i].got_rate = (int)(svs.client_pool[slot[i]].frames_sent - sent0[slot[i]]) * 1000 / (int)dt;
                peers[i].got_replies = h_rep_cap[slot[i]];
            }
            INFO("window %d ms:%s", dt, "");
            for (int i = 0; i < np; i++)
                INFO("  %s: %d frames/s, %d capability replies", peers[i].name, peers[i].got_rate, peers[i].got_replies);
        }
        if (together && !in1)
            break;      /* the peers left (a demo peer leaves to play its demo) */
    }
    /* up to 8 s with all peers in: the window of the last one fits */
    CHECK(together >= 3, "all %d peers in the game together with the host (%d windows of 1 s)", np, together);
    for (int i = 0; i < np; i++)
        CHECK(abs(peers[i].got_rate - peers[i].rate) <= peers[i].rate / 4 + 1 &&
              (peers[i].replies ? peers[i].got_replies >= 1 : !peers[i].got_replies),
              "%s: written %d frames/s (expected %d), %d capability replies (expected %s)", peers[i].name,
              peers[i].got_rate, peers[i].rate, peers[i].got_replies, peers[i].replies ? "some" : "none");
    CHECK(together && H_DIV == h_want_div() && h_exact_ok(H_EXACT), "host client at %d Hz, exact %d", H_CRATE, H_EXACT);
    h_report_contract("host");
    h_cmd("killserver");    /* the peers leave on the disconnect */
    h_run(200);
}

/* CL_CONNECT: a peer of fleet_host; accepts frames at its expected rate in its own
 * scale; with CL_PEER_DEMO records a demo, which must play back; with
 * CL_PEER_LEAVE leaves after the measurement (a host that is not fleet_host) */
static void fleet_peer(void)
{
    if (!h_join()) return;
    CHECK(!getenv("CL_PROTOCOL") || cls.serverProtocol == atoi(getenv("CL_PROTOCOL")),
          "protocol %d (requested %s)", cls.serverProtocol, getenv("CL_PROTOCOL") ? getenv("CL_PROTOCOL") : "default");
    int f0 = h_nframes, want = h_expect_rate(h_update);
    unsigned w0 = Sys_Milliseconds();
    h_run(3000);
    int got = (h_nframes - f0) * 1000 / (int)(Sys_Milliseconds() - w0);
    /* real time: within a quarter, enough to tell 10, 20, 30 and 60 apart */
    CHECK(abs(got - want) <= want / 4 + 1, "%d frames/s accepted (expected %d)", got, want);
    CHECK(!h_invalid, "no invalid frames (%d)", h_invalid);
    h_report_contract("peer");
    if (getenv("CL_PEER_DEMO")) {
        h_cmd("record h_peer_demo");
        h_run(2000);
        h_cmd("stop");
        h_run(50);
        h_cmd("demo h_peer_demo");
        for (int i = 0; i < 10000 && !(cls.demo.playback && cls.state == ca_active); i++) h_tick(1);
        int first = h_nframes, step_bad = 0, n = 0;
        h_run(1500);
        for (int i = first + 1; i < h_nframes; i++) {
            if (!h_frames[i].playback || !h_frames[i - 1].playback || h_frames[i].number != h_frames[i - 1].number + 1)
                continue;
            n++;
            if (h_frames[i].servertime - h_frames[i - 1].servertime != BASE_FRAMETIME) step_bad++;
        }
        CHECK(cls.demo.playback && n >= 10 && !step_bad, "demo recorded without a capability reply plays (%d frames, %d not 100 ms apart)",
              n, step_bad);
        h_cmd("disconnect");
        return;
    }
    if (getenv("CL_PEER_LEAVE")) {
        h_cmd("disconnect");
        return;
    }
    /* in the game until the host ends it, so that all peers overlap */
    for (int i = 0; i < 60000 && cls.state >= ca_connected; i++) h_tick(1);
    CHECK(cls.state < ca_connected, "left when the host ended the game");
}


/* ------------------------------------------- batch scenarios */

/* frame gaps of the local client in [first, h_nframes) within one epoch */
static int h_frame_gaps(int first)
{
    int gaps = 0;
    for (int i = first + 1; i < h_nframes; i++)
        if (h_frames[i].epoch == h_frames[i - 1].epoch && h_frames[i].number - h_frames[i - 1].number != 1)
            gaps++;
    return gaps;
}

#define H_HIST(h) (h)[0], (h)[1], (h)[2], (h)[3], (h)[4], (h)[5], (h)[6]

/* T01/T04/T05: callbacks at absolute deadlines floor(k * 1000 / f) for
 * f = 15/30/60/120/59.94 Hz. Each call runs the owed whole frames up to the
 * batch limit and carries the rest; with the limit sufficient no debt stays
 * after a call, so the world keeps real speed and the conservation
 * steps * 1000 + credit = credit0 + elapsed * R holds after every call.
 * External time (svs.realtime) and input (ClientThink per usercmd) are
 * counted once per call, not per step; the local client gets every frame. */
static void t05_batch_cadence(void)
{
    if (!h_join()) return;
    static const struct { int num, den; const char *name; } fs[] = {
        { 15, 1, "15" }, { 30, 1, "30" }, { 60, 1, "60" }, { 120, 1, "120" }, { 5994, 100, "59.94" },
    };
    for (size_t i = 0; i < q_countof(fs); i++) {
        const int window = 10000;
        for (int64_t k = 1, prev = 0, d; (d = k * 1000 * fs[i].den / fs[i].num) <= 1000; k++, prev = d)
            h_tick((int)(d - prev));    /* settle at this cadence */
        h_calls_reset();
        h_calls_on = true;
        uint64_t c0 = sv.framecredit, r = SV_FRAMERATE;
        int s0 = h_steps_total, f0 = h_nframes, cmd0 = cl.cmdNumber, th0 = h_thinks, debt = 0, cons = 0;
        unsigned rt0 = svs.realtime, drop0 = cls.netchan.total_dropped;
        int64_t el = 0;
        for (int64_t k = 1, prev = 0, d; (d = k * 1000 * fs[i].den / fs[i].num) <= window; k++, prev = d) {
            h_tick((int)(d - prev));
            el = d;
            debt += sv.framecredit >= FRAME_CREDIT;
            cons += (uint64_t)(h_steps_total - s0) * FRAME_CREDIT + sv.framecredit != c0 + (uint64_t)el * r;
        }
        h_calls_on = false;
        int steps = h_steps_total - s0, cmds = cl.cmdNumber - cmd0, thinks = h_thinks - th0;
        CHECK(h_call_max <= H_BATCH && !h_call_bad,
              "%s Hz callbacks at %d Hz: steps per call by the policy, at most %d (max %d; calls with 0..5,6+ steps %d %d %d %d %d %d %d; "
              "bad %d: %s)", fs[i].name, (int)r, H_BATCH, h_call_max, H_HIST(h_call_hist), h_call_bad, h_call_first);
        CHECK(!debt && !cons, "%s Hz: world at real speed, %d steps in %d ms (%" PRIu64 " owed), no debt after a call "
              "(%d calls with debt, %d conservation errors)", fs[i].name, steps, (int)el, (c0 + (uint64_t)el * r) / FRAME_CREDIT,
              debt, cons);
        CHECK(svs.realtime - rt0 == el && abs(thinks - cmds) <= 2,
              "%s Hz: external time counted once (realtime +%u for %d ms), input once (%d ClientThink for %d usercmds)",
              fs[i].name, svs.realtime - rt0, (int)el, thinks, cmds);
        CHECK(!h_frame_gaps(f0) && cls.netchan.total_dropped == drop0 && h_lp_max <= 1 + H_BATCH,
              "%s Hz: the local client got every frame of the batches (%d frames, %d gaps, %u dropped, "
              "up to %d datagrams per call)", fs[i].name, h_nframes - f0, h_frame_gaps(f0),
              cls.netchan.total_dropped - drop0, h_lp_max);
    }
    h_report_contract("cadences");
}

/* T01/T05: jitter and hitches (250 ms kept, 400 ms -> 100 ms by
 * Qcommon_Frame), then a hidden tab (100 ms and 1000 ms callbacks) and back
 * to the foreground: steps per call by the policy, the owed time bounded by
 * the hitch policy and caught up within a few foreground calls */
static void t05_batch_hitch(void)
{
    if (!h_join()) return;
    static const int jitter[] = { 16, 17, 50, 3, 90, 16, 120, 250, 16, 17, 40, 8, 200, 16, 33, 1, 66, 67, 67, 400, 16, 17, 16 };
    unsigned drop0 = cls.netchan.total_dropped;
    h_calls_reset();
    h_calls_on = true;
    for (int round = 0; round < 3; round++)
        for (size_t i = 0; i < q_countof(jitter); i++)
            h_tick(jitter[i]);
    CHECK(h_call_max <= H_BATCH && !h_call_bad,
          "jitter and hitches at %d Hz: steps per call by the policy, at most %d (max %d; calls with 0..5,6+ steps %d %d %d %d %d %d %d; bad %d: %s)",
          (int)SV_FRAMERATE, H_BATCH, h_call_max, H_HIST(h_call_hist), h_call_bad, h_call_first);
    if (H_BATCH > 1 && SV_FRAMERATE >= 60)
        CHECK(h_call_hist[0] && h_call_hist[H_BATCH], "calls with 0 and with %d steps occur (%d, %d)", H_BATCH,
              h_call_hist[0], h_call_hist[H_BATCH]);

    uint64_t top = 0, bound = 250 * (uint64_t)SV_FRAMERATE + FRAME_CREDIT;
    for (int i = 0; i < 40; i++) {
        h_tick(i < 20 ? 100 : 1000);
        top = max(top, sv.framecredit);
    }
    int back = 0;
    while (back < 50 && sv.framecredit >= FRAME_CREDIT) {
        h_tick(16 + back % 2);
        back++;
    }
    h_calls_on = false;
    CHECK(h_call_max <= H_BATCH && !h_call_bad && top < bound,
          "hidden tab (100 and 1000 ms callbacks): steps by the policy, at most %d per call (max %d; bad %d: %s), owed time bounded "
          "(max credit %" PRIu64 " < %" PRIu64 ")", H_BATCH, h_call_max, h_call_bad, h_call_first, top, bound);
    int limit = H_BATCH > 1 ? (int)((top / FRAME_CREDIT + H_BATCH - 2) / (H_BATCH - 1)) + 1 : 0;
    CHECK(sv.framecredit < FRAME_CREDIT && back <= max(limit, 1),
          "back in the foreground: the debt is caught up in %d calls of 16/17 ms (at most %d)", back, max(limit, 1));
    CHECK(cls.netchan.total_dropped == drop0 && h_lp_max <= 1 + H_BATCH,
          "no loss on the local loopback (%u dropped, up to %d datagrams per call)",
          cls.netchan.total_dropped - drop0, h_lp_max);
    h_report_contract("hitches");
}

/* T01/T04 (pause baseline): the listen server pauses with the local
 * client alone; no world step while paused, the server clock and the
 * connection keep going, the paused time is not banked as debt (also in a
 * hidden tab) and after resume the world runs at real speed without a burst */
static void t05_pause(void)
{
    h_mvd = false;  /* pause needs the local client alone */
    if (!h_join()) return;
    h_cmd("pause");
    for (int i = 0; i < 20 && !sv_paused->integer; i++)
        h_tick(16);     /* check_paused runs when a step is due */
    CHECK(sv_paused->integer && cl_paused->integer, "pause engaged (sv_paused %d, cl_paused %d)", sv_paused->integer,
          cl_paused->integer);
    if (!sv_paused->integer) return;
    unsigned rt0 = svs.realtime;
    int s0 = h_steps_total, debt = 0, el = 0;
    for (int i = 0; i < 90; i++) {
        int ms = i < 40 || i >= 60 ? 16 + i % 2 : 100;  /* foreground, hidden, foreground */
        h_tick(ms);
        el += ms;
        debt += sv.framecredit >= FRAME_CREDIT;
    }
    CHECK(h_steps_total == s0 && sv_paused->integer && cls.state == ca_active,
          "no world step in %d ms of pause (%d), client still in the game", el, h_steps_total - s0);
    CHECK(svs.realtime - rt0 == (unsigned)el, "server realtime keeps going while paused (+%u for %d ms)", svs.realtime - rt0, el);
    CHECK(!debt, "paused time is not banked as debt (%d calls with debt, credit %" PRIu64 ")", debt, sv.framecredit);

    h_cmd("pause");
    for (int i = 0; i < 20 && sv_paused->integer; i++)
        h_tick(16);
    CHECK(!sv_paused->integer, "resumed (sv_paused %d)", sv_paused->integer);
    h_calls_reset();
    h_calls_on = true;
    uint64_t c0 = sv.framecredit, r = SV_FRAMERATE;
    int s1 = h_steps_total;
    for (int64_t k = 1, prev = 0, d; (d = k * 1000 * 100 / 5994) <= 2000; k++, prev = d)
        h_tick((int)(d - prev));
    h_calls_on = false;
    int steps = h_steps_total - s1, want = (int)((c0 + 2000 * r) / FRAME_CREDIT), burst = (int)((17 * r + FRAME_CREDIT - 1) / FRAME_CREDIT);
    CHECK(!h_call_bad && h_call_max <= burst && abs(steps - want) <= 1,
          "after resume: no burst (max %d steps per call, a 17 ms call owes up to %d), world at real speed (%d steps in 2 s, %d owed; bad %d: %s)",
          h_call_max, burst, steps, want, h_call_bad, h_call_first);
    h_report_contract("pause");
}

static int h_cmd_interval(void) { return SV_FRAMERATE >= 30 ? 50 : 100; }

/* send "h_q TEXT" from the local client; true when the game queued it and
 * the engine executed it (h_ack) */
static bool h_queue(const char *text)
{
    int q0 = h_queued;
    char cmd[MAX_STRING_CHARS];
    Q_snprintf(cmd, sizeof(cmd), "cmd h_q %s", text);
    h_cmd(cmd);
    for (int i = 0; i < 200 && (h_queued == q0 || h_pending); i++)
        h_tick(h_cmd_interval());
    return h_queued > q0 && !h_pending;
}

/* the step order of a packet-queued command: batch builds stop the batch
 * before it; the fixed FPS build keeps the old order, where the step of
 * the same call precedes it (dedicated: Cbuf_Execute of the next SV_Frame) */
#define H_ORDER_OK(bad) (H_BATCH > 1 ? !(bad) : (bad) > 0)
#define H_ORDER_TEXT (H_BATCH > 1 ? "no step before it" : "fixed FPS order kept: the step of the same call precedes it")

/* T05: the command boundary. Packet-queued config/map/kick are not
 * preceded by a step; a map queued by the first step of a batch stops it; a
 * bind present before the call does not stop catch-up; a packet-queued map
 * behind an existing wait does not hold the world; the held debt is written off once and a new
 * map starts its own epoch */
static void t05_cmd_boundary(void)
{
    if (!h_join()) return;
    int iv = h_cmd_interval();
    uint64_t r = SV_FRAMERATE;

    /* packet-queued config: held, then caught up exactly once */
    for (int i = 0; i < 5; i++) h_tick(iv);
    h_calls_reset();
    h_calls_on = h_hold_ok = true;
    uint64_t c0 = sv.framecredit;
    int s0 = h_steps_total, bad0 = h_pending_bad;
    unsigned t0 = h_now;
    bool ok = h_queue("set h_cfg 1");
    for (int i = 0; i < 6; i++) h_tick(iv);
    uint64_t el = h_now - t0;
    CHECK(ok && Cvar_VariableInteger("h_cfg") == 1 && H_ORDER_OK(h_pending_bad - bad0),
          "packet-queued config: %s (steps while queued %d, calls held %d)", H_ORDER_TEXT, h_pending_bad - bad0, h_call_held);
    CHECK(!h_call_bad && (uint64_t)(h_steps_total - s0) * FRAME_CREDIT + sv.framecredit == c0 + el * r && sv.framecredit < FRAME_CREDIT,
          "held debt written off once: %d steps in %" PRIu64 " ms (%" PRIu64 " owed), credit %" PRIu64 " (bad %d: %s)",
          h_steps_total - s0, el, (c0 + el * r) / FRAME_CREDIT, sv.framecredit, h_call_bad, h_call_first);

    /* a bind present before the call: no hold */
    h_hold_ok = false;
    h_calls_reset();
    h_cmd("wait\nwait\nwait\nset h_bind 1");
    for (int i = 0; i < 10; i++) h_tick(iv);
    CHECK(Cvar_VariableInteger("h_bind") == 1 && !h_call_bad && !h_call_held,
          "a bind with waits present before the call does not stop catch-up (held %d, bad %d: %s)", h_call_held, h_call_bad,
          h_call_first);
    h_calls_on = false;

    /* a map queued by the first step of a batch */
    for (int i = 0; i < 5; i++) h_tick(iv);
    bad0 = h_pending_bad;
    h_game_map = 1;
    int spawn = -1;
    uint64_t owed = 0;
    for (int i = 0; i < 200 && (h_game_map || h_pending); i++) {
        h_tick(iv);
        if (!h_pending && !h_game_map) { spawn = h_last_steps; owed = h_c_out; }
    }
    CHECK(!h_game_map && !h_pending && h_pending_bad == bad0 && spawn == 1 && sv.framecredit == 0,
          "map queued by the first step of a batch: no further step of the old map (%d), the call ran %d step(s), "
          "the new map starts at credit %" PRIu64 " (old map owed %" PRIu64 ")", h_pending_bad - bad0, spawn, sv.framecredit, owed);
    CHECK(h_wait_active(5000), "back in the game after the map change");
    h_run(200);

    /* an existing wait plus a packet-queued map */
    h_cmd(va("cmd h_q map %s\nwait 5", h_map()));
    bad0 = h_pending_bad;
    int q0 = h_queued;
    for (int i = 0; i < 200 && (h_queued == q0 || h_pending); i++) h_tick(iv);
    CHECK(h_queued > q0 && !h_pending && h_wait_at_queue > 0 && h_pending_bad - bad0 > 0 && sv.framecredit == 0,
          "packet-queued map behind an existing wait (%d frames) runs, the world steps while it waits (%d), new epoch at credit %" PRIu64,
          h_wait_at_queue, h_pending_bad - bad0, sv.framecredit);
    CHECK(h_wait_active(5000), "back in the game after the map change");
    h_run(200);

    /* packet-queued kick of the local client */
    bad0 = h_pending_bad;
    ok = h_queue("kick h_client");
    for (int i = 0; i < 10; i++) h_tick(iv);
    CHECK(ok && H_ORDER_OK(h_pending_bad - bad0) && !h_sv_client(),
          "packet-queued kick: %s (steps while queued %d), the client is gone", H_ORDER_TEXT, h_pending_bad - bad0);
}

/* T05: ERR_DROP from a step inside a batch leaves through the error
 * boundary of Qcommon_Frame: no later step or send of that server, and the
 * next map does not consume the debt of the old one */
static void t05_batch_error(void)
{
    if (!h_join()) return;
    for (int i = 0; i < 5; i++) h_tick(16);
    int at = min(2, H_BATCH);
    h_game_error = at;
    h_tick(250);
    CHECK(h_error_fired == at && h_last_steps == at && !h_c_done && !svs.initialized,
          "ERR_DROP in step %d of a batch: %d step(s) ran, SV_Frame left by the error boundary, server shut down", at, h_last_steps);
    int steps = 0, sends = 0;
    for (int i = 0; i < 20; i++) {
        h_tick(50);
        steps += h_last_steps;
        sends += h_lp_sends;
    }
    CHECK(!steps && !sends && cls.state < ca_connected, "no step or send after the error (%d steps, %d sends, client state %d)",
          steps, sends, cls.state);
    h_cmd(va("map %s", h_map()));
    h_tick(16);
    CHECK(svs.initialized && sv.framecredit == 0, "the next map starts at credit %" PRIu64 " (no old debt)", sv.framecredit);
    h_calls_reset();
    h_calls_on = true;
    CHECK(h_wait_active(5000), "back in the game on the new map");
    h_run(500);
    h_calls_on = false;
    CHECK(!h_call_bad && h_call_max <= H_BATCH, "steps by the policy on the new map (bad %d: %s)", h_call_bad, h_call_first);
}

/* T06: the real loopback queue. Bursts up to the capacity (8 with variable
 * FPS, 4 kept without) arrive in order and intact in both
 * directions across many index wraps; an intentional overflow keeps the
 * newest messages in order and is counted */
static int h_lp_got, h_lp_bad;
static uint32_t h_lp_next, h_lp_first;
static size_t h_lp_len(uint32_t seq) { return 8 + (seq * 523u) % (MAX_PACKETLEN - 8); }
static void h_lp_cb(void)
{
    if (net_from.type != NA_LOOPBACK) return;
    uint32_t seq = RL32(msg_read.data);
    bool ok = msg_read.cursize == h_lp_len(seq) && (!h_lp_got ? (h_lp_first = seq, true) : seq == h_lp_next);
    for (size_t i = 4; ok && i < msg_read.cursize; i++)
        ok = msg_read.data[i] == (byte)(seq * 31 + i);
    h_lp_bad += !ok;
    h_lp_next = seq + 1;
    h_lp_got++;
}
static void h_lp_drop(void) {}
static void h_lp_send(netsrc_t from, uint32_t seq)
{
    static byte buf[MAX_PACKETLEN];
    netadr_t to = { .type = NA_LOOPBACK };
    size_t len = h_lp_len(seq);
    WL32(buf, seq);
    for (size_t i = 4; i < len; i++)
        buf[i] = (byte)(seq * 31 + i);
    NET_SendPacket(from, buf, len, &to);
}
static int h_lp_burst(netsrc_t from, uint32_t *seq, int n)
{
    h_lp_got = 0;
    for (int i = 0; i < n; i++)
        h_lp_send(from, (*seq)++);
    NET_GetPackets(from ^ 1, h_lp_cb);
    return h_lp_got;
}

static void t06_loopback(void)
{
    h_init();
    NET_GetPackets(NS_CLIENT, h_lp_drop);
    NET_GetPackets(NS_SERVER, h_lp_drop);
    const int cap = USE_FPS ? 8 : 4;
    for (netsrc_t from = NS_CLIENT; from <= NS_SERVER; from++) {
        const char *dir = from == NS_SERVER ? "server -> client" : "client -> server";
        uint32_t seq = 0x10000 * (from + 1);
        int lost = 0, sent = 0, bursts = 0;
        h_lp_bad = 0;
        static const int sizes[] = { 8, 1, 7, 3, 8, 2, 8, 8, 5, 6, 4 };
        for (int round = 0; round < 40; round++)
            for (size_t i = 0; i < q_countof(sizes); i++) {
                int n = min(sizes[i], cap);
                lost += n - h_lp_burst(from, &seq, n);
                sent += n;
                bursts++;
            }
        CHECK(!lost && !h_lp_bad,
              "%s: %d messages in %d bursts of up to %d (capacity) arrive in order and intact across index wraps "
              "(lost %d, bad %d)", dir, sent, bursts, cap, lost, h_lp_bad);
        uint32_t start = seq;
        int got = h_lp_burst(from, &seq, cap + 3);
        CHECK(got == cap && !h_lp_bad && h_lp_first == start + 3,
              "%s: intentional overflow by 3: the oldest 3 dropped, the newest %d of %d kept in order (got %d, first %u)",
              dir, cap, cap + 3, got, h_lp_first - start);
        CHECK(h_lp_burst(from, &seq, cap) == cap && !h_lp_bad, "%s: the queue works after the overflow", dir);
    }
}

/* T06: the actual burst of a batch on the local loopback: the batch
 * limit with a fragmented reliable (one fragment per step plus one async
 * fragment) reaches the capacity without loss; packetdup is not compiled
 * in this configuration (numpackets 1), as in the product builds */
static void t06_loopback_burst(void)
{
    if (!h_join()) return;
    client_t *c = h_sv_client();
    CHECK(c && c->numpackets == 1 && !USE_PACKETDUP,
          "packetdup hack compiled %d, numpackets %d: one datagram per send", USE_PACKETDUP, c ? c->numpackets : -1);
    if (!c) return;
    const int cap = USE_FPS ? 8 : 4;
    unsigned drop0 = cls.netchan.total_dropped;
    int frag0 = h_frag_sent;
    h_calls_reset();
    h_calls_on = true;
    for (int round = 0; round < 5; round++) {
        h_fill_reliable(c);     /* fills the packet, 16 more lines make it ~5 fragments */
        for (int i = 0; i < 16; i++)
            SV_ClientPrintf(c, PRINT_HIGH, "%01000d\n", round);
        h_tick(250);
        for (int i = 0; i < 30; i++) h_tick(16 + i % 2);
    }
    h_calls_on = false;
    CHECK(h_frag_sent > frag0 && cls.netchan.total_dropped == drop0 && !h_call_bad,
          "fragmented reliables in batches: %d fragments, %u dropped (bad %d: %s)",
          h_frag_sent - frag0, cls.netchan.total_dropped - drop0, h_call_bad, h_call_first);
    /* a client below the server rate gets a send on its own frames only */
    bool full = H_BATCH > 1 && SV_FRAMERATE >= 60 && !h_update;
    CHECK(h_lp_max <= 1 + H_BATCH && h_lp_max <= cap && (!full || h_lp_max >= H_BATCH),
          "largest burst %d datagrams per call (%d world sends + 1 async at most, capacity %d%s)", h_lp_max, H_BATCH, cap,
          full ? ", the batch limit reached" : "");
    CHECK(cls.state == ca_active && cl.frame.valid, "client in the game after the bursts");
}

/* T06 with the packetdup hack compiled (a build of its own, which runs only
 * this scenario): the server refuses packetdup to the local client, whose
 * loopback loses nothing; two copies of every send of a full batch would
 * overflow the loopback queue and lose a frame */
static void t06_packetdup_loopback(void)
{
    CHECK(USE_PACKETDUP, "packetdup hack compiled in this build");
    if (!h_join()) return;
    client_t *c = h_sv_client();
    if (!c) return;
    const int cap = USE_FPS ? 8 : 4;
    Cvar_Set("sv_packetdup_hack", "1");
    h_cmd("cmd packetdup 1");
    h_run(100);
    CHECK(c->numpackets == 1, "packetdup 1 refused on loopback (numpackets %d)", c->numpackets);
    unsigned drop0 = cls.netchan.total_dropped;
    int f0 = h_nframes;
    h_calls_reset();
    h_calls_on = true;
    for (int round = 0; round < 5; round++) {
        h_tick(250);
        for (int i = 0; i < 30; i++) h_tick(16 + i % 2);
    }
    h_calls_on = false;
    CHECK(!h_frame_gaps(f0) && cls.netchan.total_dropped == drop0 && h_lp_max <= cap,
          "full batches: every frame arrives (%d frames, %d gaps, %u dropped, up to %d datagrams per call, capacity %d)",
          h_nframes - f0, h_frame_gaps(f0), cls.netchan.total_dropped - drop0, h_lp_max, cap);
    CHECK(cls.state == ca_active && cl.frame.valid, "client in the game after the batches");
}

typedef struct { const char *name; void (*run)(void); int rated_only, kinds; } scenario_t;
static const scenario_t h_scenarios[] = {
    { "t02_timeline", t02_timeline, 0, K_NEW },
    { "t02_nolerp", t02_nolerp, 0, K_NEW },
    { "t07_parser", t07_parser, 0, K_NEW },
    { "t02_rate_change", t02_rate_change, 1, K_NEW },
    { "t02_legacy_setting_overflow", t02_legacy_setting_overflow, 1, K_OLDSV },
    { "t02_large_framenum", t02_large_framenum, 0, K_NEW },
    { "t02_epoch_reset", t02_epoch_reset, 0, K_NEW },
    { "t08_demo", t08_demo, 0, K_NEW },
    { "t08_mvd", t08_mvd, 0, K_NEW },
    { "t03_rates", t03_rates, 0, K_NEW | K_LEGACY | K_OLDSV },
    { "t03_unknown_setting", t03_unknown_setting, 0, K_NEW },
    { "t03_bad_reply", t03_bad_reply, 1, K_NEW },
    { "t07_late_join", t07_late_join, 1, K_NEW },
    { "t07_late_rate", t07_late_rate, 1, K_NEW },
    { "t07_late_fragment", t07_late_fragment, 1, K_NEW },
    { "t03_multi_change", t03_multi_change, 1, K_NEW },
    { "t05_batch_cadence", t05_batch_cadence, 0, K_NEW },
    { "t05_batch_hitch", t05_batch_hitch, 0, K_NEW },
    { "t05_pause", t05_pause, 0, K_NEW },
    { "t05_cmd_boundary", t05_cmd_boundary, 0, K_NEW },
    { "t05_batch_error", t05_batch_error, 0, K_NEW },
    { "t06_loopback", t06_loopback, 0, K_NEW },
    { "t06_loopback_burst", t06_loopback_burst, 0, K_NEW },
    { "t06_packetdup_loopback", t06_packetdup_loopback, 0, K_NEW },  /* listed only in a packetdup build */
    { "fleet_host", fleet_host },     /* by name only, with CL_SERVE */
    { "fleet_peer", fleet_peer },     /* by name only, with CL_CONNECT */
    { NULL, NULL }
};

int main(int argc, char **argv)
{
    (void)q2pro_real_main;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <scenario>|list\n", argv[0]); return 2; }
    if (getenv("CL_RATE")) h_rate = atoi(getenv("CL_RATE"));
    if (getenv("CL_UPDATERATE")) h_update = atoi(getenv("CL_UPDATERATE"));
    const char *kind = getenv("CL_KIND");
    static const char *const kinds[] = { "new", "legacy", "oldsv", "nofpssv" };
    for (int i = 0; kind && i < (int)q_countof(kinds); i++)
        if (!strcmp(kind, kinds[i])) h_kind = 1 << i, kind = NULL;
    if (kind) { fprintf(stderr, "unknown CL_KIND %s\n", kind); return 2; }
    h_real = getenv("CL_SERVE") || getenv("CL_CONNECT");
    if (h_rate < 10 || h_rate % 10) { fprintf(stderr, "CL_RATE must be a multiple of 10\n"); return 2; }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; h_scenarios[i].name; i++)
            if ((h_rate != 10 || !h_scenarios[i].rated_only) && (h_scenarios[i].kinds & h_kind) &&
                (h_scenarios[i].run == t06_packetdup_loopback) == !!USE_PACKETDUP)
                printf("%s\n", h_scenarios[i].name);
        return 0;
    }
    for (int i = 0; h_scenarios[i].name; i++) {
        if (strcmp(h_scenarios[i].name, argv[1])) continue;
        printf("client %s @%d Hz updaterate %d kind %s%s\n", argv[1], h_rate, h_update,
               getenv("CL_KIND") ? getenv("CL_KIND") : "new", USE_FPS ? "" : " (no USE_FPS)");
        h_scenarios[i].run();
        if (getenv("CL_VERBOSE"))
            for (int j = 0; j < h_nframes && j < (int)q_countof(h_frames); j++)
                printf("  frame %d delta %d old %d flags %#x sd %d st %d key %d div %d ep %d pb %d ent %d x %d/%d lerp %d psdup %d\n",
                       h_frames[j].number, h_frames[j].delta, h_frames[j].oldnumber, h_frames[j].flags,
                       h_frames[j].serverdelta, h_frames[j].servertime, h_frames[j].keyservertime, h_frames[j].div,
                       h_frames[j].epoch, h_frames[j].playback, h_frames[j].ent_frame, h_frames[j].ent_prev_x,
                       h_frames[j].ent_x, h_frames[j].ent_lerp, h_frames[j].ps_dup);
        printf("client %s: %s\n", argv[1], h_failures ? "FAIL" : "PASS");
        fflush(stdout);
        _exit(h_failures ? 1 : 0);  /* skip engine atexit/shutdown */
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}
