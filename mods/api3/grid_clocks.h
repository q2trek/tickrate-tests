/* Weak imports let these scenarios also run against the pre-grid module.
 * Before the helpers existed, the deadlines used the next world frame and
 * truncated seconds at the world rate. At 10 Hz those are the same rules. */
extern int G_WindowFrame(int framenum) __attribute__((weak));
extern int G_GridFrames(float seconds) __attribute__((weak));

static int h_grid_window(void)
{
    return G_WindowFrame ? G_WindowFrame(level.framenum) : level.framenum;
}

static int h_grid_duration(float seconds)
{
    return G_GridFrames ? G_GridFrames(seconds) : (int)(seconds * h_rate);
}

static void sc_grid_durations(void)
{
    h_scene_base();
    h_start(h_ents);
    for (int phase = 0; phase < 6; phase++) {
        /* Commands at 60 Hz, with a command at the tick boundary going first. */
        int frame = 4 * h_div + (phase * h_div + 5) / 6;
        h_frames_n(frame - h_frames);
        int start = h_grid_window();
        int delay = h_grid_duration(0.25f);
        int wait = h_grid_duration(0.35f);

        CHECK(start == (4 + (phase != 0)) * h_div,
              "phase %d window frame: world=%d window=%d", phase, frame, start);
        CHECK(delay == 2 * h_div && wait == 3 * h_div,
              "phase %d whole windows: delay=%d wait=%d", phase, delay, wait);
    }
}

static int h_grid_think_frame;

static void h_grid_record_think(edict_t *ent)
{
    h_grid_think_frame = h_frames;
    G_FreeEdict(ent);
}

static void h_grid_check_deadline(void)
{
    int expected = ((h_frames + h_div - 1) / h_div + 1) * h_div;
    edict_t *probe = G_Spawn();
    probe->think = h_grid_record_think;
    probe->nextthink = h_grid_window() + h_grid_duration(0.1f);
    h_grid_think_frame = -1;
    h_frames_n(expected - h_frames + 1);
    CHECK(h_grid_think_frame == expected, "grid timer fired at world frame %d (expected %d)",
          h_grid_think_frame, expected);
}

#ifdef HARNESS_OPENTDM
static void h_grid_exit(bool same_map)
{
    const char *map = same_map ? "harness" : "missing";
    h_scene_base();
    h_start(h_ents);
    h_frames_n(2);  /* SV_SpawnServer settling frames */
    EndDMLevel(map);
    h_frame();    /* intermission requests exit on the next frame */

    h_cmdtext[0] = 0;
    int frame = level.framenum, real = level.realframenum;
    h_frame();
    int advance = h_div > 1;
    CHECK(strstr(h_cmdtext, va("gamemap \"%s\"\n", map)) != NULL,
          "exit queued gamemap for %s", map);
    CHECK(level.framenum == frame + advance && level.realframenum == real + advance,
          "exit frame counters: game=%d->%d real=%d->%d", frame, level.framenum, real, level.realframenum);

    if (same_map) {
        /* A successful gamemap calls SpawnEntities and restarts the engine clock. */
        ge->SpawnEntities(map, h_ents, "");
        h_frames = 0;
        h_frames_n(2);
    } else {
        /* Leaving the queued command unhandled models the engine rejecting it. */
        int prints = h_nbprints;
        h_frame();
        CHECK(h_nbprints > prints && strstr(h_bprints[prints].text, "was not found on the server"),
              "failed gamemap reported and play continued on %s", level.mapname);
        CHECK(!level.exitintermission && !strcmp(level.mapname, "harness"),
              "failed gamemap returned to the current map");
    }

    for (int i = 0; i <= h_div; i++, h_frame()) {
        CHECK(FRAMESYNC == (h_frames % h_div == 0) && (level.framenum - h_frames) % h_div == 0,
              "phase after gamemap: world=%d game=%d real=%d", h_frames, level.framenum, level.realframenum);
    }
    h_grid_check_deadline();
}

