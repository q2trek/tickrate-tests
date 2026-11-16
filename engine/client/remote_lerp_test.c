// Host test of remote player history, snapshot reserve and presentation clocks.
#include <assert.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "remote_lerp.h"

// a failed check does not stop the run: every failing line is listed once, with its count, and main returns 1
#undef assert
static int fails, fail_count[2000];
static void expect(bool ok, const char *expr, int line)
{
    if (ok)
        return;
    fails++;
    if (line < 2000 && !fail_count[line]++)
        printf("  FAIL engine/client/remote_lerp_test.c:%d: %s\n", line, expr);
}
#define assert(x) expect((x), #x, __LINE__)

static rl_ring_t r;
static rl_set_t set;

static int frame_time(int n, int rate) { return n * 1000 / rate; }  // Com_FrameTime for n >= 0

static rl_snap_t snap(int t, float x, float yaw, int frame)
{
    rl_snap_t s = { .t = t, .frame = frame, .model = 1 };
    s.org[0] = x;
    s.ang[1] = yaw;
    return s;
}

static void push(int t, float x, float yaw, int frame) { rl_snap_t s = snap(t, x, yaw, frame); RL_Push(&r, &s, 512); }

static float at(float t, float D) { rl_out_t o; assert(RL_At(&r, t, D, &o)); return o.org[0]; }

// a mover on a server of rate R: position changes every `every` snapshots (a repeat when skip(n)), observer drawn at
// 144 Hz; a snapshot labelled L arrives when cl.time reaches L - I. Calls check() after each drawn frame.
typedef void (*check_t)(int cltime, float extra, const rl_out_t *o);
static float stream(int R, int every, int ms, bool (*skip)(int), check_t check)
{
    int I = RL_Interval(true, R, 1000 / R), n = 0, prev = 0;
    float x = 0, extra = 0;
    RL_Reset(&r);
    for (int k = 0; frame_time(k, 144) < ms; k++) {
        int cltime = frame_time(k, 144);
        for (; frame_time(n, R) - I <= cltime; n++) {
            if (n % every == 0 && !(skip && skip(n)))
                x += 10;
            push(frame_time(n, R), x, 0, 0);
        }
        extra = RL_Advance(&r, cltime - prev, I);
        prev = cltime;
        rl_out_t o;
        assert(RL_Sample(&r, cltime, (unsigned)(cltime), false, I, &o));
        if (check)
            check(cltime, extra, &o);
    }
    return extra;
}

// an even sender, x = 10 (n + 1) at label n: no adjustable delay, the drawn position is the input at t - I
static int even_R;
static void even_check(int cltime, float extra, const rl_out_t *o)
{
    int I = RL_Interval(true, even_R, 0), n = 0;
    float t = cltime - I;
    assert(extra == 0);
    assert(o->t == t); // includes startup: a lower clamp to oldest would change these first frames
    if (cltime < 100)
        return;
    while (frame_time(n + 1, even_R) <= t)
        n++;
    float x = 10 * (n + 1) + 10 * (t - frame_time(n, even_R)) / (frame_time(n + 1, even_R) - frame_time(n, even_R));
    assert(fabsf(o->org[0] - x) < 1e-3f);
}

static void test_interval(void)
{
    assert(RL_Interval(true, 60, 16) == 17);
    assert(RL_Interval(true, 30, 33) == 34);
    assert(RL_Interval(false, 60, 16) == 16);
    for (int R = 10; R <= 60; R += 10) {  // the largest step of the exact grid
        int big = 0;
        for (int n = 0; n < R; n++)
            if (frame_time(n + 1, R) - frame_time(n, R) > big)
                big = frame_time(n + 1, R) - frame_time(n, R);
        assert(RL_Interval(true, R, 0) == big);
    }
}

static void test_even_streams(void)
{
    even_R = 60, stream(60, 1, 3000, NULL, even_check); // 60 packets/s on S60: labels 16, 17, 17
    even_R = 30, stream(30, 1, 3000, NULL, even_check); // 30 on S30
    assert(stream(60, 2, 3000, NULL, NULL) == 17);      // 30 on S60, the rare sender: 34 - 17
}

static void test_extra_growth(void)
{
    RL_Reset(&r);
    push(0, 0, 0, 0); push(34, 10, 0, 0); push(68, 20, 0, 0);  // D = 34
    float last = 0;
    int ms = 0;
    for (int k = 0; k < 100 && RL_Advance(&r, 7, 17) < 17; k++) {  // 144 Hz: cl.time grows 7 ms
        assert(r.extra > last);           // float: 0.7 per frame, not truncated to 0
        last = r.extra;
        ms += 7;
        assert(ms <= 175);
    }
    assert(ms >= 161 && r.extra == 17);
    assert(RL_Advance(&r, 7, 17) == 17);
    r.D = 17;                             // an even sender again: extra decays to exactly 0
    for (int k = 0; k < 30 && r.extra > 0; k++)
        RL_Advance(&r, 6 + k % 2, 17);
    assert(r.extra == 0.0f);
}

static void test_counterexample(void)
{
    // changes 0@0, 10@2, 20@4, 30@10 with a record every 1 ms, D = 2
    RL_Reset(&r);
    for (int t = 0; t <= 10; t++)
        push(t, t < 2 ? 0 : t < 4 ? 10 : t < 10 ? 20 : 30, 0, 0);
    assert(at(7, 2) == 20 && at(8, 2) == 20 && at(9, 2) == 25);
    // t <= a stands on k0, alpha in [0, 1], no extrapolation after the last point
    float prev = 0;
    for (float t = 0; t <= 14; t += 0.25f) {
        float x = at(t, 2);
        assert(x >= prev && x >= 0 && x <= 30);
        prev = x;
    }
    assert(at(14, 2) == 30 && at(10, 2) == 30);
}

