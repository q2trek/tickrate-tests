/*
 * Scheduler harness: T01/T04/T05 baseline at 10 Hz; with SCHED_RATE=R the
 * rate-generic scenarios run at R Hz (variable-FPS engine build).
 *
 * Links the unmodified object files of a plain native q2proded build
 * (system.c.o gets main renamed); time and the frame boundary are observed
 * through linker wraps that pass through to the real code:
 *   Sys_Milliseconds  -> virtual clock (the only substitution)
 *   NET_Sleep         -> records the sleep budget, then real NET_Sleep(0)
 *   SV_Frame          -> records msec in / budget out, calls the real one
 *   MSG_WriteDeltaEntity -> records entity 1 events written to the wire
 * The game is a real dlopen'd API3 library (testgame.c) loaded with
 * sys_forcegamelib; a fake protocol-34 client talks to the real UDP socket.
 *
 * usage: driver <scenario>|list   (env SCHED_GAME, SCHED_BASEDIR, SCHED_RATE)
 */
#include "src/server/server.h"   /* via -I<q2pro> from the engine compile command */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int q2pro_real_main(int argc, char **argv);

static int h_failures;
static int h_rate = 10;             /* SCHED_RATE: requested world rate */
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

/* ---------------------------------------------------------------- wraps */

static unsigned h_now = 1000000;
static int h_sleep = -1;            /* argument of the last NET_Sleep */
static unsigned h_sv_in, h_sv_out;  /* last SV_Frame msec and budget */
static int h_calls;                 /* completed-or-aborted Qcommon_Frame calls */

unsigned __wrap_Sys_Milliseconds(void) { return h_now; }

int __real_NET_Sleep(int msec);
int __wrap_NET_Sleep(int msec)
{
    h_sleep = msec;
    return __real_NET_Sleep(0);
}

enum { H_EV_SPAWN = 1, H_EV_RUNFRAME, H_EV_THINK, H_EV_BEGIN, H_EV_SHUTDOWN, H_EV_WIRE };
static const char *const h_evname[] = { "?", "SPAWN", "RF", "THINK", "BEGIN", "SHUTDOWN", "WIRE" };

typedef struct { int kind, a, b, call, framenum; } h_event_t;
static h_event_t h_log[1 << 16];
static int h_nlog;

static void h_record(int kind, int a, int b)
{
    if (h_nlog < (int)(sizeof(h_log) / sizeof(h_log[0])))
        h_log[h_nlog++] = (h_event_t){ kind, a, b, h_calls, sv.framenum };
}

unsigned __real_SV_Frame(unsigned msec);
unsigned __wrap_SV_Frame(unsigned msec)
{
    h_sv_in = msec;
    h_sv_out = __real_SV_Frame(msec);
    return h_sv_out;
}

void __real_MSG_WriteDeltaEntity(const entity_packed_t *from, const entity_packed_t *to, msgEsFlags_t flags);
void __wrap_MSG_WriteDeltaEntity(const entity_packed_t *from, const entity_packed_t *to, msgEsFlags_t flags)
{
    if (to && to->number == 1 && to->event)
        h_record(H_EV_WIRE, to->event, 0);
    __real_MSG_WriteDeltaEntity(from, to, flags);
}

/* ------------------------------------------------------- game callbacks */

typedef void (*h_rf_hook_t)(game_import_t *gi, edict_t *edicts, int frame);
static h_rf_hook_t h_rf_hook;

void h_game_event(int kind, int a, int b) { h_record(kind, a, b); }

void h_game_runframe(game_import_t *gi, edict_t *edicts, int frame)
{
    if (h_rf_hook)
        h_rf_hook(gi, edicts, frame);
}

/* -------------------------------------------------------------- engine */

static int h_port;

static int h_free_port(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(a);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    getsockname(s, (struct sockaddr *)&a, &len);
    close(s);
    return ntohs(a.sin_port);
}

static void h_init(const char *map)
{
    static char port[16], rate[16], *argv[64];
    const char *game = getenv("SCHED_GAME"), *base = getenv("SCHED_BASEDIR");
    int argc = 0;

    if (!game || !base) {
        fprintf(stderr, "SCHED_GAME and SCHED_BASEDIR are required\n");
        exit(2);
    }
    h_port = h_free_port();
    snprintf(port, sizeof(port), "%d", h_port);
    snprintf(rate, sizeof(rate), "%d", h_rate);
#define ARG(...) do { const char *v_[] = { __VA_ARGS__ }; \
        for (size_t i_ = 0; i_ < sizeof(v_) / sizeof(v_[0]); i_++) argv[argc++] = (char *)v_[i_]; } while (0)
    ARG("q2proded", "+set", "basedir", base, "+set", "homedir", base, "+set", "sys_console", "0",
        "+set", "sys_forcegamelib", game, "+set", "net_ip", "127.0.0.1", "+set", "net_port", port,
        "+set", "net_enable_ipv6", "0", "+set", "maxclients", "4", "+set", "sv_enforcetime", "1",
        "+set", "logfile", "0", "+set", "developer", "0");
    if (h_rate != 10)
        ARG("+set", "sv_fps", rate);
    if (map)
        ARG("+map", map);
    else    /* any late command: keeps the dedicated_start alias of default.cfg (map base1) from running */
        ARG("+wait");
