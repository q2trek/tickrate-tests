static void sc_t24_grenade_windows(void)
{
    edict_t *p = h_armed("grenades", "grenades", 64);
    h_frames_n(h_grid_wait());
    VectorClear(p->client->v_angle);

    for (int windows = 32; windows >= 0; windows--) {
        int frame = level.framenum, mark = h_nmsgs;
        p->client->grenade_framenum = frame + windows * h_div;
        p->client->weaponstate = WEAPON_FIRING;
#ifdef HARNESS_OPENTDM
        p->client->ps.gunframe = 12;
        int expected_speed = 400 + (3.0f - windows * 0.1f) * (400 / 3.0f);
#else
        p->client->weaponframe = 12;
        int expected_speed = 400 + (30 - windows) * 13;
#endif
        Think_Weapon(p);
        edict_t *grenade = h_grenade();
        if (!windows) {
            CHECK(!grenade && h_explosions(mark) == 1, "zero remaining windows explode immediately");
            continue;
        }
        CHECK(grenade != NULL, "hand grenade with %d remaining windows is thrown", windows);
        if (!grenade) continue;
        CHECK(grenade->nextthink == frame + windows * h_div,
              "hand grenade keeps its exact remaining fuse windows (%d left, got %d world frames, want %d)",
              windows, grenade->nextthink - frame, windows * h_div);
        CHECK(grenade->velocity[0] == expected_speed,
              "hand grenade uses its classic launch speed (%d windows left, got %.0f, want %d)",
              windows, grenade->velocity[0], expected_speed);
        G_FreeEdict(grenade);
    }
}

static int h_short_action_frame;
static edict_t *h_short_respawn;

static void h_short_action_mark(edict_t *self, edict_t *other, edict_t *activator)
{
    h_short_action_frame = h_frames;
}

static void h_short_action_respawn(edict_t *self, edict_t *other, edict_t *activator)
{
    SetRespawn(h_short_respawn, 0.05f);
}

static edict_t *h_short_action_target(const char *name)
{
    return G_Find(NULL, FOFS(targetname), (char *)name);
}

enum { H_SHORT_EXPLOSION, H_SHORT_RELAY, H_SHORT_TIMER, H_SHORT_REPEAT, H_SHORT_ITEM, H_SHORT_COUNT };

static const char *const h_short_classes[] = {
    "target_explosion", "trigger_relay", "func_timer", "trigger_multiple", "ammo_shells"
};

static void h_add_short_actions(int side)
{
    for (int kind = 0; kind < H_SHORT_COUNT; kind++)
        h_ent("{\n\"classname\" \"%s\"\n\"targetname\" \"short%d_%d\"\n\"target\" \"short_result\"\n%s}\n",
              h_short_classes[kind], side, kind,
              kind == H_SHORT_REPEAT ? "\"model\" \"*1\"\n\"wait\" \"0.05\"\n" :
              kind == H_SHORT_ITEM ? "\"origin\" \"0 2000 64\"\n" : "\"delay\" \"0.05\"\n");
}