static int stop_from, stop_to;
static bool stopped(int n) { return frame_time(n, 60) >= stop_from && frame_time(n, 60) < stop_to; }
static float stop_x = -1, extra_before = -1, extra_after;
// D1: the window of the ring r at I, S + I with the shift S = min(extra + I, I + RL_MAX_EXTRA)
static float window(int I)
{
    return fminf(r.extra + I, I + RL_MAX_EXTRA) + I;
}
static void stop_check(int cltime, float extra, const rl_out_t *o)
{
    // the stop is drawn standing on the last position before it until k1.t - W after it
    if (o->t > stop_from + 100 && o->t <= stop_to - window(17)) {
        if (stop_x < 0)
            stop_x = o->org[0], extra_before = extra;
        assert(o->org[0] == stop_x);
    }
    if (cltime > stop_to + 500)
        extra_after = extra;
}

static void test_stop(void)
{
    stop_from = 1000, stop_to = 3000;
    stream(60, 2, 4000, stopped, stop_check);  // 30 packets/s on S60, a 2 s stop
    assert(r.has_moved && extra_before == 17 && extra_after <= extra_before);
}

static unsigned seed = 1;
static bool tenth(int n) { (void)n; seed = seed * 1103515245 + 12345; return (seed >> 16) % 10 == 0; }
static int last_D, d_changes, d_last_change = -1000, d_min_spacing = 1 << 30;
static void hyst_check(int cltime, float extra, const rl_out_t *o)
{
    (void)extra; (void)o;
    if (r.D != last_D) {
        if (last_D && cltime - d_last_change < d_min_spacing)
            d_min_spacing = cltime - d_last_change;
        last_D = r.D, d_last_change = cltime, d_changes++;
    }
}

static void test_hysteresis(void)
{
    stream(60, 1, 6000, tenth, hyst_check);  // 60 packets/s, ~10 % repeats
    assert(d_changes >= 1 && d_min_spacing >= RL_HOLD - 17);
}

static void test_monotonic_t(void)
{
    RL_Reset(&r);
    for (int n = 0; n < 20; n++)
        push(n * 34, n * 10.0f, 0, 0);
    for (int k = 0; k < 40; k++)
        RL_Advance(&r, 7, 17);
    rl_out_t o;
    assert(RL_Sample(&r, 700, (unsigned)(700), false, 17, &o));
    float t0 = o.t, e0 = r.extra, target0 = r.shown;
    for (int n = 20; n < 40; n++)        // D grows while cl.time stands; records are evicted, so only sample time is comparable
        push(646 + (n - 19) * 90, n * 10.0f, 0, 0);
    assert(RL_Advance(&r, 0, 17) == e0); // no growth of cl.time, no change of extra
    assert(RL_Sample(&r, 700, (unsigned)(700), false, 17, &o) && o.t >= t0);
    // Unlike the mechanical calls, real time advances while cl.time steps back: this probes the target floor, not a gap.
    assert(RL_Sample(&r, 699, 701u, false, 17, &o) && o.t >= t0 && r.shown == target0);  // sample time never goes backwards (not a position comparison)
}

static void test_angles(void)
{
    // standing and turning: the angles move, the position stands; 350 -> 10 turns through 0
    RL_Reset(&r);
    for (int n = 0; n < 10; n++)
        push(frame_time(n, 60), 0, fmodf(350 + n * 5.0f, 360), 0);
    rl_out_t o, p;
    assert(RL_At(&r, 40, 17, &o) && RL_At(&r, 41, 17, &p));
    assert(o.org[0] == 0 && p.ang[1] != o.ang[1]);
    float y = fmodf(o.ang[1] + 360, 360);
    assert(y > 350 || y < 20);
    // a rare sender turns every second snapshot: angles on t have no steps through a snapshot
    RL_Reset(&r);
    for (int n = 0; n < 30; n++)
        push(frame_time(n, 60), 0, (n / 2) * 6.0f, 0);
    float prev = -1;
    for (float t = 250; t < 460; t += 7) {  // inside the RL_WINDOW of the newest record (483)
        assert(RL_At(&r, t, 34, &o));
        assert(o.ang[1] > prev);
        prev = o.ang[1];
    }
}

static void test_animation(void)
{
    // frames A, B, B, B of a standing player: A -> B runs RL_ANIM on t and repeats do not cut it
    RL_Reset(&r);
    push(0, 0, 0, 1);
    for (int n = 1; n < 10; n++)       // up to 150: the frame-1 record stays inside RL_WINDOW
        push(frame_time(n, 60), 0, 0, 2);
    rl_out_t o;
    float prev = 2;
    int switched = 0;
    for (int k = 0; frame_time(k, 144) < 400; k++) {
        assert(RL_Sample(&r, frame_time(k, 144), (unsigned)(frame_time(k, 144)), false, 17, &o));
        if (o.frame == 2 && o.oldframe == 1) {
            assert(o.backlerp <= prev && o.backlerp >= 0);
            prev = o.backlerp, switched++;
        }
    }
    assert(o.frame == 2 && o.oldframe == 2 && o.backlerp == 0);
    assert(switched >= 10 && switched <= 15);  // t 16..100: from the label before the run of B, as cl.prevservertime
}

static void test_resets(void)
{
    rl_snap_t s;
#define FRESH(...) (RL_Reset(&r), push(0, 0, 0, 0), push(17, 10, 0, 0), s = snap(34, 20, 0, 0), __VA_ARGS__, RL_Push(&r, &s, jump), r.count == 1)
    float jump = 512;
    assert(!FRESH((void)0));
    assert(FRESH(s.model = 2));
    assert(FRESH(s.teleport = true));
    assert(FRESH(s.org[0] = 10 + 513));
    assert(!FRESH(s.org[0] = 10 + 511));
    assert(FRESH(s.stat = 7));
    assert(!FRESH(s.t = 34 + 17));      // one snapshot skipped
    assert(FRESH(s.t = 17 + 101));      // absent > 100 ms
    assert(FRESH(s.t = 17));            // label not after the last one: the server clock re-anchored
    jump = 256;                         // chase camera
    assert(FRESH(s.org[0] = 10 + 257));
    // a reset starts a new history: t of the old one does not hold the drawing back
    rl_out_t o;
    RL_Reset(&r);
    push(0, 0, 0, 0); push(34, 10, 0, 0);
    assert(RL_Sample(&r, 500, (unsigned)(500), false, 17, &o));
    RL_Reset(&r);
    assert(!r.has_shown && r.extra == 0 && r.ngaps == 0 && !RL_Sample(&r, 400, (unsigned)(400), false, 17, &o));
#undef FRESH
}

