#ifdef HARNESS_OPENTDM
static int h_timeout_window(void)
{
    return h_frames - h_match_map_start + h_match_frames_to_phase(0);
}

static void h_timeout_pair(edict_t **a, edict_t **b)
{
    h_set_override("g_timeout_limit", "0");
    h_match_pair(a, b);
}

static void sc_match_countdown_grid(void)
{
    static const struct { const char *value; int windows; } durations[] = {
        { "15", 150 }, { "2.5", 25 }, { "2.25", 22 },
    };
    for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        for (int phase = 0; phase < 6; phase++) {
            edict_t *a, *b;
            if (i || phase) h_shutdown();
            h_nworld = h_ninline = 0;
            imp_cvar_set("g_match_countdown", durations[i].value);
            h_match_pair(&a, &b);
            h_client_command(a, "ready");
            h_match_phase(phase);
            int window = h_match_grid_frame();
            int deadline = window + durations[i].windows * h_div;
            int announced = h_frames + deadline - level.framenum;
            h_client_command(b, "ready");
            CHECK(tdm_match_status == MM_COUNTDOWN && level.match_start_framenum == deadline,
                  "%s s phase %d countdown deadline is %d grid windows from %d",
                  durations[i].value, phase, durations[i].windows, window);
            int prints = h_nbprints;
            h_frames_n(announced - h_frames);
            CHECK(tdm_match_status == MM_COUNTDOWN, "match has not started before the countdown deadline");
            h_frame();
            int frame = -1;
            CHECK(tdm_match_status == MM_PLAYING && h_t23_announcements("Fight!", prints, &frame) == 1 &&
                  frame == announced, "%s s phase %d Fight! on grid frame %d", durations[i].value, phase, announced);
        }
    }
}

static void sc_match_timeout_grid(void)
{
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    edict_t *runner = G_Spawn();
    runner->movetype = MOVETYPE_NOCLIP;
    runner->velocity[0] = 100;
    for (int phase = 0; phase < 6; phase++) {
        h_match_phase(phase);
        int pad = h_timeout_window() - level.realframenum, game = level.framenum;
        float start = runner->s.origin[0];
        h_client_command(a, "time");
        CHECK(tdm_match_status == MM_TIMEOUT,
              "phase %d status pauses from the timeout command", phase);
        h_frames_n(pad);
        CHECK(level.framenum == game + pad && (runner->s.origin[0] > start) == (pad != 0),
              "phase %d world and match clock advance until the first grid frame", phase);
        float frozen = runner->s.origin[0];
        h_frames_n(h_rate);
        CHECK(runner->s.origin[0] == frozen && level.framenum == game + pad,
              "phase %d world skips the frozen grid frame and stays frozen", phase);
        h_match_phase(phase + 3);
        int resume = h_timeout_window() + 104 * h_div;
        h_client_command(a, "time");
        CHECK(level.match_resume_framenum == resume,
              "phase %d resume countdown is scheduled from the grid", phase);
        h_frames_n(resume - level.realframenum);
        CHECK(tdm_match_status == MM_TIMEOUT && runner->s.origin[0] == frozen,
              "phase %d timeout is still frozen just before the resume grid frame", phase);
        h_frame();
        /* The OpenTDM game and real clocks must rejoin in the same grid phase. */
        CHECK(tdm_match_status == MM_PLAYING && runner->s.origin[0] == frozen &&
              level.framenum == game + pad + 1 && (level.realframenum - level.framenum) % h_div == 0,
              "phase %d resume grid frame counts once without running the world", phase);
        h_frame();
        CHECK(runner->s.origin[0] > frozen, "phase %d world moves after resume", phase);
    }
}

static void h_timeout_record_think(edict_t *ent)
{
    ent->count++;
    ent->style = level.framenum;
}

static edict_t *h_timeout_timer(int type, int nextthink)
{
    edict_t *timer = G_Spawn();
    timer->movetype = type;
    timer->think = h_timeout_record_think;
    timer->nextthink = nextthink;
    return timer;
}

static void sc_match_timeout_think_grid(void)
{
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    h_match_phase(1);
    int frame = h_match_grid_frame();
    edict_t *timer = h_timeout_timer(MOVETYPE_NONE, frame);
    edict_t *push = h_timeout_timer(MOVETYPE_PUSH, frame);
    edict_t *stop = h_timeout_timer(MOVETYPE_STOP, frame);
    h_client_command(a, "time");
    h_frames_n(h_timeout_window() - level.realframenum + 1);
    CHECK(!timer->count && !push->count && !stop->count, "no think ran on the frozen grid frame");
    h_view_resume(a);
    h_frames_n(h_div - 1);
    CHECK(!timer->count, "frozen ordinary think waits for the next grid frame after Fight!");
    h_frame();
    CHECK(timer->count == 1 && timer->style == frame + h_div,
          "frozen ordinary think runs once on the next grid frame");
    CHECK(push->count == 1 && push->style == frame + 1 && stop->count == 1 && stop->style == frame + 1,
          "pusher and stopper thinks are eligible on the first world step after Fight!");
}

