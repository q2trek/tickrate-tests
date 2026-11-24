static int h_match_map_start;

static void h_match_pair(edict_t **a, edict_t **b)
{
    // The harness clock keeps counting across fresh maps within one scenario.
    h_match_map_start = h_frames;
    h_tdm_pair(a, b);
}

static int h_match_frames_to_phase(int phase)
{
    int current = (h_frames - h_match_map_start) % h_div;
    return (phase + h_div - current) % h_div;
}

static int h_match_grid_frame(void)
{
    return level.framenum + h_match_frames_to_phase(0);
}

static void h_match_phase(int phase)
{
    int tick_phase = (phase * h_div + 5) / 6 % h_div;
    h_frames_n(h_match_frames_to_phase(tick_phase));
}

static void sc_match_kill_grid(void)
{
    edict_t *a, *b;
    h_match_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    h_place(b, 800, 0, 24);
    h_tick(b, 0, 1);
    for (int phase = 0; phase < 6; phase++) {
        h_match_phase(phase);
        a->movetype = MOVETYPE_NOCLIP; /* an injected spawn leaves no body */
        respawn(a);
        int deadline = h_match_grid_frame() + 5 * h_rate;
        h_frames_n(deadline - h_div - level.framenum);
        CHECK(!a->deadflag && a->health > 0, "phase %d kill fixture is alive at the command boundary", phase);
        h_client_command(a, "kill");
        CHECK(!a->deadflag, "phase %d kill rejected before the fifth second's grid window", phase);
        h_frame();
        h_client_command(a, "kill");
        CHECK(a->deadflag, "phase %d kill allowed in the fifth second's grid window", phase);
    }
}

static void sc_match_respawn_deadline(void)
{
    edict_t *a, *b;
#ifdef HARNESS_OPENTDM
    h_set_override("g_respawn_time", "3");
    const int delay = 3 * h_rate;
#else
    const int delay = h_rate;
#endif
    h_match_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    for (int phase = 0; phase < 6; phase++) {
        h_match_phase(phase);
        a->movetype = MOVETYPE_NOCLIP;
        respawn(a);
        int deadline = h_match_grid_frame() + delay;
        h_hit(a, 200);
        CHECK(a->deadflag, "phase %d lethal damage starts the respawn delay", phase);
        CHECK(a->client->respawn_framenum == deadline,
              "phase %d respawn deadline from the death window: got %d, expected %d",
              phase, a->client->respawn_framenum, deadline);
    }
}

static void h_match_vote_pair(edict_t **a, edict_t **b, const char *minutes)
{
#ifdef HARNESS_OPENTDM
    imp_cvar_set("g_match_time", minutes);
#else
    imp_cvar_set("timelimit", minutes);
    imp_cvar_set("g_vote_mask", va("%d", VOTE_TIMELIMIT));
#endif
    h_nworld = h_ninline = 0;
    h_match_pair(a, b);
}

static void sc_match_vote_end_grid(void)
{
    for (int phase = 0; phase < 6; phase++) {
        edict_t *a, *b;
        if (phase) {
            h_shutdown();
            h_frames = 0; // The next fixture starts a new server map.
        }
#ifdef HARNESS_OPENTDM
        h_match_vote_pair(&a, &b, "120");
#else
        h_match_vote_pair(&a, &b, "2");
#endif
        if (!h_t23_start(a, b)) return;
        h_frames_n(61 * h_rate);
        h_match_phase(phase);
        int window = h_match_grid_frame();
#ifdef HARNESS_OPENTDM
        teaminfo[TEAM_A].score = 1;
#endif
        h_client_command(a, "vote timelimit 1");
        h_client_command(b, "vote yes");
#ifdef HARNESS_OPENTDM
        CHECK(!vote.active && g_match_time->value == 60, "phase %d shorter timelimit accepted", phase);
        CHECK(level.match_end_framenum == window,
              "phase %d vote ends at the coming grid frame %d", phase, window);
#else
        CHECK(!level.vote.proposal && timelimit->value == 1, "phase %d shorter timelimit accepted", phase);
#endif
        h_frames_n(window - level.framenum);
#ifdef HARNESS_OPENTDM
        CHECK(tdm_match_status == MM_PLAYING, "phase %d vote keeps match active before its grid frame", phase);
        h_frame();
        CHECK(tdm_match_status == MM_SCOREBOARD, "phase %d vote ends the match on its grid frame", phase);
#else
        CHECK(level.match_state == MS_PLAYING, "phase %d vote keeps match active before its grid frame", phase);
        h_frame();
        CHECK(level.match_state == MS_INTERMISSION, "phase %d vote ends the match on its grid frame", phase);
#endif
    }
}

#ifdef HARNESS_OPENTDM
static void sc_match_overtime_vote_grid(void)
{
    for (int mode = 1; mode <= 2; mode++) {
        for (int extend = 0; extend <= 1; extend++) {
            for (int phase = 0; phase < 6; phase++) {
                edict_t *a, *b;
                if (mode != 1 || extend || phase) h_shutdown();
                imp_cvar_set("g_tie_mode", va("%d", mode));
                h_match_vote_pair(&a, &b, "90");
                if (!h_t23_start(a, b)) return;
                int status = mode == 1 ? MM_OVERTIME : MM_SUDDEN_DEATH;
                h_frames_n(level.match_end_framenum - level.framenum + 1);
                CHECK(tdm_match_status == status, "mode %d enters a tiebreak", mode);
                h_match_phase(phase);
                int prints = h_nbprints, accepted = h_frames;
                h_client_command(a, extend ? "vote timelimit 3" : "vote timelimit 1");
                h_client_command(b, "vote yes");
                CHECK(!vote.active && g_match_time->value == (extend ? 180 : 60),
                      "mode %d phase %d timelimit accepted", mode, phase);
                int frame = -1;
                int count = h_t23_announcements("Timelimit extended", prints, &frame);
                CHECK(count == extend && (!extend || frame == accepted),
                      "mode %d phase %d extension announcement matches the vote", mode, phase);
                CHECK(tdm_match_status == (extend ? MM_PLAYING : status),
                      "mode %d phase %d only a real extension leaves the tiebreak", mode, phase);
                if (!extend) {
                    h_frames_n(h_match_grid_frame() - level.framenum);
                    CHECK(tdm_match_status == status,
                          "mode %d phase %d no early end from the shorter limit", mode, phase);
                    teaminfo[TEAM_A].score = 1;
                    h_frame();
                    CHECK(tdm_match_status == MM_SCOREBOARD,
                          "mode %d phase %d finishes on the next grid once the tie is broken", mode, phase);
                }
            }
        }
    }
}
#endif