static void test_conditions(void)
{
    const rl_cond_t on = { .enabled = true, .div = 6 };
    rl_cond_t c;
    assert(RL_EntityOn(&on, 3, 8, 0, 17));
    assert(!RL_EntityOn(&on, 9, 8, 0, 17));       // not a player
    assert(!RL_EntityOn(&on, 0, 8, 0, 17));
    assert(!RL_EntityOn(&on, 1, 8, 0, 17));       // the POV
    assert(RL_EntityOn(&on, 3, 8, 0, 0));         // extra 0: the path too (the reserve does not depend on D)
    c = on, c.demo = true;    assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    c = on, c.div = 1;        assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    c = on, c.nolerp = 1;     assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    c = on, c.enabled = false; assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    // camera: chase = frame.clientNum != own clientNum, without prediction
    assert(RL_CameraOn(&on, false, 4, 0, 17));
    assert(!RL_CameraOn(&on, true, 4, 0, 17));    // predicted view
    assert(!RL_CameraOn(&on, false, 0, 0, 17));   // cl_predict 0 without chase, OpenTDM third-person
    assert(RL_CameraOn(&on, false, 4, 0, 0));
    c = on, c.demo = true;    assert(!RL_CameraOn(&c, false, 4, 0, 17));
    c = on, c.div = 1;        assert(!RL_CameraOn(&c, false, 4, 0, 17));
    c = on, c.nolerp = 2;     assert(!RL_CameraOn(&c, false, 4, 0, 17));
    c = on, c.enabled = false; assert(!RL_CameraOn(&c, false, 4, 0, 17));
}

static void fill(rl_ring_t *ring)
{
    RL_Reset(ring);
    for (int n = 0; n < 5; n++) {
        rl_snap_t s = snap(frame_time(n, 60), n * 10.0f, 0, 0);
        RL_Push(ring, &s, 512);
    }
}

static void test_pov(void)
{
    RL_ResetAll(&set);
    assert(set.pov == -1);
    RL_SetPov(&set, 0);
    fill(&set.player[2]); fill(&set.player[5]); fill(&set.chase);
    RL_SetPov(&set, 0);                                       // same POV: nothing restarts
    assert(set.player[2].count == 5 && set.chase.count == 5);
    RL_SetPov(&set, 2);                                       // chase player 2: its ring and the camera restart
    assert(set.player[2].count == 0 && set.chase.count == 0 && set.player[5].count == 5);
    fill(&set.chase);
    RL_SetPov(&set, 5);                                       // the chase target changes
    assert(set.player[2].count == 0 && set.player[5].count == 0 && set.chase.count == 0);
}

// jitter profile (spec "Jitter profile", 3.5): the mover sends 60 moves/s with send moments jittered by 30 % of the
// interval, S60, observer at 144 Hz; a server frame shows every move that arrived by it (none: a repeat, two: a double
// step). Freeze of the drawn position, old lerp (last two snapshots) vs this path, which the client takes for every
// remote player. This is a deterministic model of command timing.
static void test_jitter(void)
{
    const int I = RL_Interval(true, 60, 16);
    unsigned seed = 1;
    float send[700], x = 0, prev_x = 0, cur_x = 0, old_last = -1, rl_last = -1;
    int n = 0, sent = 0, cur_t = 0, prev = 0, frames = 0, old_still = 0, rl_still = 0;
    for (int k = 0; k < 700; k++) {
        seed = seed * 1103515245 + 12345;
        send[k] = k * 1000.0f / 60 + ((seed >> 16) % 1001 / 1000.0f - 0.5f) * 0.6f * 1000 / 60;
    }
    RL_Reset(&r);
    for (int k = 0; frame_time(k, 144) < 10000; k++) {
        int cltime = frame_time(k, 144);
        for (; frame_time(n, 60) - I <= cltime; n++) {
            for (; sent < 700 && send[sent] <= frame_time(n, 60); sent++)
                x += 10;
            prev_x = cur_x, cur_x = x, cur_t = frame_time(n, 60);
            push(cur_t, x, 0, 0);
        }
        float extra = RL_Advance(&r, cltime - prev, I), f = 1 - (cur_t - cltime) / (float)I;
        prev = cltime;
        float old = prev_x + (f < 0 ? 0 : f > 1 ? 1 : f) * (cur_x - prev_x);
        rl_out_t o;
        assert(RL_Sample(&r, cltime, (unsigned)(cltime), false, I, &o) && extra <= RL_MAX_EXTRA);
        float drawn = o.org[0];
        if (cltime >= 1000) {   // past the first second: D settled
            frames++;
            old_still += old == old_last;
            rl_still += drawn == rl_last;
        }
        old_last = old, rl_last = drawn;
    }
    assert(old_still > frames / 50);      // the profile does repeat positions: old lerp freezes
    assert(rl_still <= old_still);
}

// causal schedule (spec "Single repeated position", "Real stop and resume"): snapshot n of a server at rate R has the
// label frame_time(n, R) and arrives on time, at the real time label - I - n % 3; the observer draws at 60 Hz, cl.time
// grows with the real time and is clamped from above by the newest label received, as CL_SetClientTime. Per frame as
// in entities.c: push the arrived snapshots, advance, then the consumer's gate and sample; drawn = old lerp when off.
typedef void (*causal_check_t)(int cltime, int I, float extra, bool on, const rl_out_t *o, float drawn);
static int cs_newest_t;
static float cs_newest_x;
static float (*cs_yaw)(int n);   // NULL: yaw = position / 10
static int causal(int R, int ms, float (*pos)(int n), bool camera, causal_check_t check)
{
    const rl_cond_t c = { .enabled = true, .div = R / 10 };
    const int I = RL_Interval(true, R, 1000 / R);
    rl_ring_t *ring = camera ? &set.chase : &r;
    int n = 0, real = 0, cltime = 0, prev_t = 0, clamps = 0;
    float prev_x = 0;
    cs_newest_t = 0, cs_newest_x = 0;
    RL_Reset(ring);
    for (int k = 0; (real = frame_time(k, 60)) < ms; k++) {
        for (; frame_time(n, R) - I - n % 3 <= real; n++) {
            rl_snap_t s = snap(frame_time(n, R), pos(n), cs_yaw ? cs_yaw(n) : pos(n) / 10, 0);
            RL_Push(ring, &s, camera ? 256 : 512);
            prev_x = cs_newest_x, prev_t = cs_newest_t, cs_newest_x = s.org[0], cs_newest_t = s.t;
        }
        int next = real;    // cl.time follows the real time
        if (next > cs_newest_t)
            next = cs_newest_t, clamps++;
        float extra = RL_Advance(ring, next - cltime, I), f = (float)(next - prev_t) / (cs_newest_t - prev_t);
        cltime = next;
        float old = prev_x + (f < 0 ? 0 : f > 1 ? 1 : f) * (cs_newest_x - prev_x);
        rl_out_t o;
        bool on = (camera ? RL_CameraOn(&c, false, 4, 0, extra) : RL_EntityOn(&c, 3, 8, 0, extra))
            && RL_Sample(ring, cltime, (unsigned)(cltime), false, I, &o);
        check(cltime, I, extra, on, &o, on ? o.org[0] : old);
    }
    return clamps;
}