static void sc_grid_exit_missing(void) { h_grid_exit(false); }
static void sc_grid_exit_same(void) { h_grid_exit(true); }
#else
static void h_grid_input_phase(int phase)
{
    int window = (h_frames + h_div - 1) / h_div + 1;
    h_frames_n(window * h_div + (phase * h_div + 5) / 6 - h_frames);
}

static void h_grid_sv_reset(void)
{
    h_tokenize("sv reset");
    ge->ServerCommand();
}

static void h_grid_after_reset(int phase, int outer_frame)
{
    for (int i = 0; i <= h_div; i++, h_frame()) {
        int elapsed = 2 * h_div + outer_frame + i;
        CHECK(FRAMESYNC == (h_frames % h_div == 0),
              "phase %d after reset: world=%d level=%d", phase, h_frames, level.framenum);
        CHECK(KEYFRAME(FRAMEDIV) - level.framenum == h_div - h_frames % h_div,
              "phase %d next key frame after reset at world=%d", phase, h_frames);
        CHECK(level.framenum == elapsed, "phase %d reset elapsed frames: got %d, expected %d",
              phase, level.framenum, elapsed);
        CHECK(fabsf(level.time - elapsed * FRAMETIME) < 0.00001f,
              "phase %d reset level.time: got %.6f, expected %.6f", phase, level.time, elapsed * FRAMETIME);
    }
    h_grid_check_deadline();
}

static void sc_grid_reset_command(void)
{
    h_scene_base();
    h_start(h_ents);
    for (int phase = 0; phase < 6; phase++) {
        h_grid_input_phase(phase);
        h_grid_sv_reset();
        CHECK(FRAMESYNC == (h_frames % h_div == 0),
              "phase %d after reset: first sv reset at world frame %d", phase, h_frames);
        h_grid_sv_reset(); // No engine frame between the two commands.
        h_grid_after_reset(phase, 0);
    }
}

static void sc_grid_reset_countdown(void)
{
    h_scene_base();
    h_ent("{ \"classname\" \"info_player_deathmatch\" \"origin\" \"800 0 24\" }\n");
    h_set_override("g_warmup", "1");
    h_set_override("g_countdown_time", "2");
    h_start(h_ents);
    edict_t *a = h_connect(0, "a"), *b = h_connect(1, "b");
    h_frames_n(3 * h_rate);
    h_client_command(a, "join");
    h_client_command(b, "join");
    for (int phase = 0; phase < 6; phase++) {
        h_grid_input_phase(phase);
        h_grid_sv_reset();
        h_frames_n(3 * h_rate);
        h_client_command(a, "ready");
        h_grid_input_phase(phase);
        int pad = (h_div - h_frames % h_div) % h_div;
        int window = h_frames + pad, window_level = level.framenum + pad;
        int start = window + 2 * h_rate;
        int prints = h_nbprints, messages = h_nmsgs;
        h_client_command(b, "ready");
        CHECK(level.match_state == MS_COUNTDOWN, "phase %d last ready starts countdown", phase);
        CHECK(level.countdown_framenum == window_level,
              "phase %d countdown starts timing from the grid frame %d", phase, window_level);
        while (h_frames <= start && level.match_state == MS_COUNTDOWN) {
            if (h_frames % h_div == 0)
                h_think(a, BUTTON_ATTACK, 1);
            h_frame();
        }
        CHECK(level.match_state == MS_PLAYING, "phase %d countdown reset started the match", phase);
        CHECK(h_frames - 1 == start, "phase %d match starts on the countdown's deadline %d", phase, start);
        int time_msgs = 0, fight = -1;
        for (int i = messages; i < h_nmsgs; i++) {
            if (h_msgs[i].svc == H_SVC_CONFIGSTRING && h_msgs[i].sub == CS_TIME) {
                CHECK(h_msgs[i].frame == window + time_msgs * h_rate,
                      "phase %d countdown message %d on world frame %d", phase, time_msgs, h_msgs[i].frame);
                time_msgs++;
            }
        }
        for (int i = prints; i < h_nbprints; i++)
            if (strstr(h_bprints[i].text, "Match has started!")) fight = h_bprints[i].frame;
        CHECK(time_msgs == 2 && fight == start, "phase %d two countdown messages then Fight on world frame %d", phase, fight);
        CHECK(!h_shots(a, MZ_BLASTER, messages), "phase %d no shot before the match starts", phase);
        h_grid_after_reset(phase, 1);
        for (int i = 0; i < h_rate && !h_shots(a, MZ_BLASTER, messages); i++) {
            if (h_frames % h_div == 0)
                h_think(a, BUTTON_ATTACK, 1);
            h_frame();
        }
        int first_shot = -1;
        for (int i = messages; i < h_nmsgs; i++) {
            if (h_msgs[i].svc == H_SVC_MUZZLEFLASH && h_msgs[i].entnum == NUM(a)) {
                first_shot = h_msgs[i].frame;
                break;
            }
        }
        CHECK(first_shot == start + 6 * h_div,
              "phase %d first shot after Fight on a grid frame (world %d, windows after Fight %d)",
              phase, first_shot, (first_shot - start) / h_div);
        h_think(a, 0, 1);
    }
}