#undef ARG
    argv[argc] = NULL;
    Qcommon_Init(argc, argv);
    h_calls = 1;    /* events of Qcommon_Init carry call 0 */
    /* precondition: the requested rate really runs (variable-FPS build and
     * a game exporting GMF_VARIABLE_FPS), otherwise R Hz results are 10 Hz */
    if (map && SV_FRAMERATE != h_rate) {
        printf("  FAIL precondition: server runs %d Hz, requested %d Hz\n", (int)SV_FRAMERATE, h_rate);
        fflush(stdout);
        _exit(1);
    }
}

/* one external frame: `msec` of wall time pass, then the real Qcommon_Frame */
static bool h_frame_ok;
static void h_frame(unsigned msec);

/* ----------------------------------------------------------- fake client */

static int c_sock = -1;
static unsigned c_seq = 1, c_in_seq, c_in_rel;
static char c_oob[1024];

static void c_open(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    c_sock = socket(AF_INET, SOCK_DGRAM, 0);
    bind(c_sock, (struct sockaddr *)&a, sizeof(a));
    a.sin_port = htons(h_port);
    connect(c_sock, (struct sockaddr *)&a, sizeof(a));
}

static void c_pump(void)
{
    uint8_t buf[MAX_PACKETLEN];
    ssize_t n;

    if (c_sock < 0)
        return;
    while ((n = recv(c_sock, buf, sizeof(buf), MSG_DONTWAIT)) >= 4) {
        uint32_t seq = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
        if (seq == 0xffffffff) {
            size_t len = min((size_t)n - 4, sizeof(c_oob) - 1);
            memcpy(c_oob, buf + 4, len);
            c_oob[len] = 0;
            continue;
        }
        if (seq & 0x80000000)
            c_in_rel ^= 1;
        c_in_seq = seq & 0x7fffffff;
    }
}

static void c_oob_send(const char *text)
{
    char buf[1400];
    int n = snprintf(buf, sizeof(buf), "\xff\xff\xff\xff%s", text);
    send(c_sock, buf, n, 0);
}

typedef struct { uint8_t data[1400]; int len; } c_msg_t;
static void w8(c_msg_t *m, int v) { m->data[m->len++] = v; }
static void w16(c_msg_t *m, int v) { w8(m, v & 255); w8(m, (v >> 8) & 255); }
static void w32(c_msg_t *m, unsigned v) { w16(m, v & 0xffff); w16(m, v >> 16); }

static void c_packet(const c_msg_t *payload)
{
    c_msg_t m = { .len = 0 };
    w32(&m, c_seq++);
    w32(&m, c_in_seq | c_in_rel << 31);
    w16(&m, 1234);  /* qport */
    memcpy(m.data + m.len, payload->data, payload->len);
    m.len += payload->len;
    send(c_sock, m.data, m.len, 0);
}

static void c_stringcmd(const char *s)
{
    c_msg_t m = { .len = 0 };
    w8(&m, clc_stringcmd);
    memcpy(m.data + m.len, s, strlen(s) + 1);
    m.len += strlen(s) + 1;
    c_packet(&m);
}

/* clc_move of protocol 34: checksum (skipped by the server), lastframe,
 * oldest/oldcmd/newcmd; equal commands, so the two deltas carry only msec */
static void c_move(int msec, int buttons)
{
    c_msg_t m = { .len = 0 };
    w8(&m, clc_move);
    w8(&m, 0);
    w32(&m, (unsigned)-1);
    for (int i = 0; i < 3; i++) {
        w8(&m, i == 0 && buttons ? CM_BUTTONS : 0);
        if (i == 0 && buttons)
            w8(&m, buttons);
        w8(&m, msec);
        w8(&m, 0);  /* lightlevel */
    }
    c_packet(&m);
}

static client_t *c_client(void) { return &svs.client_pool[0]; }