// an even sender moves 10 per snapshot; snapshot rp_m repeats the position before it, the next one is a double step
static int rp_m;
static float rp_last;
static float repeat_pos(int n) { return 10.0f * (n == rp_m ? n - 1 : n); }
static void repeat_check(int cltime, int I, float extra, bool on, const rl_out_t *o, float drawn)
{
    if (cltime >= 200) {            // past the warm-up
        assert(extra == 0);         // D = I: the reserve, not the adjustable delay, carries the repeat
        assert(on && o->t == cltime - I);
        assert(drawn > rp_last);    // no drawn frame without movement, across the repeat too
        assert(drawn <= cs_newest_x);
    }
    rp_last = drawn;
}

static void test_single_repeat(void)
{
    for (int R = 20; R <= 60; R += 10) {
        rp_m = R, rp_last = -1;     // the repeat at the label 1000
        assert(causal(R, 2000, repeat_pos, false, repeat_check) == 0);   // on time: no high clamp
    }
}

// the chase camera ring: the same shift and the same continuous repeat through RL_CameraOn
static void test_chase_reserve(void)
{
    for (int R = 20; R <= 60; R += 10) {
        rp_m = R, rp_last = -1;
        assert(causal(R, 2000, repeat_pos, true, repeat_check) == 0);
    }
}

// D1a: an even run to 590@983, a 2 s stop with standing snapshots, then 600@3000, 610@3016, ...; yaw = x / 10
static float resume_pos(int n) { return n <= 59 ? 10.0f * n : n < 180 ? 590 : 600 + 10.0f * (n - 180); }
static int resume_frames, resume_595;
static void resume_check(int cltime, int I, float extra, bool on, const rl_out_t *o, float drawn)
{
    if (!on || o->t < 1100 || o->t >= 3000) {
        assert(on || cltime < 1100);
        return;
    }
    // until the label of the first new point: the transition from the standing anchor over the window 2 I
    assert(extra == 0);
    float a = 3000 - 2 * I, f = o->t <= a ? 0 : (o->t - a) / (2 * I);
    assert(fabsf(drawn - (590 + 10 * f)) < 1e-3f);
    assert(fabsf(o->ang[1] - (59 + f)) < 1e-3f);
    resume_frames++;
    if (cltime == 3000) {           // 600@3000 and 610@3016 both received: 595, not 600
        assert(cs_newest_t >= 3016 && o->t == 2983 && fabsf(drawn - 595) < 1e-3f && fabsf(o->ang[1] - 59.5f) < 1e-3f);
        resume_595++;
    }
}

static void test_resume_anchor(void)
{
    resume_frames = resume_595 = 0;
    assert(causal(60, 3100, resume_pos, false, resume_check) == 0);
    assert(r.extra == 0 && resume_595 == 1 && resume_frames > 100);
}

// D1a for angles alone: the position never changes, yaw turns 340 -> 350 at 333 and stands until 3000, then 10@3000, 12@3016, ...;
// the turned anchor (yaw 350) is the k0 of the first new yaw: 350 -> 370 (= 10, through 0) over 2 I, no jump to 10
static float still_pos(int n) { (void)n; return 0; }
static float turn_yaw(int n) { return n < 20 ? 340 : n < 180 ? 350 : 10 + 2.0f * (n - 180); }  // 350@333: a turn alone, past RL_WINDOW (no gap)
static int turn_frames, turn_360;
static void turn_check(int cltime, int I, float extra, bool on, const rl_out_t *o, float drawn)
{
    if (!on || o->t < 1100 || o->t >= 3000)
        return;
    float a = 3000 - 2 * I, f = o->t <= a ? 0 : (o->t - a) / (2 * I);
    assert(extra == 0 && drawn == 0);
    assert(fabsf(o->ang[1] - (350 + 20 * f)) < 1e-3f);
    turn_frames++;
    if (cltime == 3000) {
        assert(o->t == 2983 && fabsf(o->ang[1] - 360) < 1e-3f);
        turn_360++;
    }
}

static void test_turn_anchor(void)
{
    turn_frames = turn_360 = 0, cs_yaw = turn_yaw;
    assert(causal(60, 3100, still_pos, false, turn_check) == 0);
    cs_yaw = NULL;
    assert(r.has_anchor_turned && turn_360 == 1 && turn_frames > 100);
}

// D1: the window is I + the full delay S on every step, while extra has not caught up with a falling or a rising D,
// so on time the transition starts at the arrival: the first frame after it shows no part of the way. A stand at 0,
// then 100 at WL = 200: the first change point stays in the ring, 100@WL arrives at cl.time = WL - I, and the
// transition [max(0, WL - W), WL] gives W back from a drawn point inside it: W = (WL - t) / (1 - f)
#define WL 200
static int window_probes;
static void window_probe(int I)
{
    rl_out_t o;
    float S = fminf(r.extra + I, I + RL_MAX_EXTRA);
    r.has_shown = r.has_P = false;
    r.last_real = 0;
    assert(RL_Sample(&r, WL - I, (unsigned)(WL - I), false, I, &o));
    assert(fabsf(WL - I - o.t - S) < 1e-3f && o.org[0] < 1e-3f);    // the arrival frame: f = 0
    assert(RL_Sample(&r, WL + 10, (unsigned)(WL + 10), false, I, &o));
    assert(fabsf((WL - o.t) / (1 - o.org[0] / 100) - (I + S)) < 1e-2f);
    window_probes++;
}