static void h_grid_reset_intermission(bool same_map)
{
    h_scene_base();
    h_set_override("g_intermission_time", "5");
    h_start(h_ents);
    for (int phase = 0; phase < 6; phase++) {
        h_grid_input_phase(phase);
        strcpy(level.nextmap, same_map ? level.mapname : "missing");
        h_cmdtext[0] = 0;
        BeginIntermission();
        for (int i = 0; i <= 5 * h_rate + 6 && level.match_state == MS_INTERMISSION; i++)
            h_frame();
        CHECK(level.match_state == MS_PLAYING && !strcmp(level.mapname, "harness"),
              "phase %d intermission reset continued play on the current map", phase);
        CHECK(same_map ? !h_cmdtext[0] : strstr(h_cmdtext, "gamemap \"missing\"\n") != NULL,
              "phase %d intermission %s gamemap", phase, same_map ? "skips" : "queued");
        h_grid_after_reset(phase, 1);
    }
}

static void sc_grid_reset_missing(void) { h_grid_reset_intermission(false); }
static void sc_grid_reset_same(void) { h_grid_reset_intermission(true); }

static void sc_grid_reset_phase_reader(void)
{
    h_scene_base();
    h_start(h_ents);
    h_grid_input_phase(1);
    h_grid_sv_reset();
    h_real_phase(0);
    CHECK(h_frames % h_div == 0, "phase reader reaches the engine keyframe after reset (world %d)", h_frames);
}

static void sc_grid_reset_slow(void)
{
    h_hurt_scene(16);
    edict_t *p = h_join(0, "p1");
    for (int whole_second = 0; whole_second <= 1; whole_second++) {
        h_grid_input_phase(1);
        h_grid_sv_reset();
        int second = h_frames + h_rate - 2 * h_div;
        second += (h_div - second % h_div) % h_div;
        int window = whole_second ? second : second - h_div;
        h_frames_n(window - h_div + 1 - h_frames);

        int hits[H_MAXTICKS], sounds;
        int total = h_hurt_run(p, h_rate + 1, 1, hits, &sounds);
        CHECK(total == 2 && hits[0] == 1 && hits[h_rate] == 1,
              "SLOW after reset hits on contact and one second later");
        CHECK(sounds == 2 * whole_second,
              "SLOW after reset sounds only in whole-second windows (got %d, expected %d)",
              sounds, 2 * whole_second);
    }
}