/* connect through the real handshake and spawn; returns false on failure */
static bool c_join(void)
{
    char cmd[512];
    int challenge = -1;

    c_open();
    c_oob_send("getchallenge\n");
    for (int i = 0; i < 10 && challenge < 0; i++) {
        h_frame(0);
        if (!strncmp(c_oob, "challenge ", 10))
            challenge = atoi(c_oob + 10);
    }
    if (challenge < 0)
        return false;
    snprintf(cmd, sizeof(cmd), "connect 34 1234 %d \"\\name\\h1\\rate\\100000\"\n", challenge);
    c_oob_send(cmd);
    for (int i = 0; i < 10 && strncmp(c_oob, "client_connect", 14); i++)
        h_frame(0);
    if (strncmp(c_oob, "client_connect", 14))
        return false;
    c_stringcmd("\177c version harness");
    h_frame(0);
    c_stringcmd("new");
    h_frame(0);
    if (c_client()->state != cs_primed)
        return false;
    c_stringcmd("begin");
    h_frame(0);
    return c_client()->state == cs_spawned;
}

static void h_frame(unsigned msec)
{
    h_now += msec;
    Qcommon_Frame(); h_frame_ok = true;
    h_calls++;
    c_pump();
}

/* ------------------------------------------------------------- queries */

static int h_count(int kind, int from_event)
{
    int n = 0;
    for (int i = from_event; i < h_nlog; i++)
        n += h_log[i].kind == kind;
    return n;
}

static int h_count_call(int kind, int call)
{
    int n = 0;
    for (int i = 0; i < h_nlog; i++)
        n += h_log[i].kind == kind && h_log[i].call == call;
    return n;
}

/* events of one call as a short string, e.g. "THINK RF WIRE" */
static const char *h_trace(int call)
{
    static char buf[512];
    buf[0] = 0;
    for (int i = 0; i < h_nlog; i++)
        if (h_log[i].call == call && strlen(buf) < sizeof(buf) - 16)
            Q_strlcat(buf, va("%s%s", buf[0] ? " " : "", h_evname[h_log[i].kind]), sizeof(buf));
    return buf;
}

/* --------------------------------------------------------------- T01 */

/* whole ms until the n-th rational deadline n * 1000 / R: ceil */
static unsigned h_deadline(unsigned n) { return (n * 1000 + h_rate - 1) / h_rate; }

/* D(n) = ceil(n * 1000 / r), the deadline the scheduler budget pays to
 * (SV_FrameBudget); reference only, the engine has no helper for it */
static int64_t h_D(int64_t n, int r) { return n * 1000 / r + (n * 1000 % r > 0); }

/* the frame-time contract of inc/common/utils.h itself (one direct
 * parametrised check of an engine helper), for all
 * arithmetic rates 10..60 Hz, whatever SCHED_RATE is */
static void t01_contract(void)
{
    static const int rates[] = { 10, 20, 30, 40, 50, 60 };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        const int r = rates[i];
        int bad_t = 0, bad_d = 0, bad_inv = 0, bad_gap = 0;
        for (int64_t n = -3 * r; n <= 3 * r; n++) {
            /* T(n) = floor(n * 1000 / r): T <= exact < T + 1 */
            int64_t t = Com_FrameTime(n, r), d = h_D(n, r);
            bad_t += !(t * r <= n * 1000 && n * 1000 < (t + 1) * r);
            bad_d += !(d * r >= n * 1000 && n * 1000 > (d - 1) * r);
            bad_gap += d - t > 1;
            /* N(T(n)) = n: the inverse keeps the boundary frame */
            bad_inv += Com_FrameForTime(t, r) != n;
            /* N(t) is the smallest frame with T >= t */
            for (int64_t ms = t - 2; ms <= t; ms++) {
                int64_t f = Com_FrameForTime(ms, r);
                bad_inv += !(Com_FrameTime(f, r) >= ms && Com_FrameTime(f - 1, r) < ms);
            }
        }
        CHECK(!bad_t && !bad_d && !bad_gap, "%d Hz: T = floor, D = ceil of n * 1000 / R, D - T <= 1 (bad %d/%d/%d)",
              r, bad_t, bad_d, bad_gap);
        CHECK(!bad_inv, "%d Hz: inverse N(T(n)) = n and N(t) = smallest frame with T >= t (bad %d)", r, bad_inv);
        CHECK(Com_FrameTime(60 * r, r) == 60000 && h_D(60 * r, r) == 60000,
              "%d Hz: virtual minute = %d frames exactly", r, 60 * r);
        /* large frame numbers: the product is 64-bit (a 30-day map) */
        int64_t big = (int64_t)r * 86400 * 30 + 1;
        CHECK(Com_FrameTime(big, r) == (int64_t)86400 * 30 * 1000 + 1000 / r &&
              Com_FrameForTime(Com_FrameTime(big, r), r) == big, "%d Hz: frame %" PRId64 " (30 days) exact", r, big);
    }
    CHECK(Com_FrameTime(1, 60) == 16 && Com_FrameForTime(16, 60) == 1, "60 Hz: T(1) = 16 maps back to frame 1, not 0");
    int64_t t60[6], d60[6];
    for (int n = 1; n <= 6; n++) { t60[n - 1] = Com_FrameTime(n, 60); d60[n - 1] = h_D(n, 60); }
    CHECK(t60[0] == 16 && t60[1] == 33 && t60[2] == 50 && t60[3] == 66 && t60[4] == 83 && t60[5] == 100,
          "60 Hz T(1..6) = 16 33 50 66 83 100");
    CHECK(d60[0] == 17 && d60[1] == 34 && d60[2] == 50 && d60[3] == 67 && d60[4] == 84 && d60[5] == 100,
          "60 Hz D(1..6) = 17 34 50 67 84 100");
    CHECK(Com_FrameTime(-1, 60) == -17, "60 Hz: T(-1) = -17 (floor, not truncation)");
}