static void test_window(void)
{
    RL_Reset(&r);
    for (int n = 0; frame_time(n, 60) <= WL; n++)
        push(frame_time(n, 60), frame_time(n, 60) < WL ? 0 : 100, 0, 0);
    window_probes = 0;
    r.D = 34;                               // D falls to I while extra is 17 (loops bounded: assert does not stop)
    for (int k = 0; k < 1000 && RL_Advance(&r, 1, 17) < 17; k++)
        ;
    r.D = 17;
    int k = 0;
    do
        window_probe(17);
    while (++k < 1000 && RL_Advance(&r, 1, 17) > 0);
    r.D = 117;                              // D rises to (100, I + 100] while extra is 0
    k = 0;
    do
        window_probe(17);
    while (++k < 2000 && RL_Advance(&r, 1, 17) < 100);
    window_probe(17);
    assert(window_probes > 1100);
    static const int Is[] = { 17, 34, 50 };  // ceilings: S = I + RL_MAX_EXTRA, W = 2 I + RL_MAX_EXTRA
    for (int i = 0; i < 3; i++) {
        int I = Is[i];
        rl_out_t o;
        r.D = 1000, r.extra = 0;
        for (int k = 0; k < 1000 && RL_Advance(&r, 10, I) < RL_MAX_EXTRA; k++)
            ;
        window_probe(I);
        r.has_shown = r.has_P = false;
        r.last_real = 0;
        assert(RL_Sample(&r, WL + 10, (unsigned)(WL + 10), false, I, &o));
        float S = WL + 10 - o.t, W = (WL - o.t) / (1 - o.org[0] / 100);
        assert(fabsf(S - (I + RL_MAX_EXTRA)) < 1e-3f && fabsf(W - (2 * I + RL_MAX_EXTRA)) < 1e-2f);
    }

    // a sparse sender on S60: a change every 7 snapshots (116/117 ms, D in (100, I + 100]), once 8 (133 ms); the
    // observer draws at each on-time arrival (cl.time = label - I) from its first second, D rising, on: the frame
    // with a new change point received equals the frame without it (no partial step of the new point)
    const int I = 17;
    int cltime = -I, checked = 0;
    float x = 0;
    RL_Reset(&r);
    for (int n = 0; n < 180; n++) {
        bool change = n == 148 || (n % 7 == 0 && n != 147);
        int next = frame_time(n, 60) - I;
        RL_Advance(&r, next - cltime, I);
        cltime = next;
        // This window probe assumes clocks at target; carrying P while extra grows would test recovery instead.
        r.has_P = false, r.last_real = 0;
        rl_ring_t without = r;
        x += change ? 10 : 0;
        push(frame_time(n, 60), x, 0, 0);
        rl_out_t o, p;
        assert(RL_Sample(&r, cltime, (unsigned)(cltime), false, I, &o));
        if (n == 0 || !RL_Sample(&without, cltime, (unsigned)(cltime), false, I, &p))
            continue;
        if (n == 146)
            assert(r.D > 100 && r.D <= I + RL_MAX_EXTRA);
        if (change)
            assert(fabsf(o.org[0] - p.org[0]) < 1e-3f), checked++;
    }
    assert(checked > 20);
}

// D1a: a sparse sender with D at the ceiling (S + I plus the change interval is longer than the ring horizon, so the
// change point before t leaves the ring): position and yaw never decrease and never pass the newest received point,
// and equal the transition between the nearest change points of the input (k0 <= t < k1 received, window S + I), so a
// lost k0 shows neither as a step back nor as a jump to k1.
// Cells of critic-a/repro-sparse.log, aligned (one interval) and non-aligned (a cycle of intervals), on the causal
// schedule and with cl.time at the low end of its clamp (cl.time = newest - I, drawn at each arrival).
static const int *sp_cycle;
static int sp_len, sp_sum;
static float sparse_pos(int n)   // 10 per change; changes at the cumulative offsets of the cycle
{
    int c = 0, m = n % sp_sum;
    for (int i = 0, o = 0; i < sp_len && o <= m; o += sp_cycle[i++])
        c++;
    return 10.0f * (sp_len * (n / sp_sum) + c);
}
// the input path at t: k0 the last change point <= t, k1 the first one after it received by newest
static float sparse_expect(int R, float t, int newest, float W)
{
    int n = 0, m;
    while (frame_time(n + 1, R) <= t)
        n++;
    while (n > 0 && sparse_pos(n) == sparse_pos(n - 1))
        n--;
    for (m = n + 1; frame_time(m, R) <= newest && sparse_pos(m) == sparse_pos(n); m++)
        ;
    if (frame_time(m, R) > newest)
        return sparse_pos(n);
    float k0 = frame_time(n, R), k1 = frame_time(m, R), a = fmaxf(k0, k1 - W);
    return sparse_pos(n) + (t <= a ? 0 : (t - a) / (k1 - a)) * (sparse_pos(m) - sparse_pos(n));
}
static float sp_last, sp_ylast;
static int sp_frames, sp_R;
static void sparse_step(int I, float extra, bool on, const rl_out_t *o, float drawn, float newest_x, int newest_t)
{
    assert(on);
    if (!on)
        return;
    if (sp_frames++) {
        assert(drawn >= sp_last - 1e-3f);           // never backwards
        assert(o->ang[1] >= sp_ylast - 1e-3f);
    }
    assert(drawn <= newest_x + 1e-3f && o->ang[1] <= newest_x / 10 + 1e-3f);   // never past the newest point
    if (o->t >= 300) {                              // past the first records
        float x = sparse_expect(sp_R, o->t, newest_t, fminf(extra + I, I + RL_MAX_EXTRA) + I);
        assert(fabsf(drawn - x) < 1e-2f && fabsf(o->ang[1] - x / 10) < 1e-3f);
    }
    sp_last = drawn, sp_ylast = o->ang[1];
}
static void sparse_check(int cltime, int I, float extra, bool on, const rl_out_t *o, float drawn)
{
    (void)cltime;
    sparse_step(I, extra, on, o, drawn, cs_newest_x, cs_newest_t);
}
static void sparse_cell(int R, const int *cycle, int len)
{
    const rl_cond_t c = { .enabled = true, .div = R / 10 };
    const int I = RL_Interval(true, R, 1000 / R);
    sp_cycle = cycle, sp_len = len, sp_sum = 0, sp_R = R;
    for (int i = 0; i < len; i++)
        sp_sum += cycle[i];

    sp_frames = 0;
    assert(causal(R, 6000, sparse_pos, false, sparse_check) == 0);
    assert(sp_frames > 300 && r.extra == RL_MAX_EXTRA);     // D at the ceiling

    // cl.time at the low end of its clamp: drawn at each arrival, cl.time = newest - I
    int cltime = -I;
    RL_Reset(&r);
    sp_frames = 0;
    for (int n = 0; frame_time(n, R) < 6000; n++) {
        rl_snap_t s = snap(frame_time(n, R), sparse_pos(n), sparse_pos(n) / 10, 0);
        RL_Push(&r, &s, 512);
        int next = s.t - I;
        float extra = RL_Advance(&r, next - cltime, I);
        cltime = next;
        rl_out_t o;
        bool on = RL_EntityOn(&c, 3, 8, 0, extra) && RL_Sample(&r, cltime, (unsigned)(cltime), false, I, &o);
        sparse_step(I, extra, on, &o, on ? o.org[0] : 0, s.org[0], s.t);
    }
    assert(sp_frames > 100 && r.extra == RL_MAX_EXTRA);
}