static void sc_grid_reset_map(void)
{
    h_scene_base();
    int model = h_add_inline(0, 0, 0, 32, 32, 8);
    h_ent("{ \"classname\" \"weapon_railgun\" \"origin\" \"0 256 64\" }\n"
          "{ \"classname\" \"func_train\" \"model\" \"*%d\" \"targetname\" \"train\" \"target\" \"first\" \"spawnflags\" \"1\" }\n"
          "{ \"classname\" \"path_corner\" \"targetname\" \"first\" \"origin\" \"0 512 64\" }\n"
          "{ \"classname\" \"misc_banner\" \"origin\" \"0 768 64\" }\n"
          "{ \"classname\" \"misc_blackhole\" \"origin\" \"0 1024 64\" }\n"
          "{ \"classname\" \"viewthing\" \"origin\" \"0 1280 64\" }\n"
          "{ \"classname\" \"target_string\" \"targetname\" \"display\" }\n"
          "{ \"classname\" \"func_clock\" \"target\" \"display\" \"spawnflags\" \"1\" \"count\" \"10\" }\n"
          "{ \"classname\" \"func_timer\" \"target\" \"bell\" \"spawnflags\" \"1\" \"wait\" \"1\" }\n"
          "{ \"classname\" \"target_speaker\" \"targetname\" \"bell\" \"noise\" \"world/10_0.wav\" }\n", model);
    h_start(h_ents);
    h_frames_n(2 * h_div + h_div - 1);
    h_grid_sv_reset();
    edict_t *item = h_by_class("weapon_railgun", 0), *train = h_by_class("func_train", 0);
    edict_t *clock = h_by_class("func_clock", 0), *timer = h_by_class("func_timer", 0);
    edict_t *bell = h_by_class("target_speaker", 0);
    edict_t *banner = h_by_class("misc_banner", 0), *blackhole = h_by_class("misc_blackhole", 0);
    edict_t *viewthing = h_by_class("viewthing", 0);
    if (!item || !train || !clock || !timer || !bell || !banner || !blackhole || !viewthing)
        imp_error("missing reset map fixture");
    CHECK(item->nextthink == 2 && item->solid == SOLID_NOT && !item->touch &&
          train->nextthink == 2 && train->s.origin[1] == 512,
          "map bootstrap item deadline=%d solid=%d train deadline=%d y=%.0f",
          item->nextthink, item->solid, train->nextthink, train->s.origin[1]);
    printf("NEXTTHINK reset-return item=%d train=%d clock=%d timer=%d\n",
           item->nextthink, train->nextthink, clock->nextthink, timer->nextthink);

    int phase = h_frames % h_div, pad = (h_div - phase) % h_div;
    /* Keep the successful 10 Hz trace comparable to the previous reset fixture. */
    if (viewthing->nextthink != 5 * h_div - phase)
        CHECK(0, "map KEYFRAME deadline uses the restored phase (got %d, expected %d)",
              viewthing->nextthink, 5 * h_div - phase);
    int banner_due = h_div == 1 ? 2 : h_div;
    if (banner->nextthink != banner_due || blackhole->nextthink != 2 * h_div)
        CHECK(0, "map short animations retain their bootstrap deadlines (banner=%d blackhole=%d)",
              banner->nextthink, blackhole->nextthink);
    int banner_frame = banner->s.frame, blackhole_frame = blackhole->s.frame;
    int sounds = h_nsounds, clocks = 0, timers = 0;
    for (int frame = 2 * h_div; frame <= 4 * h_rate + pad; frame++) {
        int before = clock->health;
        h_frame();
        if (frame == 2 * h_div) {
            CHECK(item->solid == SOLID_TRIGGER && item->touch == Touch_Item &&
                  item->nextthink == 0 && train->nextthink == 0,
                  "first external frame consumes the item and train deadlines");
            if (banner->s.frame != (banner_frame + 1) % 16 || blackhole->s.frame != blackhole_frame + 1)
                CHECK(0, "map short animations advance on the first external frame");
        }
        if (clock->health != before) {
            clocks++;
            CHECK(frame == clocks * h_rate && clock->health == clocks,
                  "map clock event %d at %.6f s", clocks, (double)frame / h_rate);
        }
        for (; sounds < h_nsounds; sounds++) {
            if (h_sounds_log[sounds].entnum != NUM(bell)) continue;
            timers++;
            int due = (timers + 1) * h_rate + pad;
            CHECK(frame == due && h_sounds_log[sounds].frame % h_div == 0,
                  "map timer event %d at %.6f s (expected %.6f s)",
                  timers, (double)frame / h_rate, (double)due / h_rate);
        }
    }
    CHECK(clocks == 4 && timers == 3, "map boot timers fired %d clock and %d timer events", clocks, timers);
}