static void sc_match_timeout_reentry(void)
{
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    for (int pause = 0; pause < 3; pause++) {
        int pad = h_timeout_window() - level.realframenum;
        int frame = level.framenum + pad;
        edict_t *timer = h_timeout_timer(MOVETYPE_NONE, frame);
        if (pause < 2)
            h_client_command(a, "time");
        else
            ge->ClientDisconnect(b);
        CHECK(tdm_match_status == MM_TIMEOUT, "pause %d enters timeout immediately after Fight!", pause);
        h_frames_n(pad);
        CHECK(level.framenum == frame, "pause %d can run the world up to its own grid frame", pause);
        h_frame();
        CHECK(level.framenum == frame && !timer->count &&
              (h_div == 1 || timer->nextthink == frame + h_div),
              "pause %d freezes and defers its own grid frame", pause);
        if (pause < 2) h_view_resume(a);
        G_FreeEdict(timer);
    }
}

static void sc_match_timeout_win_grid(void)
{
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    ge->ClientDisconnect(b);
    CHECK(tdm_match_status == MM_TIMEOUT, "disconnect starts a timeout");
    h_frames_n(h_rate);
    h_match_phase(1);
    int resume = h_timeout_window() + h_div, prints = h_nbprints;
    int announcement = h_frames + resume - level.realframenum;
    h_client_command(a, "win");
    CHECK(level.match_resume_framenum == resume, "win schedules one grid interval from its coming grid frame");
    h_frames_n(resume - level.realframenum);
    CHECK(tdm_match_status == MM_TIMEOUT, "win does not resume before its grid deadline");
    h_frame();
    int frame = -1;
    CHECK(tdm_match_status == MM_SCOREBOARD && h_t23_announcements("Fight!", prints, &frame) == 1 &&
          frame == announcement, "win resumes and settles the forfeit on its grid frame");
}

static void h_check_timeout_exit_clock(const char *action, int first_grid, int frozen)
{
    int game = frozen + max(0, h_frames - first_grid);
    CHECK(level.realframenum == h_frames && level.framenum == game,
          "%s clock follows external frames: world=%d real=%d match=%d expected=%d",
          action, h_frames, level.realframenum, level.framenum, game);
}

static void h_timeout_exit_grid(qboolean restart)
{
    const char *action = restart ? "vote restart" : "break";
    h_set_override("g_match_countdown", "2.5");
    h_set_override("g_intermission_time", "1");
    h_set_override("g_auto_rejoin_match", "1");
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    h_client_command(a, "time");
    h_frames_n(h_rate);
    h_frames_n((1 + h_div - h_frames % h_div) % h_div);
    CHECK(tdm_match_status == MM_TIMEOUT, "%s fixture is in timeout", action);
    int first_grid = (h_frames + h_div - 1) / h_div * h_div;
    int frozen = level.framenum;
    edict_t *runner = G_Spawn();
    runner->movetype = MOVETYPE_NOCLIP;
    runner->velocity[0] = 100;
    int prints = h_nbprints;
    if (restart) {
        h_client_command(a, action);
        h_client_command(b, "vote yes");
    } else {
        a->client->pers.admin = true;
        h_client_command(a, action);
    }
    CHECK(tdm_match_status == (restart ? MM_COUNTDOWN : MM_SCOREBOARD),
          "%s leaves the timeout", action);
    while (h_frames < first_grid) {
        h_frame();
        h_check_timeout_exit_clock(action, first_grid, frozen);
        CHECK(runner->s.origin[0] == 0, "%s world stays frozen until the external grid frame", action);
    }
    h_frame();
    CHECK(runner->s.origin[0] > 0, "%s world runs on the external grid frame", action);
    h_check_timeout_exit_clock(action, first_grid, frozen);

    int countdown = first_grid;
    if (!restart) {
        h_frames_n(2 * h_rate);
        CHECK(tdm_match_status == MM_WARMUP, "break ends intermission on the same map");
        h_check_timeout_exit_clock(action, first_grid, frozen);
        h_client_command(a, "ready");
        h_client_command(b, "ready");
        CHECK(tdm_match_status == MM_COUNTDOWN, "both teams can ready after break");
        countdown = (h_frames + h_div - 1) / h_div * h_div;
    }

    int fight = countdown + 25 * h_div;
    h_frames_n(fight - h_frames);
    h_check_timeout_exit_clock(action, first_grid, frozen);
    CHECK(tdm_match_status == MM_COUNTDOWN, "%s still counts down before the expected Fight!", action);
    h_frame();
    int announced = -1;
    CHECK(tdm_match_status == MM_PLAYING && h_t23_announcements("Fight!", prints, &announced) == 1 &&
          announced == fight && announced % h_div == 0,
          "%s Fight! comes on the external grid frame %d (got %d)", action, fight, announced);
    h_check_timeout_exit_clock(action, first_grid, frozen);
}

static void sc_match_timeout_restart_grid(void) { h_timeout_exit_grid(true); }
static void sc_match_timeout_break_grid(void) { h_timeout_exit_grid(false); }
#endif