static void test_sparse_monotonic(void)
{
    static const int e3[] = { 3 }, e4[] = { 4 }, e5[] = { 5 }, e6[] = { 6 }, e7[] = { 7 }, e8[] = { 8 };
    static const int u20[] = { 3, 4 }, u30[] = { 4, 5, 3 }, u60[] = { 7, 9, 8, 10 };
    sparse_cell(20, e3, 1); sparse_cell(20, e4, 1);
    sparse_cell(30, e4, 1); sparse_cell(30, e5, 1);
    sparse_cell(40, e6, 1);
    sparse_cell(50, e7, 1);
    sparse_cell(60, e8, 1);
    sparse_cell(20, u20, 2); sparse_cell(30, u30, 3); sparse_cell(60, u60, 4);   // non-aligned intervals
}

// The client accumulates elapsed time; each received pair supplies BOTH CL_SetClientTime bounds.
// An absolute real-time clock, or a model with only the high clamp, loses the carry after a late frame.
typedef struct {
    int time, previous, newest, high, low;
} client_clock_t;

static int client_step(client_clock_t *c, int elapsed)
{
    int next = c->time + elapsed;
    if (next > c->newest)
        next = c->newest, c->high++;
    else if (next < c->previous)
        next = c->previous, c->low++;
    return c->time = next;
}

static void test_client_clock_model(void)
{
    client_clock_t c = { .time = 10, .newest = 20 };
    assert(client_step(&c, 16) == 20 && c.high == 1);
    assert(client_step(&c, 16) == 20 && c.high == 2);
    c.previous = 60, c.newest = 80;
    assert(client_step(&c, 16) == 60 && c.low == 1);
    assert(client_step(&c, 16) == 76); // accumulate from the clamp, not absolute elapsed real time
}

static bool same_output(const rl_out_t *a, const rl_out_t *b)
{
    return a->t == b->t && !memcmp(a->org, b->org, sizeof(a->org))
        && !memcmp(a->ang, b->ang, sizeof(a->ang)) && a->frame == b->frame
        && a->oldframe == b->oldframe && a->backlerp == b->backlerp;
}

static void test_clock_empty_repeat(void)
{
    RL_Reset(&r);
    r.P = 42, r.has_P = true, r.last_real = 123;
    rl_ring_t before = r;
    rl_out_t a, b;
    assert(!RL_Sample(&r, 1000, 10000, false, 17, &a));
    assert(!memcmp(&r, &before, sizeof(r))); // empty histories do not consume real time
    RL_Reset(&r);
    for (int t = 800; t <= 1100; t += 17)
        push(t, t / 10.0f, t / 20.0f, t / 50);
    assert(RL_Sample(&r, 1000, 10000, false, 17, &a));
    before = r;
    assert(RL_Sample(&r, 1000, 10000, false, 17, &b));
    assert(same_output(&a, &b) && !memcmp(&r, &before, sizeof(r)));
}

// P=t would hold on these frames; a real-time step keeps moving inside the accepted history.
static void test_late_frame(void)
{
    for (int R = 20; R <= 60; R += 10) {
        int I = RL_Interval(true, R, 0), n = 0, previous_real = 0, checked = 0;
        int delayed = R;
        client_clock_t c = {0};
        rl_out_t o = {0};
        RL_Reset(&r);
        for (int k = 0; frame_time(k, 60) < 1600; k++) {
            int real = frame_time(k, 60), dt = real - previous_real;
            // One late frame; the next on-time frame queues behind it, preserving arrival order.
            while (frame_time(n, R) + (n == delayed ? I : 0) <= real) {
                int label = frame_time(n++, R);
                push(label, label / 10.0f, label / 20.0f, n % 4);
                c.previous = c.newest, c.newest = label;
            }
            int old_ct = c.time, high = c.high;
            float old_P = r.P, old_x = o.org[0];
            bool initialized = r.has_P;
            client_step(&c, dt);
            RL_Advance(&r, c.time - old_ct, I);
            assert(RL_Sample(&r, c.time, (unsigned)real, false, I, &o));
            assert(o.t <= c.newest && (!initialized || o.t >= old_P));
            if (real >= 1000 && real < 1000 + I && initialized
                && c.high > high && c.time == old_ct && c.newest > old_P) {
                checked++;
                assert(o.org[0] > old_x);
                assert(o.t > old_P);
                assert(o.t - old_P <= 1.1f * dt + 1e-3f);
                assert(o.t == c.newest || o.t - old_P >= 0.5f * dt - 1e-3f);
            }
            previous_real = real;
        }
        assert(checked > 0); // fail if the delayed receipt never produces a held cl.time with headroom
        printf("late-frame R=%d: held frames with headroom=%d\n", R, checked);
    }
}