static void sc_grid_reset_thresholds(void)
{
    h_scene_base();
    h_ent("{ \"classname\" \"info_player_deathmatch\" \"origin\" \"128 0 24\" }\n"
          "{ \"classname\" \"info_player_deathmatch\" \"origin\" \"512 0 24\" }\n");
    h_set_override("g_warmup", "1");
    h_set_override("dmflags", "512"); // The normal spawn rule chooses the farthest spot.
    h_start(h_ents);
    edict_t *a = h_connect(0, "threshold-a"), *b = h_connect(1, "threshold-b");
    strcpy(a->client->pers.netname, "threshold-a");
    strcpy(b->client->pers.netname, "threshold-b");
    h_frames_n(4 * h_rate + h_div - 1);
    h_client_command(a, "join");
    h_client_command(b, "join");
    imp_cvar_set("g_vote_mask", va("%d", VOTE_TIMELIMIT));
    h_grid_sv_reset();
    int zero = h_frames - 2 * h_div;

    edict_t *slot = G_Spawn();
    h_frames_n(zero + 2 * h_rate - 1 - h_frames);
    G_FreeEdict(slot);
    CHECK(G_Spawn() == slot, "edict reuse just before 2 s uses the early rule");
    h_frame();
    G_FreeEdict(slot);
    CHECK(G_Spawn() != slot, "edict reuse at 2 s uses the cooldown");

    h_frames_n(zero + 3 * h_rate - 1 - h_frames);
    CHECK(h_t23_print(a, "ready", "readyness too soon") && !a->client->resp.ready,
          "first ready is blocked just before 3 s");
    h_frame();
    h_client_command(a, "ready");
    CHECK(a->client->resp.ready, "first ready opens at 3 s");

    h_frames_n(zero + 5 * h_rate - 1 - h_frames);
    for (int edge = 0; edge < 2; edge++) {
        bool open = edge != 0;
        CHECK(h_t23_print(a, "vote timelimit", open ? "Argument required" : "initiate votes too soon"),
              "first vote %s the 5 s threshold", open ? "at" : "before");
        VectorSet(a->s.origin, 0, 0, 24);
        gi.linkentity(a);
        b->health = 0;
        Q_srand(0);
        PutClientInServer(b);
        CHECK(b->s.origin[0] == (open ? 512 : 128) && a->health > 0,
              "telefrag avoidance %s the 5 s threshold (spawn x=%.0f)", open ? "at" : "before", b->s.origin[0]);
        h_client_command(a, "spectate");
        CHECK(PlayerSpawned(a) != open, "mode change %s the 5 s threshold", open ? "at" : "before");
        if (!open)
            h_frame();
    }

    h_frames_n(zero + 600 * h_rate - 1 - h_frames);
    for (int edge = 0; edge < 2; edge++) {
        const char *time = edge ? "10" : "9:59";
        int mark = h_ntexts;
        h_client_command(b, "players");
        DeathmatchScoreboardMessage(b, true);
        const htext_t *players = h_text_to(b, 0, "threshold-b", mark);
        const htext_t *scores = h_text_to(b, H_SVC_LAYOUT, "threshold-b", mark);
        char shown[16];
        CHECK(players && sscanf(players->text, "%*d %*s %*d %15s", shown) == 1 && !strcmp(shown, time),
              "players time format %s 10 min", edge ? "at" : "before");
        CHECK(scores && sscanf(strstr(scores->text, "threshold-b") + strlen("threshold-b"),
                               "%*d %*d %*d %*d %15s", shown) == 1 && !strcmp(shown, time),
              "scoreboard time format %s 10 min", edge ? "at" : "before");
        if (!edge)
            h_frame();
    }
}
#endif