/* rate-generic; at 10 Hz: 50 / 49 / 1 ms, budgets 50 and 100 */
static void t01_early_call(void)
{
    h_init(h_map());
    const unsigned d1 = h_deadline(1), half = d1 / 2;
    int rf = h_count(H_EV_RUNFRAME, 0);
    h_frame(half);
    CHECK(h_count(H_EV_RUNFRAME, 0) == rf, "%u ms after spawn: no world step (RF %d)", half, h_count(H_EV_RUNFRAME, 0) - rf);
    CHECK(h_sv_out == d1 - half, "budget after early call = %u (got %u)", d1 - half, h_sv_out);
    h_frame(d1 - 1 - half);
    CHECK(h_count(H_EV_RUNFRAME, 0) == rf, "%u ms: still no step (RF %d)", d1 - 1, h_count(H_EV_RUNFRAME, 0) - rf);
    CHECK(h_sleep == (int)(d1 - half), "next call sleeps the previous budget %u (got %d)", d1 - half, h_sleep);
    h_frame(1);
    CHECK(h_count(H_EV_RUNFRAME, 0) == rf + 1, "%u ms: deadline reached, exactly 1 step (got %d)", d1, h_count(H_EV_RUNFRAME, 0) - rf);
    CHECK(h_sv_out == h_deadline(2) - d1, "budget after an on-time step = %u (got %u)", h_deadline(2) - d1, h_sv_out);
}

/* call idle (0 ms) frames while the server owes steps; returns the calls */
static int h_idle_while_owed(void)
{
    int n = 0;
    while (h_sv_out == 0 && n < 1000) {
        h_frame(0);
        n++;
    }
    return n;
}

/* rate-generic; at 10 Hz 4 x 30 ms: budgets 70/40/10/80, residual 20 kept */
static void t01_residual(void)
{
    h_init(h_map());
    const unsigned c = 300 / h_rate;
    int rf = h_count(H_EV_RUNFRAME, 0);
    unsigned cum = 0;
    for (int i = 0; i < 4; i++) {
        h_frame(c);
        cum += c;
        unsigned expect = h_deadline(h_count(H_EV_RUNFRAME, 0) - rf + 1) - cum;
        CHECK(h_sv_out == expect, "%u ms call %d: budget %u (got %u)", c, i + 1, expect, h_sv_out);
    }
    CHECK(h_count(H_EV_RUNFRAME, 0) - rf == 1, "4 x %u ms: 1 step, residual %u ms kept (steps %d)",
          c, 4 * c - h_deadline(1), h_count(H_EV_RUNFRAME, 0) - rf);
}

/* rate-generic: random partitions of 0..floor(1000/R) ms (at most one
 * owed step per call); at 10 Hz 0..100 ms, floor(elapsed / 100), 600 steps */