static void sc_t24_short_actions(void)
{
    h_scene_base();
    h_add_inline(-32, 1936, 0, 32, 2064, 64);
    h_ent("{\n\"classname\" \"info_notnull\"\n\"targetname\" \"short_gap\"\n}\n");
    h_add_short_actions(0);
    h_ent("{\n\"classname\" \"func_timer\"\n\"targetname\" \"short_source\"\n\"wait\" \"60\"\n}\n");
    h_add_short_actions(1);
    h_start(h_ents);
    edict_t *p = h_join(0, "p1"), *source = h_short_action_target("short_source");
    edict_t *gap = h_short_action_target("short_gap");
    edict_t *sink = G_Spawn();
    sink->targetname = "short_result";
    if (!source || !gap) imp_error("short action fixtures missing");
    h_frames_n(h_div * 2);

    for (int side = 1; side >= 0; side--) {
        for (int kind = 0; kind < H_SHORT_COUNT; kind++) {
            char target[32];
            snprintf(target, sizeof(target), "short%d_%d", side, kind);
            edict_t *action = h_short_action_target(target);
            if (!action || ((NUM(action) > NUM(source)) != side))
                imp_error("short action fixture has wrong slot order");
            if (kind == H_SHORT_TIMER) {
                action->target = NULL;
                action->wait = 60.0f;
            }
            sink->use = kind == H_SHORT_ITEM ? h_short_action_respawn : h_short_action_mark;
            h_short_respawn = action;
            h_short_action_frame = -1;
            h_frames_n(h_grid_wait());
            int start = h_frames, observed = -1;
            if (!side && kind == H_SHORT_RELAY) G_FreeEdict(gap);
            source->target = kind == H_SHORT_ITEM ? sink->targetname : action->targetname;
            source->activator = p;
            source->nextthink = level.framenum;
            for (int i = 0; i <= h_div; i++) {
                int frame = h_frames;
                h_frame();
                if (kind == H_SHORT_RELAY && !side && !i)
                    CHECK(gap->inuse && !strcmp(gap->classname, "DelayedUse"), "relay uses the earlier slot");
                if (kind == H_SHORT_REPEAT && action->nextthink == 0) {
                    observed = frame;
                    break;
                }
                if (kind == H_SHORT_ITEM && action->solid == SOLID_TRIGGER) {
                    observed = frame;
                    break;
                }
                if (kind == H_SHORT_TIMER && action->nextthink > level.framenum + h_rate) {
                    observed = frame;
                    break;
                }
                if (kind < H_SHORT_TIMER && h_short_action_frame >= 0) {
                    observed = h_short_action_frame;
                    break;
                }
            }
            CHECK(observed == start + (side ? 0 : h_div),
                  "short delayed action waits for the grid (%s %s source, got %+d frames, want %d)",
                  h_short_classes[kind], side ? "after" : "before", observed - start, side ? 0 : h_div);
            source->nextthink = 0;
            action->nextthink = 0;
        }
    }
}

#ifdef HARNESS_OPENTDM
static void h_place_blocked_rider(edict_t *rider, edict_t *plat)
{
    plat->nextthink = 0;
    plat->s.origin[2] = -32;
    VectorSet(plat->velocity, 0, 0, 180);
    plat->moveinfo.state = H_STATE_UP;
    gi.linkentity(plat);
    VectorSet(rider->s.origin, 0, 2000, 120.125f);
    gi.linkentity(rider);
    rider->groundentity = plat;
    rider->groundentity_linkcount = plat->linkcount;
}

static void sc_t24_timeout_blocked_rider(void)
{
    h_scene_base();
    h_add_world(-64, 1936, 153, 64, 2064, 256);
    int model = h_add_inline(-64, 1936, 64, 64, 2064, 128);
    h_ent("{\n\"classname\" \"func_plat\"\n\"model\" \"*%d\"\n\"height\" \"64\"\n\"dmg\" \"7\"\n}\n", model);
    h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"300 0 24\"\n}\n");
    h_start(h_ents);
    edict_t *a = h_join(0, "p1"), *b = h_join(1, "p2"), *plat = h_by_model("*1");
    h_frames_n(3 * h_rate);
    if (!h_tdm_start(a, b)) return;

    h_place_blocked_rider(b, plat);
    int health = b->health;
    meansOfDeath = MOD_UNKNOWN;
    h_frame();
    CHECK(plat->s.origin[2] == -32 && b->s.origin[2] == 120.125f, "blocked pusher rolls back the rider and platform");
    CHECK(health - b->health == 7 && (meansOfDeath & ~MOD_FRIENDLY_FIRE) == MOD_CRUSH &&
          plat->moveinfo.state == H_STATE_DOWN, "blocked platform in play deals crush damage and reverses");
    VectorClear(plat->velocity);
    plat->nextthink = 0;
    h_frames_n(h_div + 1);

    h_frames_n((h_grid_wait() + 1) % h_div);
    h_place_blocked_rider(b, plat);
    health = b->health;
    meansOfDeath = MOD_UNKNOWN;
    h_client_command(a, "time");
    CHECK(tdm_match_status == MM_TIMEOUT && !level.timeout_frozen, "timeout starts before the freeze frame");
    h_frame();
    CHECK(plat->s.origin[2] == -32 && b->s.origin[2] == 120.125f, "blocked timeout push still rolls back positions");
    h_frames_n(h_div > 1 ? h_grid_wait() + 1 : 0);
    CHECK(level.timeout_frozen, "world freezes on the grid");
    CHECK(b->health == health && (meansOfDeath & ~MOD_FRIENDLY_FIRE) != MOD_CRUSH &&
          plat->moveinfo.state == H_STATE_UP,
          "blocked platform in timeout preserves rider health and direction (health %d to %d, state %d)",
          health, b->health, plat->moveinfo.state);
}
#endif
