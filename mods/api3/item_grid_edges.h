static void sc_item_grid_dropped_quad_expiry(void)
{
    edict_t *a, *b;
    h_tdm_pair(&a, &b);
    if (!h_t23_start(a, b)) return;
    for (int active = 0; active < 2; active++) {
        for (int remaining = h_div; remaining >= 0; remaining -= h_div) {
            while (h_frames % h_div) h_frame();
            int expiry = level.framenum + 2 * h_rate;
            edict_t *drop = Drop_Item(a, (gitem_t *)FindItem("Quad Damage"));
            /* The death drop is immediately touchable through the normal pickup path. */
            drop->spawnflags |= DROPPED_PLAYER_ITEM;
            drop->movetype = MOVETYPE_NONE;
            drop->touch = Touch_Item;
            drop->think = G_FreeEdict;
            drop->nextthink = expiry;
            h_frames_n(2 * h_rate - remaining - (h_div > 1));
            int prior = active ? expiry + h_rate : 0;
            b->client->quad_framenum = prior;
            drop->touch(drop, b, NULL, NULL);
            CHECK(!drop->inuse, "quad can be picked up before its expiry think");
            int expected = active ? prior + remaining : remaining ? expiry : 0;
            CHECK((int)b->client->quad_framenum == expected,
                  "dropped quad adds only its remaining grid frames (active %d, remaining %d, got %d, want %d)",
                  active, remaining, (int)b->client->quad_framenum, expected);
        }
    }
}

static int item_grid_timer_calls, item_grid_timer_last, item_grid_timer_gap;

static void item_grid_timer_target(edict_t *self, edict_t *other, edict_t *activator)
{
    int gap = h_frames - item_grid_timer_last;
    if (item_grid_timer_calls > 1 && gap != h_div)
        item_grid_timer_gap = gap;
    item_grid_timer_calls++;
    item_grid_timer_last = h_frames;
}

static void sc_item_grid_short_timer(void)
{
    h_scene_base();
    h_ent("{\n\"classname\" \"func_timer\"\n\"target\" \"short_tick\"\n\"wait\" \"0.05\"\n}\n");
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    edict_t *sink = G_Spawn();
    sink->targetname = "short_tick";
    sink->use = item_grid_timer_target;
    edict_t *timer = h_by_class("func_timer", 0);
    CHECK(timer && timer->use, "short timer spawned");
    if (!timer || !timer->use) return;
    for (int phase = 0; phase < h_div; phase++) {
        while (h_frames % h_div != phase) h_frame();
        int first = h_frames;
        int repeat = first + (phase ? h_div - phase : 0);
        item_grid_timer_calls = item_grid_timer_gap = 0;
        timer->use(timer, p, p);
        CHECK(item_grid_timer_calls == 1 && item_grid_timer_last == first,
              "short timer activates immediately on manual use");
        h_frames_n(repeat - h_frames);
        CHECK(item_grid_timer_calls == 1, "short timer repeats no earlier than the coming grid frame");
        h_frames_n(3 * h_div);
        CHECK(item_grid_timer_calls == 4 && item_grid_timer_gap == 0,
              "short timer repeats exactly once per grid window (got %d calls, bad gap %d)",
              item_grid_timer_calls, item_grid_timer_gap);
        timer->use(timer, p, p);
    }
}

#ifndef HARNESS_OPENTDM
static void sc_item_grid_spawn_protection(void)
{
    h_set_override("g_protection_time", "1.05");
    h_scene_base();
    h_start(h_ents);
    edict_t *p = h_join(0, "p1");
    while (h_frames % h_div) h_frame();
    respawn(p);
    int protection = p->client->invincible_framenum;
    CHECK(protection == (int)(level.framenum + 1.05f * h_rate),
          "spawn protection retains its configured duration");
    int protection_frames = protection - level.framenum;
    int deadline = ((protection_frames + h_div - 1) / h_div) * h_div + 30 * h_rate;
    int begin = h_frames;
    int mark = h_nsounds, sound = imp_soundindex("items/protect2.wav");
    h_t23_use_powerup(p, 1);
    h_frames_n(deadline - 3 * h_rate);
    CHECK(h_count_sounds(sound, mark) == 0, "invulnerability is quiet before three seconds remain");
    h_frame();
    h_frames_n(3 * h_rate);
    int at = -1;
    for (int i = mark; i < h_nsounds; i++)
        if (h_sounds_log[i].soundindex == sound)
            at = h_sounds_log[i].frame;
    CHECK(h_count_sounds(sound, mark) == 1 && at == begin + deadline - 3 * h_rate,
          "invulnerability used during spawn protection warns once at the three-second grid frame (got %d)", at);
}
#endif