static void t01_partitions(void)
{
    h_init(h_map());
    const unsigned dmax = 1000 / h_rate;
    int rf0 = h_count(H_EV_RUNFRAME, 0), bad = 0, bad_budget = 0;
    unsigned cum = 0, seed = 12345, rt0 = svs.realtime;
    while (cum < 60000) {
        seed = seed * 1103515245 + 12345;
        unsigned d = (seed >> 16) % (dmax + 1);
        if (cum + d > 60000)
            d = 60000 - cum;
        h_frame(d);
        cum += d;
        int steps = h_count(H_EV_RUNFRAME, 0) - rf0;
        bad += steps != (int)(cum * h_rate / 1000);
        bad_budget += h_sv_out != h_deadline(steps + 1) - cum;
    }
    int steps = h_count(H_EV_RUNFRAME, 0) - rf0;
    CHECK(bad == 0, "after every call steps == floor(elapsed * %d / 1000) (%d mismatches over %d calls)", h_rate, bad, h_calls);
    CHECK(bad_budget == 0, "after every call budget == next rational deadline - elapsed (%d mismatches)", bad_budget);
    CHECK(steps == 60 * h_rate, "virtual minute in random 0..%u ms partitions: %d steps (got %d)", dmax, 60 * h_rate, steps);
    CHECK(svs.realtime - rt0 == 60000, "server realtime advanced once per elapsed ms: 60000 (got %u)", svs.realtime - rt0);
}

/* rate-generic: 2.5 periods in one call (250 ms at 10 Hz) */
static void t01_one_step_per_call(void)
{
    h_init(h_map());
    const unsigned d = 2500 / h_rate;
    int rf = h_count(H_EV_RUNFRAME, 0);
    h_frame(d);
    CHECK(h_count_call(H_EV_RUNFRAME, h_calls - 1) == 1, "%u ms: 1 step in this call (got %d)", d, h_count_call(H_EV_RUNFRAME, h_calls - 1));
    CHECK(h_sv_out == 0, "%u ms: debt left, budget 0 (got %u)", d, h_sv_out);
    h_frame(0);
    CHECK(h_count_call(H_EV_RUNFRAME, h_calls - 1) == 1, "next 0 ms call: the owed step (got %d)", h_count_call(H_EV_RUNFRAME, h_calls - 1));
    CHECK(h_sv_out == h_deadline(3) - d, "then budget to the 3rd deadline %u (got %u)", h_deadline(3) - d, h_sv_out);
    CHECK(h_count(H_EV_RUNFRAME, 0) - rf == 2, "%u ms gave 2 steps over 2 calls (got %d)", d, h_count(H_EV_RUNFRAME, 0) - rf);
    INFO("at most one world step per SV_Frame call on the original algorithm; multi-step catch-up is not implemented");
}

/* rate-generic; the reset keeps its millisecond meaning (owed > 250 ms
 * after a step -> 100 ms owed); at 10 Hz 3 steps, 2 dropped, budget 100 */
static void t01_residual_reset(void)
{
    h_init(h_map());
    int rf = h_count(H_EV_RUNFRAME, 0);
    h_frame(250);   /* 10 Hz: residual 250 -> step -> 150 */
    h_frame(250);   /* 10 Hz: residual 400 -> step -> 300 > 250 -> reset to 100 */
    CHECK(h_sv_out == 0, "budget 0 (got %u)", h_sv_out);
    h_idle_while_owed();
    int steps = h_count(H_EV_RUNFRAME, 0) - rf, expect = 2 + h_rate / 10;
    CHECK(steps == expect, "500 ms (250+250) then idle calls: %d steps (2 + 100 ms), %d dropped (got %d)",
          expect, h_rate / 2 - expect, steps);
    CHECK(h_sv_out == h_deadline(1), "debt settled, budget %u (got %u)", h_deadline(1), h_sv_out);
}

static void t01_hitch(void)
{
    h_init(h_map());
    int rf = h_count(H_EV_RUNFRAME, 0);
    unsigned rt = svs.realtime;
    h_frame(251);
    CHECK(h_sv_in == 100, "hitch 251 ms enters SV_Frame as 100 (got %u)", h_sv_in);
    CHECK(svs.realtime - rt == 100, "server realtime advanced 100 (got %u)", svs.realtime - rt);
    h_idle_while_owed();
    h_frame(0);
    CHECK(svs.realtime - rt == 100, "idle calls add no time: realtime +100 (got %u)", svs.realtime - rt);
    CHECK(h_count(H_EV_RUNFRAME, 0) - rf == h_rate / 10, "hitch: exactly 100 ms of steps, %d (got %d)",
          h_rate / 10, h_count(H_EV_RUNFRAME, 0) - rf);
    CHECK(h_sv_out == h_deadline(1), "no debt after hitch, budget %u (got %u)", h_deadline(1), h_sv_out);
    rf = h_count(H_EV_RUNFRAME, 0);
    rt = svs.realtime;
    h_frame(250);
    h_idle_while_owed();
    int steps = h_count(H_EV_RUNFRAME, 0) - rf;
    CHECK(svs.realtime - rt == 250, "250 ms is not a hitch: realtime +250 (got %u)", svs.realtime - rt);
    CHECK(steps == 250 * h_rate / 1000, "250 ms: %d steps (got %d)", 250 * h_rate / 1000, steps);
    CHECK(h_sv_out == h_deadline(steps + 1) - 250, "then budget to the next deadline %u (got %u)",
          h_deadline(steps + 1) - 250, h_sv_out);
}