// Error = target - P: positive is behind. Explicit seeds make gain=[1,1] fail; the newest record stays beyond P throughout.
static void recovery_case(int R, int fps, float error, int deadline)
{
    int I = RL_Interval(true, R, 0), next_label = 700, previous = 0, reached = -1;
    RL_Reset(&r);
    while (next_label <= 1150) {
        push(next_label, next_label / 10.0f, 0, 0);
        next_label += I;
    }
    r.P = 1000 - I - error, r.has_P = true, r.last_real = 10000;
    r.shown = 1000 - I, r.has_shown = true;
    assert(fabsf((1000 - I) - r.P - error) < 1e-3f && fabsf(error) > 1);
    for (int k = 1; frame_time(k, fps) <= deadline; k++) {
        int elapsed = frame_time(k, fps), dt = elapsed - previous, ct = 1000 + elapsed;
        while (next_label <= ct + 150) {
            push(next_label, next_label / 10.0f, 0, 0);
            next_label += I;
        }
        float old_P = r.P, old_error = (1000 + previous - I) - old_P;
        rl_out_t o;
        assert(RL_Sample(&r, ct, 10000u + elapsed, false, I, &o));
        float now_error = (ct - I) - o.t, step = o.t - old_P;
        assert(o.t < r.rec[r.head].s.t); // exclude the newest-label cap from the recovery oracle
        assert(step >= 0.5f * dt - 1e-3f && step <= 1.1f * dt + 1e-3f);
        assert(o.t >= old_P && fabsf(now_error) <= fabsf(old_error) + 1e-3f);
        if (old_error > 1e-3f) {
            assert(step > dt && old_error - now_error <= 0.1f * dt + 1e-3f);
        } else if (old_error < -1e-3f) {
            assert(step < dt && now_error - old_error <= 0.5f * dt + 1e-3f);
        }
        if (reached < 0 && fabsf(now_error) <= 1)
            reached = elapsed;
        previous = elapsed;
    }
    assert(reached >= 0 && reached <= deadline);
}

static void test_clock_recovery(void)
{
    static const float errors[] = {-10, -5, 5, 10, 20};
    for (int R = 20; R <= 60; R += 10) {
        for (int fps = 60; fps <= 120; fps += 60) {
            for (unsigned j = 0; j < sizeof(errors) / sizeof(*errors); j++)
                recovery_case(R, fps, errors[j], errors[j] == 20 ? 400 : 300);
            recovery_case(R, fps, -RL_Interval(true, R, 0), 400);
        }
    }
    static const int gaps[] = {100, 200, 250};
    static const float long_errors[] = {-20, -5, 5, 20};
    for (unsigned i = 0; i < sizeof(gaps) / sizeof(*gaps); i++) {
        for (unsigned j = 0; j < sizeof(long_errors) / sizeof(*long_errors); j++) {
            int dt = gaps[i];
            float error = long_errors[j];
            RL_Reset(&r);
            for (int t = 1000; t <= 1500; t += 17)
                push(t, t / 10.0f, 0, 0);
            r.P = 983 - error, r.has_P = true, r.last_real = 10000;
            float old_P = r.P;
            rl_out_t o;
            assert(RL_Sample(&r, 1000 + dt, 10000u + dt, false, 17, &o));
            float new_error = (983 + dt) - o.t;
            assert(o.t >= old_P && o.t < r.rec[r.head].s.t);
            if (dt <= 200)
                assert(fabsf(new_error) <= fabsf(error) + 1e-3f);
            else
                assert(fabsf(new_error - error) <= (error > 0 ? 0.1f : 0.5f) * dt + 1e-3f);
        }
    }
}

static void pause_history(float error)
{
    RL_Reset(&r);
    for (int t = 700; t <= 1150; t += 17)
        push(t, t / 10.0f, t / 20.0f, (t / 17) % 4);
    r.P = 983 + error, r.has_P = true, r.last_real = 10000;
    r.shown = 983, r.has_shown = true;
}

// Snapping on pause would jump on its FIRST frame, for either sign of the initialized error.
static void test_pause_gap(void)
{
    for (int sign = -1; sign <= 1; sign += 2) {
        pause_history(sign * 10);
        rl_out_t before, target, o;
        assert(RL_At(&r, r.P, 34, &before) && RL_At(&r, 983, 34, &target));
        assert(before.org[0] != target.org[0] && before.ang[1] != target.ang[1]);
        assert(before.backlerp != target.backlerp);
        for (int k = 1; k <= 10; k++) {
            unsigned real = 10000u + frame_time(k, 60);
            assert(RL_Sample(&r, 1000, real, true, 17, &o));
            assert(same_output(&before, &o));
            assert(r.last_real == real);
        }
        // Resume uses time since the last paused sample, not the entire pause.
        unsigned real = r.last_real + 16;
        float previous = r.P;
        assert(RL_Sample(&r, 1016, real, false, 17, &o));
        assert(o.t - previous >= 8 && o.t - previous <= 17.6f + 1e-3f);
    }
    static const unsigned gaps[] = {250, 251, 1000};
    for (unsigned j = 0; j < sizeof(gaps) / sizeof(*gaps); j++) {
        for (int ahead = 0; ahead <= 1; ahead++) {
            pause_history(ahead ? 10 : -10);
            float previous = r.P;
            // Snapshots kept arriving during the gap, including new angles and animation frames.
            for (int t = 1167; t <= 1400; t += 17)
                push(t, t / 10.0f, t / 20.0f, (t / 17) % 4);
            int ct = ahead ? 1000 : 1200;
            rl_out_t o;
            assert(RL_Sample(&r, ct, 10000u + gaps[j], false, 17, &o));
            if (gaps[j] >= 251)
                assert(o.t == (ahead ? previous : 1183));
            else {
                assert(o.t > previous); // 250 must still step, rather than snap to either target
                assert(fabsf(o.t - (ahead ? previous : 1183)) > 1);
                assert(o.t - previous >= 125 && o.t - previous <= 275);
            }
            assert(o.t >= previous);
        }
    }
    RL_Reset(&r);
    push(900, 0, 0, 0); push(1000, 10, 10, 2);
    rl_out_t o;
    assert(!r.has_P && RL_Sample(&r, 950, 12345, false, 17, &o));
    assert(o.t == 933 && r.has_P && r.last_real == 12345);
}

