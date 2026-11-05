// Host test of q2pro src/client/remote_lerp.c (cl_remote_lerp): history, delay, teleports, stops, rate changes.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "remote_lerp.h"

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
        assert(RL_Sample(&r, cltime, I, &o));
        if (check)
            check(cltime, extra, &o);
    }
    return extra;
}

static void extra_zero(int cltime, float extra, const rl_out_t *o) { (void)cltime; (void)o; assert(extra == 0); }

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
    stream(60, 1, 3000, NULL, extra_zero);              // 60 packets/s on S60: labels 16, 17, 17
    stream(30, 1, 3000, NULL, extra_zero);              // 30 on S30
    assert(stream(60, 2, 3000, NULL, NULL) == 17);      // 30 on S60, the rare sender: 34 - 17
}

static void test_extra_growth(void)
{
    RL_Reset(&r);
    push(0, 0, 0, 0); push(34, 10, 0, 0); push(68, 20, 0, 0);  // D = 34
    float last = 0;
    int ms = 0;
    while (RL_Advance(&r, 7, 17) < 17) {  // 144 Hz: cl.time grows 7 ms
        assert(r.extra > last);           // float: 0.7 per frame, not truncated to 0
        last = r.extra;
        ms += 7;
        assert(ms <= 175);
    }
    assert(ms >= 161 && r.extra == 17);
    assert(RL_Advance(&r, 7, 17) == 17);
    r.D = 17;                             // an even sender again: extra decays to exactly 0, the path goes off
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
static void stop_check(int cltime, float extra, const rl_out_t *o)
{
    // the stop is drawn standing on the last position before it until k1.t - D after it
    if (o->t > stop_from + 100 && o->t <= stop_to - 34) {
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
    assert(RL_Sample(&r, 700, 17, &o));
    float t0 = o.t, e0 = r.extra;
    for (int n = 20; n < 40; n++)        // D grows (stop-and-go sender) while cl.time stands: high clamp, pause
        push(646 + (n - 19) * 90, n * 10.0f, 0, 0);
    assert(RL_Advance(&r, 0, 17) == e0); // no growth of cl.time, no change of extra
    assert(RL_Sample(&r, 700, 17, &o) && o.t >= t0);
    assert(RL_Sample(&r, 699, 17, &o) && o.t >= t0);  // never drawn backwards
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
        assert(RL_Sample(&r, frame_time(k, 144), 17, &o));
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
    assert(RL_Sample(&r, 500, 17, &o));
    RL_Reset(&r);
    assert(!r.has_shown && r.extra == 0 && r.ngaps == 0 && !RL_Sample(&r, 400, 17, &o));
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
    assert(!RL_EntityOn(&on, 3, 8, 0, 0));        // extra 0: the old code
    c = on, c.demo = true;    assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    c = on, c.div = 1;        assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    c = on, c.nolerp = 1;     assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    c = on, c.enabled = false; assert(!RL_EntityOn(&c, 3, 8, 0, 17));
    // camera: chase = frame.clientNum != own clientNum, without prediction
    assert(RL_CameraOn(&on, false, 4, 0, 17));
    assert(!RL_CameraOn(&on, true, 4, 0, 17));    // predicted view
    assert(!RL_CameraOn(&on, false, 0, 0, 17));   // cl_predict 0 without chase, OpenTDM third-person
    assert(!RL_CameraOn(&on, false, 4, 0, 0));
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

// jitter profile (senders that jitter): the mover sends 60 moves/s with send moments jittered by 30 % of the
// interval, S60, observer at 144 Hz; a server frame shows every move that arrived by it (none: a repeat, two: a double
// step). Freeze of the drawn position, old lerp (last two snapshots) vs this path, which the client takes when
// extra > 0. Live jitter: "проверяется тестом, вживую не проверяется: в humblenet-слое нет имитации".
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
        assert(RL_Sample(&r, cltime, I, &o) && extra <= RL_MAX_EXTRA);
        float drawn = extra > 0 ? o.org[0] : old;
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
    puts("remote-lerp: all checks passed");
    return 0;
}