static void t01_uninitialized(void)
{
    /* uninitialized the loop runs the base 10 Hz: 30 ms, then 29 x 100 ms */
    h_init(NULL);
    unsigned cum = 0;
    for (int i = 0; i < 30; i++) {
        h_frame(i == 0 ? 30 : 100);
        cum += i == 0 ? 30 : 100;
    }
    CHECK(!svs.initialized, "no map: server not initialized");
    CHECK(h_count(H_EV_RUNFRAME, 0) == 0, "3 s uninitialized: no RunFrame (got %d)", h_count(H_EV_RUNFRAME, 0));
    CHECK(h_sv_out == 100 - cum % 100, "budget still follows the 10 Hz residual: %u (got %u)", 100 - cum % 100, h_sv_out);
    const unsigned d1 = h_deadline(1), half = d1 / 2;
    Cbuf_AddText(&cmd_buffer, va("map %s\n", h_map()));
    h_frame(half);
    int call = h_calls - 1;
    CHECK(svs.initialized && h_count_call(H_EV_SPAWN, call) == 1, "map executed in the next call");
    CHECK(h_count_call(H_EV_RUNFRAME, call) == 2 && sv.framenum == 2,
          "only the 2 spawn settle frames, no world step from earlier time (RF %d, framenum %d)",
          h_count_call(H_EV_RUNFRAME, call), sv.framenum);
    CHECK(h_sv_out == d1 - half, "residual restarts at the map: budget %u (got %u)", d1 - half, h_sv_out);
    h_frame(d1 - half - 1);
    CHECK(h_count_call(H_EV_RUNFRAME, h_calls - 1) == 0, "no world step before the first deadline %u ms", d1);
    h_frame(1);
    CHECK(h_count_call(H_EV_RUNFRAME, h_calls - 1) == 1, "first world step %u ms after map", d1);
}

#if USE_FPS
/* sv_fps is rounded down to a multiple of 10 within 10..60 before the world
 * starts, and the cvar reports the rate that runs (the mods refuse any other
 * value: mods/api3 t01_rate_refuse) */
static void t01_rate_round(void)
{
    static const int cases[][2] = { { 25, 20 }, { 5, 10 }, { 70, 60 }, { 40, 40 } };
    h_init(h_map());
    for (size_t i = 0; i < q_countof(cases); i++) {
        Cbuf_AddText(&cmd_buffer, va("killserver\nset sv_fps %d\nmap %s\n", cases[i][0], h_map()));
        h_frame(100);
        h_frame(100);
        CHECK(svs.initialized && SV_FRAMERATE == cases[i][1] && sv_fps->integer == cases[i][1],
              "sv_fps %d runs %d Hz (server %d Hz, cvar %d)", cases[i][0], cases[i][1], (int)SV_FRAMERATE,
              sv_fps->integer);
    }
}
#endif

/* map queued by the game from RunFrame */
static int h_hook_frame;
static void hook_map(game_import_t *gi, edict_t *e, int frame)
{
    (void)e;
    if (frame == h_hook_frame)
        gi->AddCommandString(va("gamemap %s\n", h_map()));  /* what the mods issue */
}

static void t01_map_from_runframe(void)
{
    h_init(h_map());
    for (int i = 0; i < 4; i++)
        h_frame(100);
    h_hook_frame = h_count(H_EV_RUNFRAME, 0) + 1;  /* game frame counter of the next step */
    h_rf_hook = hook_map;
    int spawn0 = h_count(H_EV_SPAWN, 0);
    h_frame(100);                       /* RF 5 queues the map */
    int queued = h_calls - 1;
    CHECK(h_count_call(H_EV_SPAWN, queued) == 0, "map is not run inside RunFrame");
    h_rf_hook = NULL;
    h_frame(100);
    int next = h_calls - 1;
    INFO("call after RunFrame queued map: %s", h_trace(next));
    int first = -1, spawn = -1, rf_after = 0;
    for (int i = 0; i < h_nlog; i++) {
        if (h_log[i].call != next)
            continue;
        if (first < 0)
            first = h_log[i].kind;
        if (h_log[i].kind == H_EV_SPAWN)
            spawn = i;
        else if (h_log[i].kind == H_EV_RUNFRAME && spawn >= 0)
            rf_after++;
    }
    CHECK(first == H_EV_SPAWN, "next call starts with the map, before any world step of the old level");
    CHECK(h_count(H_EV_SPAWN, 0) == spawn0 + 1, "map ran once");
    CHECK(rf_after == 3, "new level: 2 settle frames + 1 world step for this call's 100 ms (got %d)", rf_after);
}