static void test_window_shrink(void)
{
    static const float steps[] = {7, 100};
    for (unsigned j = 0; j < sizeof(steps) / sizeof(*steps); j++) {
        float step = steps[j];
        RL_Reset(&r);
        for (int t = 0; t < 1000; t += 17)
            push(t, 0, 0, 0);
        push(1000, step, step, 1);
        r.extra = 100, r.P = 900, r.has_P = true, r.last_real = 10000;
        r.shown = 883, r.has_shown = true;
        rl_out_t before, after, old_before, old_after;
        assert(RL_At(&r, 900, 134, &before));
        assert(RL_At(&r, 883, 134, &old_before));
        assert(fabsf(before.org[0] - step * 34 / 134) < 1e-4f);
        RL_Advance(&r, 83, 17); // a low-clamp jump: the target advances 91.3 ms, real time only 1 ms
        assert(fabsf(r.extra - 91.7f) < 1e-4f);
        assert(RL_Sample(&r, 1083, 10001, false, 17, &after));
        assert(RL_At(&r, 1083 - (r.extra + 17), r.extra + 34, &old_after));
        float bound = step * 8.3f / 134;
        assert(after.t >= before.t && after.org[0] < before.org[0]); // nonvacuous backwards position
        assert(before.org[0] - after.org[0] <= bound + 1e-4f);
        assert(after.ang[1] < before.ang[1] && before.ang[1] - after.ang[1] <= bound + 1e-4f);
        assert(old_after.org[0] > old_before.org[0] && old_after.org[0] > after.org[0]);
    }
}

static void test_realtime_wrap(void)
{
    pause_history(-10);
    r.last_real = UINT_MAX - 7;
    float previous = r.P;
    rl_out_t o;
    assert(RL_Sample(&r, 1016, 8, false, 17, &o)); // unsigned elapsed = 16 across wrap
    assert(o.t > previous && o.t - previous >= 8 && o.t - previous <= 17.6f + 1e-3f);
    assert(o.t != 999 && r.last_real == 8); // snapping to the target would conceal a signed/wrap bug
}

static float delivery_uniform(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return ((*state >> 8) + 0.5f) / 16777216.0f;
}

// P=t must reproduce the positive hold baseline on EXACTLY these render frames.
static void test_delivery_jitter(void)
{
    const int I = 17;
    unsigned state = 1; // same LCG family as test_jitter; Box-Muller normal sigma=25 ms, ordered delivery
    int arrival[720];
    for (int n = 0; n < 720; n++) {
        float u = delivery_uniform(&state), v = delivery_uniform(&state);
        float normal = sqrtf(-2 * logf(u)) * cosf(6.28318530718f * v);
        int next = (int)lroundf(frame_time(n, 60) + 100 + 25 * normal);
        arrival[n] = n && next < arrival[n - 1] ? arrival[n - 1] : next;
    }
    RL_Reset(&r);
    client_clock_t c = {0};
    int n = 0, previous_real = 0, frames = 0, clock_holds = 0, target_holds = 0;
    float clock_previous = -1, target_previous = -1;
    bool received = false;
    for (int k = 0; frame_time(k, 60) < 11000; k++) {
        int real = frame_time(k, 60), old_ct = c.time;
        while (n < 720 && arrival[n] <= real) {
            int label = frame_time(n, 60);
            push(label, n * 10.0f, n / 10.0f, n % 4);
            c.previous = c.newest, c.newest = label;
            if (!received)
                c.time = c.previous, received = true;
            n++;
        }
        if (!received) {
            previous_real = real;
            continue;
        }
        client_step(&c, real - previous_real);
        float extra = RL_Advance(&r, c.time - old_ct, I);
        float S = fminf(extra + I, I + RL_MAX_EXTRA);
        rl_out_t clock, target;
        assert(RL_Sample(&r, c.time, (unsigned)real, false, I, &clock));
        assert(RL_At(&r, r.shown, S + I, &target)); // old path: its monotonically clamped target, same ring/window
        float actual_shift = c.time - r.shown;
        assert(actual_shift <= I + 100 && fabsf(actual_shift - S) < 1e-3f && clock.t <= c.newest);
        if (real >= 1000) { // one-second warm-up, ten seconds measured; no input stops or spatial repeats
            frames++;
            clock_holds += clock.org[0] == clock_previous;
            target_holds += target.org[0] == target_previous;
        }
        clock_previous = clock.org[0], target_previous = target.org[0];
        previous_real = real;
    }
    assert(frames == 600 && target_holds > 0 && c.high > 0 && c.low > 0);
    // First 2.1 run, seed=1: 9/149 holds (6.041%); smallest whole-percent ceiling is 7%, below the 10% cap.
    assert(clock_holds * 100 <= target_holds * 7);
    printf("delivery-jitter seed=1 sigma=25: frames=%d clock=%d target=%d high=%d low=%d limit=7%%\n",
           frames, clock_holds, target_holds, c.high, c.low);
}

int main(void)
{
    test_interval();
    test_even_streams();
    test_extra_growth();
    test_counterexample();
    test_stop();
    test_hysteresis();
    test_monotonic_t();
    test_angles();
    test_animation();
    test_resets();
    test_conditions();
    test_pov();
    test_jitter();
    test_single_repeat();
    test_chase_reserve();
    test_resume_anchor();
    test_turn_anchor();
    test_window();
    test_sparse_monotonic();
    test_client_clock_model();
    test_clock_empty_repeat();
    test_late_frame();
    test_clock_recovery();
    test_pause_gap();
    test_window_shrink();
    test_realtime_wrap();
    test_delivery_jitter();
    if (fails) {
        for (int i = 0; i < 2000; i++)
            if (fail_count[i])
                fprintf(stderr, "remote-lerp: line %d failed %d times\n", i, fail_count[i]);
        fprintf(stderr, "remote-lerp: %d checks failed\n", fails);
        return 1;
    }
    puts("remote-lerp: all checks passed");
    return 0;
}