static void hook_error(game_import_t *gi, edict_t *e, int frame)
{
    (void)e;
    if (frame == h_hook_frame)
        gi->error("h_scheduler_error");
}

static void t01_error(void)
{
    h_init(h_map());
    for (int i = 0; i < 3; i++)
        h_frame(100);
    h_hook_frame = h_count(H_EV_RUNFRAME, 0) + 1;
    h_rf_hook = hook_error;
    h_frame(100);
    /* fork: upstream Qcommon_Frame returns void, the error return is not checked */
    CHECK(h_count_call(H_EV_SHUTDOWN, h_calls - 1) == 1, "game library shut down once");
    CHECK(!svs.initialized, "server no longer initialized");
    int rf = h_count(H_EV_RUNFRAME, 0);
    for (int i = 0; i < 10; i++)
        h_frame(100);
    CHECK(h_count(H_EV_RUNFRAME, 0) == rf, "no RunFrame after the error (got %d)", h_count(H_EV_RUNFRAME, 0) - rf);
}

/* --------------------------------------------------------------- T04 */

static void t04_msec(void)
{
    h_init(h_map());
    CHECK(c_join(), "client joined through getchallenge/connect/new/begin");
    static const int msecs[] = { 7, 33, 100, 255, 1 };
    for (size_t i = 0; i < sizeof(msecs) / sizeof(msecs[0]); i++) {
        int from = h_nlog;
        c_move(msecs[i], 0);
        h_frame(100);
        int think = -1, got = -1;
        for (int j = from; j < h_nlog; j++)
            if (h_log[j].kind == H_EV_THINK && think < 0) { think = j; got = h_log[j].a; }
        CHECK(got == msecs[i], "usercmd.msec %d reaches ClientThink unchanged (got %d)", msecs[i], got);
    }
}

/* run 1.6 s allowance windows of wall time in 1 ms calls, `per_call` moves
 * of 100 msec every 100 ms; the first window starts right after a refill
 * (the step at framenum 16k * framediv) */
static void t04_windows(int per_call, int windows, int *accepted)
{
    const int refill = 16 * h_rate / 10;
    while (sv.framenum % refill != 1)
        h_frame(1);
    for (int w = 0; w < windows; w++) {
        int from = h_nlog;
        for (int ms = 0; ms < 1600; ms++) {
            if (ms % 100 == 0)
                for (int k = 0; k < per_call; k++)
                    c_move(100, 0);
            h_frame(1);
        }
        accepted[w] = h_count(H_EV_THINK, from);
    }
}

static void t04_allowance(void)
{
    int acc[3];
    h_init(h_map());
    CHECK(c_join(), "client joined");
    t04_windows(1, 2, acc);
    CHECK(acc[0] == 16 && acc[1] == 16, "1 x 100 ms per 100 ms (1.6 s per window): all 16 accepted (%d, %d)", acc[0], acc[1]);
    t04_windows(2, 3, acc);
    CHECK(acc[0] == 18 && acc[1] == 18 && acc[2] == 18,
          "2 x 100 ms per 100 ms: 1800 ms allowance per 1.6 s -> 18 accepted, 14 dropped (%d, %d, %d)", acc[0], acc[1], acc[2]);
    t04_windows(1, 1, acc);
    CHECK(acc[0] == 16, "back to 1 x 100 ms: input released again, 16 accepted (%d)", acc[0]);
    Cvar_Set("sv_enforcetime", "0");
    t04_windows(2, 1, acc);
    CHECK(acc[0] == 32, "control: sv_enforcetime 0 accepts all 32 (%d)", acc[0]);
}

/* --------------------------------------------------------------- T05 */

static int h_event_frame;
static char h_mark_seen[3][16];
static void hook_event_and_cmd(game_import_t *gi, edict_t *e, int frame)
{
    if (frame == h_event_frame) {
        e[1].s.event = EV_FOOTSTEP;
        Q_strlcpy(h_mark_seen[0], gi->cvar("h_mark", "", 0)->string, sizeof(h_mark_seen[0]));
        gi->AddCommandString("set h_mark 1\n");
        Q_strlcpy(h_mark_seen[1], gi->cvar("h_mark", "", 0)->string, sizeof(h_mark_seen[1]));
    } else if (frame == h_event_frame + 1) {
        Q_strlcpy(h_mark_seen[2], gi->cvar("h_mark", "", 0)->string, sizeof(h_mark_seen[2]));
    }
}

static void t05_order(void)
{
    h_init(h_map());
    CHECK(c_join(), "client joined");
    for (int i = 0; i < 3; i++) {
        c_move(100, 0);
        h_frame(100);
    }
    h_event_frame = h_count(H_EV_RUNFRAME, 0) + 1;  /* game frame counter of the next step */
    h_rf_hook = hook_event_and_cmd;
    c_move(100, 1);
    h_frame(100);
    int step = h_calls - 1;
    c_move(100, 0);
    h_frame(100);
    int next = h_calls - 1;
    h_rf_hook = NULL;
    char step_trace[512];
    Q_strlcpy(step_trace, h_trace(step), sizeof(step_trace));
    INFO("step call: %s | next call: %s", step_trace, h_trace(next));
    CHECK(!strcmp(h_trace(step), "THINK RF WIRE"), "one step: packet ClientThink -> RunFrame -> send with the event (%s)", h_trace(step));
    CHECK(h_count_call(H_EV_WIRE, next) == 0, "event cleared after the send: not resent next frame (%s)", h_trace(next));
    CHECK(!h_mark_seen[0][0] && !h_mark_seen[1][0], "command from RunFrame not executed inside RunFrame ('%s','%s')",
          h_mark_seen[0], h_mark_seen[1]);
    CHECK(!strcmp(h_mark_seen[2], "1"), "command from RunFrame executed before the next world step (h_mark='%s')", h_mark_seen[2]);
    INFO("the boundary of packet-queued commands is checked by the client harness (t05_cmd_boundary)");
}

/* any_rate: 1 written for any R, 2 for R > 10 only (the 10 Hz test game
 * does not advertise variable FPS); the rest is the 10 Hz baseline only
 * (their R Hz forms are the batching and command boundary scenarios of the
 * client harness) */
typedef struct { const char *name; void (*run)(void); int any_rate; } scenario_t;
static const scenario_t h_scenarios[] = {
    { "t01_contract", t01_contract, 1 },
    { "t01_early_call", t01_early_call, 1 },
    { "t01_residual", t01_residual, 1 },
    { "t01_partitions_minute", t01_partitions, 1 },
    { "t01_one_step_per_call", t01_one_step_per_call, 1 },
    { "t01_residual_reset", t01_residual_reset, 1 },
    { "t01_hitch", t01_hitch, 1 },
    { "t01_uninitialized", t01_uninitialized, 1 },
#if USE_FPS
    { "t01_rate_round", t01_rate_round, 2 },
#endif
    { "t01_map_from_runframe", t01_map_from_runframe },
    { "t01_error", t01_error },
    { "t04_msec", t04_msec, 1 },
    { "t04_allowance", t04_allowance, 1 },
    { "t05_order", t05_order },
    { NULL, NULL }
};

int main(int argc, char **argv)
{
    (void)q2pro_real_main;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <scenario>|list\n", argv[0]); return 2; }
    if (getenv("SCHED_RATE")) h_rate = atoi(getenv("SCHED_RATE"));
    if (h_rate < 10 || h_rate % 10) { fprintf(stderr, "SCHED_RATE must be a multiple of 10\n"); return 2; }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; h_scenarios[i].name; i++)
            if (h_rate == 10 ? h_scenarios[i].any_rate != 2 : h_scenarios[i].any_rate) printf("%s\n", h_scenarios[i].name);
        return 0;
    }
    for (int i = 0; h_scenarios[i].name; i++) {
        if (strcmp(h_scenarios[i].name, argv[1])) continue;
        if (h_rate != 10 && !h_scenarios[i].any_rate) { fprintf(stderr, "%s is 10 Hz only\n", argv[1]); return 2; }
        if (h_rate == 10 && h_scenarios[i].any_rate == 2) { fprintf(stderr, "%s needs SCHED_RATE > 10\n", argv[1]); return 2; }
        printf("scheduler %s @%d Hz\n", argv[1], h_rate);
        h_scenarios[i].run();
        if (getenv("SCHED_VERBOSE"))
            for (int j = 0; j < h_nlog; j++)
                printf("  log call=%d framenum=%d %s %d %d\n", h_log[j].call, h_log[j].framenum,
                       h_evname[h_log[j].kind], h_log[j].a, h_log[j].b);
        printf("scheduler %s: %s\n", argv[1], h_failures ? "FAIL" : "PASS");
        fflush(stdout);
        _exit(h_failures ? 1 : 0);  /* skip engine atexit/shutdown */
    }
    fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
}
