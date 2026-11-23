/* Drive the same scenarios through each module's weapon, RNG and match interfaces. */
#define HU_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define hu_min(a, b) ((a) < (b) ? (a) : (b))
#define hu_max(a, b) ((a) > (b) ? (a) : (b))

#ifdef HARNESS_OPENTDM
#define HU_SCREEN ITEM_ITEM_POWER_SCREEN
#define HU_CELLS ITEM_AMMO_CELLS
#define hu_srand init_genrand
#define hu_rand genrand_int32
#define hu_begin_damage TDM_BeginDamage
#define hu_end_damage TDM_EndDamage
#define hu_begin_match TDM_BeginMatch

static int hu_ssg_attacks(const edict_t *ent) { return ent->client->resp.teamplayerinfo->shots_fired[TDMG_SSHOTGUN]; }
static int hu_ssg_hits(const edict_t *ent) { return ent->client->resp.teamplayerinfo->shots_hit[TDMG_SSHOTGUN]; }

static void hu_spectate(edict_t *ent)
{
    TDM_LeftTeam(ent, false);
    respawn(ent);
}

static void hu_reset_match(void)
{
    imp_cvar_set("g_auto_rejoin_match", "1");
    TDM_ResetGameState();
}
#else
#define HU_SCREEN ITEM_POWER_SCREEN
#define HU_CELLS ITEM_CELLS
#define hu_srand Q_srand
#define hu_rand Q_rand
#define hu_begin_damage G_BeginDamage
#define hu_end_damage G_EndDamage
#define hu_begin_match() ((void)0)
#define hu_reset_match G_ResetLevel

static int hu_ssg_attacks(const edict_t *ent) { return ent->client->resp.frags[FRAG_SUPERSHOTGUN].atts; }
static int hu_ssg_hits(const edict_t *ent) { return ent->client->resp.frags[FRAG_SUPERSHOTGUN].hits; }

static void hu_spectate(edict_t *ent) { spectator_respawn(ent, CONN_SPECTATOR); }
#endif

static void hu_fire_pellets(edict_t *ent, vec3_t start, vec3_t dir, int kick, int spread, int count, unlag_shot_t *shot)
{
    fire_shotgun(ent, start, dir, 4, kick, spread, spread, count, MOD_SHOTGUN, shot);
}

/* Warmup gives OpenTDM body armor. The shared damage checks use bare targets. */
static void hu_clear_body_armor(edict_t *ent)
{
#ifdef HARNESS_OPENTDM
    ent->client->inventory[ITEM_ITEM_ARMOR_BODY] = 0;
#endif
}

static edict_t *hu_join(int slot, const char *name)
{
    edict_t *ent = h_join(slot, name);
    hu_clear_body_armor(ent);
    return ent;
}

static void hu_respawn(edict_t *ent)
{
    PutClientInServer(ent);
    hu_clear_body_armor(ent);
}

static void sc_unlag_fixture(void)
{
    h_unlag_fixture = 1;
    h_maxclients = 32;
    h_scene_base();
    h_add_world(64, 30, 1, 96, 90, 64);
    h_world_contents[h_nworld - 1] = CONTENTS_WATER;
    h_start(h_ents);
    CHECK(game.maxclients == h_maxclients, "game initialized with %d client slots", h_maxclients);

    edict_t *a = hu_join(0, "west");
    edict_t *b = hu_join(1, "east");
    h_place(a, -80, -48, 24);
    h_think(a, 0, 0);
    h_place(b, -80, 48, 24);
    h_think(b, 0, 0);
    vec3_t start = { 32, 60, 24 }, inside = { 80, 60, 24 };
    int contents = gi.pointcontents(inside);
    CHECK(contents == CONTENTS_WATER, "engine point contents is water (got %#x)", contents);
    trace_t tr = gi.trace(start, NULL, NULL, inside, NULL, MASK_WATER);
    CHECK(tr.fraction < 1 && tr.contents == CONTENTS_WATER && !tr.startsolid,
          "outside trace enters water (fraction %.4f contents %#x)", tr.fraction, tr.contents);

    h_nmulticasts = 0;
    gi.WriteByte(H_SVC_TEMP_ENTITY);
    gi.WriteByte(TE_GUNSHOT);
    gi.multicast(a->s.origin, MULTICAST_PVS);
    gi.WriteByte(H_SVC_TEMP_ENTITY);
    gi.WriteByte(TE_GUNSHOT);
    gi.multicast(b->s.origin, MULTICAST_PVS);
    CHECK(h_nmulticasts == 2, "both multicast events recorded (got %d)", h_nmulticasts);
    if (h_nmulticasts == 2) {
        const h_multicast_t *from_a = &h_multicasts[0], *from_b = &h_multicasts[1];
        CHECK(VectorCompare(from_a->origin, a->s.origin) && from_a->mode == MULTICAST_PVS &&
              from_a->svc == H_SVC_TEMP_ENTITY && from_a->sub == TE_GUNSHOT && from_a->recipients == 1,
              "multicast A reaches only slot 0 (mask %#x)", from_a->recipients);
        CHECK(VectorCompare(from_b->origin, b->s.origin) && from_b->mode == MULTICAST_PVS &&
              from_b->svc == H_SVC_TEMP_ENTITY && from_b->sub == TE_GUNSHOT && from_b->recipients == 2,
              "multicast B reaches only slot 1 (mask %#x)", from_b->recipients);
    }
    CHECK(h_cvar_find("g_unlag") && h_cvar_find("g_unlag_max_ms"), "history settings are registered");
    CHECK(unlag_api && unlag_game_ex && unlag_game_ex->clip,
          "OpenFFA receives shot-time extension and the engine clip import");
}

static void hu_position(edict_t *ent, float x, float y, float z)
{
    VectorSet(ent->s.origin, x, y, z);
    VectorSet(ent->client->ps.pmove.origin, x * 8, y * 8, z * 8);
    VectorClear(ent->velocity);
    gi.linkentity(ent);
}

static void hu_arm(edict_t *ent, const char *name)
{
    gclient_t *client = ent->client;
    client->weapon = FindItem((char *)name);
#ifdef HARNESS_OPENTDM
    client->ammo_index = client->weapon->ammoindex;
    client->next_weapon_think = 0;
#else
    client->ammo_index = ITEM_INDEX(FindItem(client->weapon->ammo));
#endif
    client->inventory[client->ammo_index] = 100;
    client->weaponstate = WEAPON_READY;
    client->weapon_thunk = client->attack_pending = false;
    client->buttons = client->latched_buttons = 0;
    client->pers.hand = CENTER_HANDED;
}

static edict_t *hu_join_shooter(void)
{
    edict_t *ent = hu_join(0, "shooter");
    hu_position(ent, -128, 0, 24);
    return ent;
}

static void hu_command_yaw(edict_t *ent, int buttons, unlag_kind_t kind, int frame, float yaw)
{
    h_unlag_command = (unlag_command_t){ kind, frame };
    h_unlag_yaw = ANGLE2SHORT(yaw);
    h_think(ent, buttons, 0);
}

static void hu_command(edict_t *ent, int buttons, unlag_kind_t kind, int frame, float y)
{
    hu_command_yaw(ent, buttons, kind, frame, atan2f(y, 512) * 180 / M_PI);
}

static int hu_rail(edict_t *shooter, edict_t *target, unlag_kind_t kind, int frame, float y)
{
    int health = target->health;
    hu_arm(shooter, "Railgun");
    hu_command(shooter, BUTTON_ATTACK, kind, frame, y);
    return health - target->health;
}

static void hu_log(int slot, int age, int selected, int hits, int ping)
{
    char expected[256];
    if (age < 0)
        snprintf(expected, sizeof(expected), "unlag slot=%d estimate_age= age= estimated=0 hits=0 ping=%d\n",
                 slot, ping);
    else
        snprintf(expected, sizeof(expected), "unlag slot=%d estimate_age=%d age=%d estimated=1 hits=%d ping=%d\n",
                 slot, age, selected, hits, ping);
    CHECK(!strcmp(expected, h_unlag_log), "expected %s  actual %s", expected, h_unlag_log);
}

/* x=512, y increases by 5 units on each 60 Hz world step. */
static void hu_strafe(edict_t *target, int frames, float y, float step)
{
    for (int i = 0; i < frames; i++) {
        hu_position(target, 512, y + step * i, 24);
        h_frame();
    }
    hu_position(target, 512, y + step * frames, 24);
}

static void hu_scene(int slots)
{
    h_unlag_fixture = 1;
    h_maxclients = slots;
    h_scene_base();
#ifdef HARNESS_OPENTDM
    imp_cvar("g_max_players_per_team", "0", 0);
    for (int i = 0; i < slots; i++)
        h_ent("{\n\"classname\" \"info_player_deathmatch\"\n\"origin\" \"-1024 %d 24\"\n}\n", 96 * i);
#endif
}

static void sc_unlag_rail(void)
{
    hu_scene(4);
    h_start(h_ents);
    cvar_t *unlag = h_cvar_find("g_unlag");
    CHECK(unlag && unlag->value == 0, "g_unlag defaults to zero");
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "strafer");
    hu_position(shooter, 0, 0, 24);
    target->health = 2000;
    shooter->client->ping = 77;
    int age = h_div;
    int chosen = h_frames;
    hu_strafe(target, age, 0, 30.0f / age);

    int dmg = hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0);
    CHECK(!dmg && !h_unlag_clips, "default-off shot misses 30 units behind a moving target");
    imp_cvar_set("g_unlag", "2");
    dmg = hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0);
    CHECK(dmg == 100, "rail hits the moving target's earlier position (damage %d)", dmg);
    CHECK(target->s.origin[1] == 30 && target->velocity[0] > 0 &&
          fabsf(target->client->damage_from[0] - (496 - 0.03125f)) < 0.001f &&
          fabsf(target->client->damage_from[1] - 30) < 0.001f,
          "damage and knockback at current origin, with the original side epsilon");
    hu_log(0, age, age, 1, 77);

    dmg = hu_rail(shooter, target, UNLAG_REPEAT, 0, 0);
    CHECK(dmg == 100, "already executed lastcmd repeat preserves the earlier estimate");
    dmg = hu_rail(shooter, target, UNLAG_NO_ESTIMATE, 0, 0);
    CHECK(!dmg, "new command without an estimate clears the earlier estimate");
    hu_log(0, -1, 0, 0, 77);
    dmg = hu_rail(shooter, target, UNLAG_NO_ESTIMATE, 0, 30);
    CHECK(dmg == 100, "without an estimate, the current target can still be hit");

    dmg = hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0);
    CHECK(dmg == 100, "fresh estimate restores unlag");
    dmg = hu_rail(shooter, target, UNLAG_OUTSIDE, 0, 0);
    CHECK(!dmg, "a non-network command clears the estimate");
    dmg = hu_rail(shooter, target, UNLAG_ESTIMATE, h_frames, 30);
    CHECK(dmg == 100, "second packet in the same world frame replaces the first estimate");
}

static void sc_unlag_no_extension(void)
{
    h_scene_base();
    h_start(h_ents);
    CHECK(!unlag_api && !unlag_game_ex, "module initialized without the extended game API");
    CHECK(h_cvar_find("g_unlag") && h_cvar_find("g_unlag_max_ms"), "unlag settings exist without the extension");
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    hu_position(shooter, 0, 0, 24);
    hu_position(target, 512, 0, 24);
    target->health = 1000;
    int chosen = h_frames;
    h_frame();
    hu_position(target, 512, 64, 24);
    imp_cvar_set("g_unlag", "1");
    CHECK(!hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0) && !G_UnlagBeginShot(shooter).estimated,
          "without shot-time extension, the earlier box is not used");
    CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 64) == 100 && !h_unlag_clips,
          "without shot-time extension, the current box takes normal rail damage");
}

static void sc_unlag_limits(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "strafer");
    hu_position(shooter, 0, 0, 24);
    target->health = 3000;
    imp_cvar_set("g_unlag", "2");
    int start = h_frames, frames = 4 * h_div;
    int step = 30 / h_div;
    hu_strafe(target, frames, 0, step);
    const int limits[] = { 200, 50, 0, -1, 250, 1000 };
    for (int i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        int age = h_rate * hu_min(250, hu_max(0, limits[i])) / 1000;
        imp_cvar_set("g_unlag_max_ms", va("%d", limits[i]));
        int dmg = hu_rail(shooter, target, UNLAG_ESTIMATE, start, 0);
        CHECK(!dmg, "configured %d ms: a ray at the original estimate misses", limits[i]);
        hu_log(0, frames, age, 0, 0);
        dmg = hu_rail(shooter, target, UNLAG_ESTIMATE, start, step * (frames - age));
        CHECK(dmg == 100, "configured %d ms: ray at the effective age %d hits", limits[i], age);
        hu_log(0, frames, age, age ? 1 : 0, 0);
    }
}

static void hu_delayed(edict_t *shooter, edict_t *target, int phase, bool old_ray)
{
    hu_position(shooter, 0, 0, 24);
    hu_position(target, 512, -30, 24);
    target->health = 1000;
    h_frames_n(2 * h_div);
    while (level.framenum % h_div != phase)
        h_frame();
    hu_arm(shooter, "Railgun");
    shooter->client->weapon_thunk = true;
    int press = h_frames;
    int age = 2 * h_div;
    int due = press + (phase ? 2 * h_div - phase : h_div);
#ifdef HARNESS_OPENTDM
    shooter->client->next_weapon_think = level.framenum + due - press;
#endif
    int latest = due - h_div;
    int before = h_unlag_lines;
    hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, press - age, -30);
    CHECK(shooter->client->attack_pending && h_unlag_lines == before, "phase %d press waits for the weapon step", phase);
    for (; h_frames <= due; h_frame()) {
        int y = h_frames < latest ? -30 : (h_frames - latest) * (30 / h_div);
        hu_position(target, 512, y, 24);
        if (h_frames == due)
            hu_command(shooter, 0, UNLAG_ESTIMATE, latest, old_ray ? -30 : 0);
    }
    CHECK(h_unlag_lines == before + 1, "phase %d fired once in RunFrame after %d ticks", phase, due - press);
    CHECK(target->health == (old_ray ? 1000 : 900), "phase %d delayed rail %s (health %d)",
          phase, old_ray ? "misses the press ray" : "hits the latest ray", target->health);
    hu_log(0, due - latest, hu_min(due - latest, age), !old_ray, 0);
}

static void sc_unlag_delayed(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    imp_cvar_set("g_unlag", "2");
    for (int phase = 0; phase < h_div; phase++) {
        hu_delayed(shooter, target, phase, false);
        hu_delayed(shooter, target, phase, true);
    }
}

static void sc_unlag_rail_row(void)
{
    hu_scene(25);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *targets[24];
    for (int i = 0; i < 24; i++) {
        targets[i] = hu_join(i + 1, va("target%d", i));
        hu_position(targets[i], 128 + 56 * i, i < 2 ? 0 : 60, 24);
        targets[i]->health = 500;
    }
    hu_position(shooter, 0, 0, 24);
    imp_cvar_set("g_unlag", "2");
    int chosen = h_frames;
    h_frame();
    for (int i = 0; i < 24; i++)
        hu_position(targets[i], targets[i]->s.origin[0], 60, 24);
    hu_rail(shooter, targets[0], UNLAG_ESTIMATE, chosen, 0);
    CHECK(targets[0]->health == 400 && targets[1]->health == 400 && targets[2]->health == 500,
          "rail through two historical player boxes");
    hu_log(0, 1, 1, 2, 0);

    for (int i = 0; i < 24; i++) {
        hu_position(targets[i], targets[i]->s.origin[0], 0, 24);
        targets[i]->health = 500;
    }
    chosen = h_frames;
    h_frame();
    for (int i = 0; i < 24; i++)
        hu_position(targets[i], targets[i]->s.origin[0], 60, 24);
    hu_rail(shooter, targets[0], UNLAG_ESTIMATE, chosen, 0);
    int hits = 0;
    for (int i = 0; i < 24; i++)
        hits += targets[i]->health == 400;
    CHECK(hits == 24, "rail traverses the row of 24 historical player boxes (hit %d)", hits);
    hu_log(0, 1, 1, 24, 0);
}

static void hu_rail_overlap(bool both_historical)
{
    hu_scene(4);
    h_start(h_ents);
    hu_srand(1);
    edict_t *shooter = hu_join_shooter();
    edict_t *a = hu_join(1, "first");
    hu_position(a, 128, -64, 24);
    edict_t *b = hu_join(2, "second");
    hu_position(b, 128, 64, 24);
#ifdef HARNESS_OPENTDM
    TDM_LeftTeam(b, false);
    b->client->pers.team = TEAM_B;
    JoinedTeam(b, false, false);
    level.match_start_framenum = level.framenum;
#endif
    edict_t *rear = hu_join(3, "rear");
    hu_begin_match();
    hu_position(shooter, 0, 0, 24);
    hu_position(a, 128, -8, 24);
    hu_position(b, 128, both_historical ? 8 : 64, 24);
    hu_position(rear, 256, 0, 24);
    int chosen = h_frames;
    h_frame();
    hu_position(a, 128, 64, 24);
    if (!both_historical) {
        hu_position(shooter, -128, 0, 24);
        hu_respawn(b);
        hu_position(shooter, 0, 0, 24);
    }
    hu_position(b, 128, both_historical ? 96 : 8, 24);
    a->health = b->health = rear->health = 10000;
#ifdef HARNESS_OPENTDM
    CHECK(TEAM(a) != TEAM(shooter) && TEAM(b) == TEAM(a) && TEAM(rear) == TEAM(a),
          "all three targets count towards match accuracy");
    teamplayer_t *stats = shooter->client->resp.teamplayerinfo;
    int hits = stats->shots_hit[TDMG_RAILGUN];
#endif
    imp_cvar_set("g_unlag", "1");
    CHECK(G_UnlagHistory(a, chosen) && !!G_UnlagHistory(b, chosen) == both_historical,
          "the front pair has the intended historical and current boxes");
    h_unlag_trace_budget = 12;
    hu_rail(shooter, a, UNLAG_ESTIMATE, chosen, 0);
    h_unlag_trace_budget = 0;
    CHECK(a->health == 9900 && b->health == 9900 && rear->health == 9900,
          "rail hits each overlapping player once and reaches the rear player (damage %d, %d, %d)",
          10000 - a->health, 10000 - b->health, 10000 - rear->health);
#ifdef HARNESS_OPENTDM
    CHECK(stats->shots_hit[TDMG_RAILGUN] == hits + 3,
          "rail accuracy counts each traversed player once");
#endif
}

static void sc_unlag_rail_mixed_overlap(void) { hu_rail_overlap(false); }
static void sc_unlag_rail_history_overlap(void) { hu_rail_overlap(true); }

static uint32_t hu_recipients(int type, int from)
{
    uint32_t recipients = 0;
    for (int i = from; i < h_nmulticasts; i++)
        if (h_multicasts[i].svc == H_SVC_TEMP_ENTITY && h_multicasts[i].sub == type)
            recipients |= h_multicasts[i].recipients;
    return recipients;
}

static void sc_unlag_water(void)
{
    hu_scene(4);
    h_add_world(256, -512, 1, 512, -1, 128);
    h_world_contents[h_nworld - 1] = CONTENTS_WATER;
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    hu_position(shooter, 0, -48, 24);
    hu_position(target, 128, -48, 24);
    target->health = 1000;
    int chosen = h_frames;
    h_frame();

    int before = h_nmulticasts;
    hu_srand(123);
    hu_arm(shooter, "Machinegun");
    hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
    int expected_health = target->health;
    uint32_t expected_rand = hu_rand();
    CHECK(expected_health < 1000 && !hu_recipients(TE_SPLASH, before), "control hits player before water");

    hu_position(target, 128, 48, 24);
    target->health = 1000;
    int linkcount = target->linkcount, solid = target->solid;
    imp_cvar_set("g_unlag", "1");
    before = h_nmulticasts;
    hu_srand(123);
    hu_arm(shooter, "Machinegun");
    hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
    CHECK(target->health == expected_health && hu_rand() == expected_rand, "historical target in front of water preserves damage and RNG");
    CHECK(!hu_recipients(TE_SPLASH, before) && hu_recipients(TE_BLOOD, before) == 2,
          "blood is multicast at the current box and the bullet never enters water");
    CHECK(target->s.origin[1] == 48 && target->solid == solid && target->linkcount == linkcount,
          "historical hit did not move, relink or hide the target");

    before = h_nmulticasts;
    hu_srand(123);
    hu_arm(shooter, "Machinegun");
    hu_command(shooter, BUTTON_ATTACK, UNLAG_NO_ESTIMATE, 0, 0);
    CHECK(hu_recipients(TE_SPLASH, before) == 1 && hu_recipients(TE_BUBBLETRAIL, before) == 1,
          "clear bullet path enters the water and emits the expected effects");

    hu_position(shooter, 272, -48, 24);
    hu_position(target, 400, -48, 24);
    chosen = h_frames;
    h_frame();
    hu_position(target, 400, 48, 24);
    target->health = 1000;
    before = h_nmulticasts;
    hu_srand(123);
    hu_arm(shooter, "Machinegun");
    hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
    CHECK(target->health < 1000 && !hu_recipients(TE_SPLASH, before) && hu_recipients(TE_BUBBLETRAIL, before) == 1,
          "historical target inside water is hit without an entry splash");

    edict_t *other = hu_join(2, "other");
    hu_position(other, 400, 96, 24);
    hu_position(shooter, 272, -120, 24);
    hu_position(target, 400, -48, 24);
    chosen = h_frames;
    h_frame();
    hu_position(target, 400, 48, 24);
    /* Put a second client after target in its area so reordering is observable. */
    gi.linkentity(other);
    hu_arm(shooter, "Shotgun");
    hu_command(shooter, 0, UNLAG_ESTIMATE, chosen, 0);

    edict_t *players[] = { shooter, target, other };
    int links[3], solids[3], health[3];
    for (int i = 0; i < HU_COUNT(players); i++) {
        links[i] = players[i]->linkcount;
        solids[i] = players[i]->solid;
        health[i] = players[i]->health;
    }
    vec3_t lo = { 0, -256, -64 }, hi = { 512, 256, 128 };
    edict_t *order[MAX_EDICTS], *after[MAX_EDICTS];
    int n = gi.BoxEdicts(lo, hi, order, HU_COUNT(order), AREA_SOLID);
    int linked_players = 0;
    for (int i = 0; i < n; i++)
        linked_players += order[i]->client != NULL;
    CHECK(linked_players == HU_COUNT(players), "area query sees all players before the miss (got %d)", linked_players);
    unlag_shot_t shot = G_UnlagBeginShot(shooter);
    vec3_t start = { 272, -120, 36 }, forward = { 1, 0, 0 };
    hu_srand(123);
    hu_fire_pellets(shooter, start, forward, 8, 500, DEFAULT_DEATHMATCH_SHOTGUN_COUNT, &shot);
    CHECK(shot.rewind && !shot.hits, "spread in water misses every historical box");
    for (int i = 0; i < HU_COUNT(players); i++) {
        CHECK(players[i]->health == health[i] && players[i]->linkcount == links[i] && players[i]->solid == solids[i],
              "miss preserves slot %d health, linkcount and solid", i);
    }
    CHECK(gi.BoxEdicts(lo, hi, after, HU_COUNT(after), AREA_SOLID) == n &&
          !memcmp(order, after, n * sizeof(*order)), "miss preserves the player order in the engine area list");
}

static void sc_unlag_shotgun_kill(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *front = hu_join(1, "front");
    hu_position(front, 96, 0, 24);
    edict_t *rear = hu_join(2, "rear");
    hu_position(rear, 192, 0, 24);
    hu_position(shooter, 0, 0, 24);
    int chosen = h_frames;
    h_frame();
    hu_position(front, 96, 80, 24);
    front->health = 1;
    rear->health = 1000;
    hu_srand(123);
    imp_cvar_set("g_unlag", "1");
    hu_command(shooter, 0, UNLAG_ESTIMATE, chosen, 0);
    unlag_shot_t shot = G_UnlagBeginShot(shooter);
    vec3_t start = { 0, 0, 36 }, forward = { 1, 0, 0 };
    hu_begin_damage();
    hu_fire_pellets(shooter, start, forward, 0, 0, 2, &shot);
    hu_end_damage();
    CHECK(front->deadflag && front->health == -3 && front->s.origin[1] == 80 && shot.hits == 1,
          "first pellet killed the front player through its historical box exactly once");
    CHECK(rear->health == 996, "second pellet follows the same ray through the old box and does 4 damage to the rear player");
}

static void sc_unlag_stationary_kill(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *front = hu_join(1, "front");
    hu_position(front, 48, 0, 24);
    edict_t *rear = hu_join(3, "rear");
    hu_begin_match();
    hu_position(rear, 80, 0, 24);
    for (int gib = 0; gib < 2; gib++) {
        uint32_t random_after = 0;
        for (int mode = 0; mode <= 2; mode++) {
            hu_position(shooter, -128, 0, 24);
            hu_respawn(front);
            hu_position(front, 48, 0, gib ? 56 : 24);
            hu_position(rear, 80, 0, 24);
            hu_position(shooter, 0, 0, 24);
            front->health = 1;
            rear->health = 1000;
            int chosen = h_frames;
            h_frame();
            hu_arm(shooter, "Super Shotgun");
            imp_cvar_set("g_unlag", va("%d", mode));
            hu_srand(123);
            int clips = h_unlag_clips;
            int atts = hu_ssg_attacks(shooter);
            int hits = hu_ssg_hits(shooter);
            hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
            CHECK(front->deadflag && front->health == (gib ? -41 : -5) &&
                  (front->takedamage == DAMAGE_NO) == gib &&
                  rear->health == 1000 - 6 * (DEFAULT_SSHOTGUN_COUNT - (gib ? 7 : 1)),
                  "%s mode %d: both super shotgun fans deal the expected damage (health %d/%d)",
                  gib ? "gib" : "death", mode, front->health, rear->health);
            CHECK(hu_ssg_attacks(shooter) == atts + 1 &&
                  hu_ssg_hits(shooter) == hits + 1,
                  "super shotgun accuracy counts one attack and one hit in mode %d", mode);
            CHECK(h_unlag_clips == clips, "stationary players require no historical clips in mode %d", mode);
            if (mode == 2)
                hu_log(0, 1, 1, 0, 0);
            uint32_t rng = hu_rand();
            if (!mode)
                random_after = rng;
            CHECK(rng == random_after, "stationary super shotgun preserves RNG in mode %d", mode);
        }
    }
}

/* A missing history has the same result as an empty slot in the red build. */
static void sc_unlag_history(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *target = hu_join(1, "target");
    int record = h_frames;
    hu_strafe(target, 16, 0, 5);
    const unlag_pose_t *pose = G_UnlagHistory(target, record + 1);
    CHECK(pose && pose->frame == record + 1 && pose->origin[1] == 5 &&
          VectorCompare(pose->mins, target->mins) && VectorCompare(pose->maxs, target->maxs),
          "cold start at %d Hz retains an exact pose 15 frames back", h_rate);
    uint64_t life = pose ? pose->life : 0;

    record = h_frames;
    hu_position(target, 512, 25, 24);
    ge->RunFrame();
    hu_position(target, 512, 45, 24);
    ge->RunFrame();
    pose = G_UnlagHistory(target, record);
    CHECK(pose && pose->origin[1] == 45 && !G_UnlagHistory(target, record + 1),
          "two RunFrame calls at one world frame leave the most recent pose only");

    hu_reset_match();
    CHECK(!G_UnlagHistory(target, record), "reset cleared all prior history including the inner RunFrame writes");
    hu_strafe(target, 16, 0, 5);
    pose = G_UnlagHistory(target, record + 1);
    CHECK(pose && pose->origin[1] == 5 && pose->life > life, "reset starts a new life and recovers 15 frames of history");
    life = pose ? pose->life : 0;
    record = h_frames;

#ifdef HARNESS_OPENTDM
    tdm_match_status = MM_COUNTDOWN;
    level.match_start_framenum = level.framenum;
    h_frame();
    pose = G_UnlagHistory(target, record);
    CHECK(tdm_match_status == MM_PLAYING && !G_UnlagHistory(target, record - 1) && pose && pose->life > life,
          "countdown reset preserves only the outer frame's new-life record");
    hu_reset_match();
#else
    level.vote.proposal = VOTE_MAP;
    level.vote.initiator = target->client;
    level.vote.index++;
    target->client->level.vote.index = level.vote.index;
    target->client->level.vote.accepted = true;
    Q_strlcpy(level.vote.map, level.mapname, sizeof(level.vote.map));
    CHECK(G_CheckVote() && level.intermission_framenum, "same-map vote entered intermission");
    G_ExitLevel();
    CHECK(!G_UnlagHistory(target, record - 1), "same-map vote reset discarded history");
    hu_strafe(target, 16, 0, 5);
    record = h_frames;

    level.match_state = MS_COUNTDOWN;
    level.countdown_framenum = level.framenum - (int)g_countdown_time->value * h_rate;
    h_frame();
    pose = G_UnlagHistory(target, record);
    CHECK(level.match_state == MS_PLAYING && !G_UnlagHistory(target, record - 1) && pose && pose->life > life,
          "countdown reset preserves only the outer frame's new-life record");
#endif
    life = pose ? pose->life : 0;

    ge->ClientDisconnect(target);
    CHECK(!G_UnlagHistory(target, record), "disconnect invalidates history");
    target = hu_join(1, "new client");
    CHECK(!G_UnlagHistory(target, record), "same slot cannot read another player's history");
    record = h_frames;
    hu_strafe(target, 16, 0, 5);
    pose = G_UnlagHistory(target, record + 1);
    CHECK(pose && pose->origin[1] == 5 && pose->life > life, "reused slot recovers 15 frames under a unique life number");

    he_set_game(ge);
    ge->SpawnEntities("next", h_ents, "");
    CHECK(!G_UnlagHistory(target, record + 1), "map change cleared prior poses");
    imp_cvar_set("g_unlag", "1");
    target = hu_join(1, "reinit");
    hu_command(target, 0, UNLAG_ESTIMATE, h_frames - 2, 0);
    CHECK(G_UnlagBeginShot(target).estimated, "module holds a command estimate before reinit");
    ge->Shutdown();
#ifdef HARNESS_OPENTDM
    /* The linked fixture retains the old entity string across Shutdown. */
    level.entity_string = NULL;
#endif
    h_unlag_initialized = false;
    h_frames = 0;
    h_start(h_ents);
    CHECK(!G_UnlagHistory(EDICT(2), record + 1) && !G_UnlagBeginShot(EDICT(2)).estimated,
          "reinitializing the module clears history and command estimates");
}

static trace_t hu_trace(int frame, edict_t *shooter, vec3_t start, vec3_t end, int mask)
{
    unlag_shot_t shot = { .rewind = true, .frame = frame };
    vec3_t damage_point;
    return G_UnlagTrace(&shot, shooter, start, end, shooter, mask, damage_point);
}

static bool hu_trace_equal(trace_t a, trace_t b)
{
    return a.fraction == b.fraction && VectorCompare(a.endpos, b.endpos) &&
           VectorCompare(a.plane.normal, b.plane.normal) && a.plane.dist == b.plane.dist &&
           a.plane.type == b.plane.type && a.plane.signbits == b.plane.signbits &&
           a.surface == b.surface && a.contents == b.contents &&
           a.startsolid == b.startsolid && a.allsolid == b.allsolid && a.ent == b.ent;
}

static void sc_unlag_trace(void)
{
    hu_scene(4);
    h_add_world(256, -256, 1, 320, -1, 140);
    h_world_contents[h_nworld - 1] = CONTENTS_WATER;
    h_add_world(600, -256, 0, 700, 256, 200);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *a = hu_join(1, "a");
    hu_position(a, 128, -64, 80);
    edict_t *b = hu_join(2, "b");
    hu_position(b, 128, 400, 80);

    struct {
        const char *name;
        vec3_t start, end;
        int mask;
    } rays[] = {
        { "side", {32, -64, 80}, {180, -64, 80}, MASK_SHOT },
        { "from above", {128, -64, 180}, {128, -64, 70}, MASK_SHOT },
        { "from below", {128, -64, 5}, {128, -64, 80}, MASK_SHOT },
        { "along face", {32, -48, 80}, {180, -48, 80}, MASK_SHOT },
        { "start inside", {128, -64, 80}, {180, -64, 80}, MASK_SHOT },
        { "towards wall", {32, -64, 80}, {660, -64, 80}, MASK_SHOT },
        { "before water", {32, -64, 80}, {660, -64, 80}, MASK_SHOT | MASK_WATER },
        { "corner tangent", {32, -88, 80}, {192, -8, 80}, MASK_SHOT },
        { "historical miss", {32, 64, 80}, {180, 64, 80}, MASK_SHOT },
        { "oblique miss", {32, -64, 80}, {180, -128, 80}, MASK_SHOT },
    };
    trace_t reference[sizeof(rays) / sizeof(rays[0])];
    for (int i = 0; i < sizeof(rays) / sizeof(rays[0]); i++)
        reference[i] = gi.trace(rays[i].start, NULL, NULL, rays[i].end, shooter, rays[i].mask);
    int chosen = h_frames;
    h_frame();
    hu_position(a, 128, 64, 80);
    trace_t control = gi.trace(rays[0].start, NULL, NULL, rays[0].end, shooter, rays[0].mask);
    CHECK(reference[0].ent == a && control.fraction == 1, "ordinary trace hits at A and misses at B");
    CHECK(gi.trace(rays[8].start, NULL, NULL, rays[8].end, shooter, MASK_SHOT).ent == a,
          "current box blocks the historical miss ray before it is hidden");
    for (int i = 0; i < sizeof(rays) / sizeof(rays[0]); i++) {
        int clips = h_unlag_clips;
        trace_t tr = hu_trace(chosen, shooter, rays[i].start, rays[i].end, rays[i].mask);
        CHECK(hu_trace_equal(tr, reference[i]), "%s: all trace fields match (fraction %.6f vs %.6f)",
              rays[i].name, tr.fraction, reference[i].fraction);
        CHECK(h_unlag_clips - clips == (i < 8), "%s: only an intersecting historical box reaches clip",
              rays[i].name);
    }

    hu_position(a, 480, -64, 80);
    trace_t behind = gi.trace(rays[6].start, NULL, NULL, rays[6].end, shooter, rays[6].mask);
    chosen = h_frames;
    h_frame();
    int before = h_unlag_clips;
    CHECK(hu_trace_equal(hu_trace(chosen, shooter, rays[6].start, rays[6].end, rays[6].mask), behind) &&
          h_unlag_clips == before, "unmoved target stays in the normal trace");
    hu_position(a, 480, 64, 80);
    CHECK(behind.contents == CONTENTS_WATER &&
          hu_trace_equal(hu_trace(chosen, shooter, rays[6].start, rays[6].end, rays[6].mask), behind),
          "water still wins when it precedes the historical box");

    hu_position(a, 128, -64, 80);
    hu_position(b, 128, -64, 80);
    chosen = h_frames;
    h_frame();
    hu_position(a, 128, 64, 80);
    hu_position(b, 128, 96, 80);
    CHECK(hu_trace(chosen, shooter, rays[0].start, rays[0].end, MASK_SHOT).ent == a,
          "equal historical fractions choose the smaller slot");
    gi.linkentity(a);
    CHECK(hu_trace(chosen, shooter, rays[0].start, rays[0].end, MASK_SHOT).ent == a,
          "reversing link order does not change the tie");
    before = h_unlag_clips;
    trace_t inside = hu_trace(chosen, shooter, rays[4].start, (vec3_t){132, -64, 80}, MASK_SHOT);
    CHECK(inside.startsolid && inside.allsolid && inside.ent == g_edicts && h_unlag_clips == before + 1,
          "first allsolid target stops the scan");
    before = h_unlag_clips;
    trace_t wall = hu_trace(chosen, shooter, (vec3_t){599.984375f, 0, 80}, (vec3_t){660, 0, 80}, MASK_SHOT);
    CHECK(wall.fraction == 0 && wall.ent == g_edicts && h_unlag_clips == before,
          "zero fraction from the world bypasses historical clips");

    hu_position(a, 128, -64, 80);
    hu_position(b, 128, -64, 80);
    chosen = h_frames;
    h_frame();
    hu_position(a, 128, 64, 80);
    reference[4] = gi.trace(rays[4].start, NULL, NULL, rays[4].end, shooter, MASK_SHOT);
    CHECK(reference[4].startsolid &&
          hu_trace_equal(hu_trace(chosen, shooter, rays[4].start, rays[4].end, MASK_SHOT), reference[4]),
          "start inside a historical and a current box merges the solid flags");
    CHECK(a->solid == SOLID_BBOX && b->solid == SOLID_BBOX, "all temporary solid values were restored");
}

static void sc_unlag_lifetimes(void)
{
    hu_scene(4);
    h_ent("{\n\"classname\" \"misc_teleporter\"\n\"target\" \"exit\"\n\"origin\" \"0 400 24\"\n}\n");
    h_ent("{\n\"classname\" \"misc_teleporter_dest\"\n\"targetname\" \"exit\"\n\"origin\" \"0 0 24\"\n}\n");
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    imp_cvar_set("g_unlag", "2");
    target->health = 1000;
    hu_position(target, 512, 0, 34);
    int chosen = h_frames;
    h_frame();
    hu_position(target, 512, 64, 34);
    hu_position(shooter, 0, 400, 34);
    CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0) == 0 &&
          shooter->s.event == EV_PLAYER_TELEPORT && shooter->s.origin[1] == 0,
          "attack command teleported the shooter and missed the former target position");
    hu_log(0, -1, 0, 0, 0);

    hu_arm(shooter, "Railgun");
    shooter->client->attack_pending = true;
    int before = h_unlag_lines;
    for (int i = 0; i < 2 * h_div && h_unlag_lines == before; i++)
        h_frame();
    CHECK(h_unlag_lines == before + 1 && target->health == 1000, "next weapon step still has no estimate");
    hu_log(0, -1, 0, 0, 0);
    shooter->s.event = EV_PLAYER_TELEPORT;  /* same-world notification has not yet been cleared */
    CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0) == 100,
          "the next real command refreshes the estimate even while the event is set");

    int before_teleport = h_frames;
    hu_position(target, 512, 0, 34);
    h_frame();
    hu_position(shooter, -128, 0, 34);
    hu_position(target, 0, 400, 34);
    hu_command(target, 0, UNLAG_OUTSIDE, 0, 0);
    CHECK(target->s.event == EV_PLAYER_TELEPORT && !G_UnlagHistory(target, before_teleport),
          "target teleport discards the prior pose");
    hu_position(target, 512, 64, 34);
    hu_position(shooter, 0, 0, 34);
    CHECK(!hu_rail(shooter, target, UNLAG_ESTIMATE, before_teleport, 0) &&
          hu_rail(shooter, target, UNLAG_ESTIMATE, before_teleport, 64) == 100,
          "only the current target position can be hit after teleport");

    int old = h_frames;
    hu_position(target, 512, 0, 34);
    h_frame();
    vec3_t point = {512, 0, 34};
    T_Damage(target, shooter, shooter, vec3_origin, point, vec3_origin, 2000, 0, DAMAGE_NO_PROTECTION, MOD_RAILGUN);
    CHECK(target->deadflag && !G_UnlagHistory(target, old), "death excludes the corpse from historical targets");
    hu_position(shooter, -128, 0, 34);
    hu_respawn(target);
    hu_position(target, 512, 64, 34);
    hu_position(shooter, 0, 0, 34);
    target->health = 1000;
    CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, old, 0) == 0 &&
          hu_rail(shooter, target, UNLAG_ESTIMATE, old, 64) == 100,
          "new life cannot be hit at its previous position");
    int spectator_frame = h_frames;
    h_frame();
    hu_spectate(target);
    CHECK(!G_UnlagHistory(target, spectator_frame) && target->solid == SOLID_NOT &&
          hu_rail(shooter, target, UNLAG_ESTIMATE, spectator_frame, 64) == 0,
          "spectator on the line neither clips nor retains history");
    hu_respawn(shooter);
    hu_rail(shooter, target, UNLAG_REPEAT, 0, 0);
    hu_log(0, -1, 0, 0, 0);
}

static void hu_power_screen(edict_t *target, float yaw)
{
    target->flags |= FL_POWER_ARMOR;
    target->client->inventory[HU_SCREEN] = 1;
    target->client->inventory[HU_CELLS] = 100;
    target->s.angles[YAW] = yaw;
}

static void hu_bullet(edict_t *shooter, int chosen, float z)
{
    hu_command(shooter, 0, UNLAG_ESTIMATE, chosen, 0);
    unlag_shot_t shot = G_UnlagBeginShot(shooter);
    vec3_t start = {0, 0, z}, forward = {1, 0, 0};
    fire_bullet(shooter, start, forward, 30, 0, 0, 0, MOD_MACHINEGUN, &shot);
    G_UnlagEndShot(shooter, &shot);
}

static void sc_unlag_stance(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    hu_position(shooter, 0, 0, 64);
    hu_position(target, 512, 0, 64);
    target->health = 1000;
    int chosen = h_frames;
    h_frame();
    float threshold = 180 - acosf(0.3f) * 180 / M_PI;
    hu_power_screen(target, threshold - 0.001f);
    hu_bullet(shooter, chosen, 64);
    int old_cells = target->client->inventory[HU_CELLS];
    vec3_t old_point;
    VectorCopy(target->client->damage_from, old_point);
    imp_cvar_set("g_unlag", "2");
    hu_power_screen(target, threshold - 0.001f);
    int before = h_unlag_clips;
    hu_bullet(shooter, chosen, 64);
    CHECK(h_unlag_clips == before && old_cells == target->client->inventory[HU_CELLS] &&
          VectorCompare(old_point, target->client->damage_from), "stationary side impact preserves the threshold decision and epsilon");

    target->maxs[2] = 4;
    gi.linkentity(target);
    hu_power_screen(target, 180);
    hu_bullet(shooter, chosen, 88);
    CHECK(target->client->inventory[HU_CELLS] == 90 && target->client->damage_from[2] == 68 &&
          target->client->damage_from[0] == 496 - 0.03125f,
          "front upper-box impact clamps only its height when the target has ducked");
    hu_power_screen(target, 0);
    hu_bullet(shooter, chosen, 88);
    CHECK(target->client->inventory[HU_CELLS] == 100, "rear impact bypasses Power Screen after the stance change");
    hu_power_screen(target, threshold - 0.001f);
    hu_bullet(shooter, chosen, 64);
    CHECK(old_cells == target->client->inventory[HU_CELLS] &&
          VectorCompare(old_point, target->client->damage_from), "lower side impact retains the standing result after ducking");
}

static void sc_unlag_held(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    hu_position(shooter, 0, 0, 24);
    imp_cvar_set("g_unlag", "2");
    hu_arm(shooter, "Machinegun");
    int estimate = h_frames - 1;
    hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, estimate, 0);
    int seen = h_unlag_lines;
    for (int i = 0; i < 5 * h_div; i++) {
        int previous = h_unlag_lines;
        int world_frame = h_frames;
        h_frame();
        if (h_unlag_lines != previous) {
            int age = world_frame - estimate;
            hu_log(0, age, hu_min(age, h_rate / 5), 0, 0);
        }
    }
    CHECK(h_unlag_lines >= seen + 3, "held machinegun fired through the input pause and exceeded the cap");

    hu_command(shooter, 0, UNLAG_NO_ESTIMATE, 0, 0);
    hu_position(shooter, -128, 0, 24);
    edict_t *target = hu_join(1, "target");
    for (int old_ray = 0; old_ray < 2; old_ray++) {
        int press_pose = h_frames;
        hu_position(target, 128, -48, 24);
        h_frame();
        estimate = h_frames;
        hu_position(target, 128, 0, 24);
        hu_position(shooter, 0, 0, 24);
        target->health = 1000;
        hu_arm(shooter, "Chaingun");
        int before = h_unlag_lines;
        hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, press_pose, 0);
        CHECK(h_unlag_lines == before + 1 && target->health == 1000,
              "chaingun fires on press and misses the press pose");
        h_frame();
        hu_position(target, 128, 48, 24);
        hu_command(shooter, 0, UNLAG_ESTIMATE, estimate, old_ray ? -192 : 0);
        int groups = 0;
        for (int i = 0; i < 4 * h_div; i++) {
            int previous = h_unlag_lines;
            int world_frame = h_frames;
            int health = target->health;
            h_frame();
            if (h_unlag_lines == previous)
                continue;
            int age = world_frame - estimate;
            bool rewind_to_release = age <= h_rate / 5;
            bool hit = !old_ray && rewind_to_release;
            groups += rewind_to_release;
            CHECK(target->health == health - (hit ? 6 : 0),
                  "chaingun after release: %s ray, age %d, damage %d (expected %d)",
                  old_ray ? "press" : "release", age, health - target->health, hit ? 6 : 0);
            hu_log(0, age, hu_min(age, h_rate / 5), hit, 0);
        }
        CHECK(groups > 0, "chaingun fires after release while the release frame is inside the age limit");
    }
}

static void sc_unlag_blockers(void)
{
    hu_scene(4);
    int model = h_add_inline(240, -96, 1, 256, 0, 100);
    h_ent("{\n\"classname\" \"func_door\"\n\"model\" \"*%d\"\n\"angle\" \"0\"\n\"targetname\" \"cover\"\n}\n", model);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    hu_position(target, 512, -8, 24);
    target->health = 1000;
    int chosen = h_frames;
    h_frame();
    hu_position(target, 512, 60, 24);
    imp_cvar_set("g_unlag", "2");
    hu_position(shooter, 0, -16, 24);
    CHECK(!hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0),
          "closed door blocks the ray into the covered part of the historical target");
    hu_position(shooter, 0, 4, 24);
    CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0) == 100,
          "the uncovered part of the historical box is hittable");
    int clips = h_unlag_clips, lines = h_unlag_lines;
    hu_arm(shooter, "Rocket Launcher");
    hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
    CHECK(h_count_msgs(H_SVC_MUZZLEFLASH, MZ_ROCKET, 0) == 1 &&
          h_unlag_clips == clips && h_unlag_lines == lines, "rocket follows the ordinary projectile path");
    edict_t *rocket = G_Find(NULL, FOFS(classname), "rocket");
    if (rocket)
        G_FreeEdict(rocket);

    vec3_t start = { 32, 4, 36 }, forward = { 1, 0, 0 };
    int laser_damage[2];
    for (int mode = 0; mode <= 1; mode++) {
        hu_position(target, 144, 96, 24);
        target->health = 1000;
        meansOfDeath = MOD_UNKNOWN;
        imp_cvar_set("g_unlag", mode ? "1" : "0");
        fire_bfg(shooter, start, forward, 200, 400, 1000);
        edict_t *bfg = G_Find(NULL, FOFS(classname), "bfg blast");
        CHECK(bfg, "BFG projectile spawned in mode %d", mode);
        if (!bfg)
            return;
        for (int i = 0; i <= h_div && target->health == 1000; i++)
            h_frame();
        laser_damage[mode] = 1000 - target->health;
        CHECK(laser_damage[mode] == 5 && meansOfDeath == MOD_BFG_LASER,
              "mode %d BFG laser deals 5 MOD_BFG_LASER damage (damage %d, mod %d)",
              mode, laser_damage[mode], meansOfDeath);
        G_FreeEdict(bfg);
    }
    CHECK(laser_damage[0] == laser_damage[1] && h_unlag_clips == clips && h_unlag_lines == lines,
          "BFG laser damage agrees in both modes without historical traces");
}

static long long hu_elapsed_ns(struct timespec start, struct timespec end)
{
    return (long long)(end.tv_sec - start.tv_sec) * 1000000000 + end.tv_nsec - start.tv_nsec;
}

static void hu_bench(bool dense)
{
    const char *mode = getenv("HARNESS_UNLAG");
    if (h_rate != 60 || !mode || (strcmp(mode, "0") && strcmp(mode, "1")))
        imp_error("unlag_bench requires HARNESS_RATE=60 and HARNESS_UNLAG=0 or 1");

    enum { PLAYERS = 32, HISTORY = 17, AGE = 6, DISTANCE = 320, HALF_LANE = 32 };
    hu_scene(PLAYERS);
    h_start(h_ents);
    hu_srand(123);
    edict_t *players[PLAYERS];
    for (int i = 0; i < PLAYERS; i++) {
        players[i] = hu_join(i, va("shooter%d", i));
        hu_position(players[i], 0, 96 * (i + 1), 24);
    }
#ifdef HARNESS_OPENTDM
    /* Enter the same spawn-protection window as a completed countdown. */
    level.match_start_framenum = level.framenum;
#endif
    hu_begin_match();

    /* The sparse row moves along Y. Dense groups put two separate targets
     * across from each shooter, on opposite sides of both fans' overlap. */
    for (int offset = 0; offset <= HISTORY; offset++) {
        for (int i = 0; i < PLAYERS; i++) {
            if (dense) {
                float x = (i & 2 ? DISTANCE / 2 : -DISTANCE / 2) + offset;
                float y = 400 * (i / 4 + 1) + (i & 1 ? HALF_LANE : -HALF_LANE);
                hu_position(players[i], x, y, 24);
            } else {
                hu_position(players[i], 0, 96 * (i + 1) + 5 * offset, 24);
            }
        }
        if (offset < HISTORY)
            h_frame();
    }
    int chosen = h_frames - AGE;
    int records = 0, estimates = 0;
    float yaws[PLAYERS];
    const float aim = atan2f(HALF_LANE, DISTANCE) * 180 / M_PI;
    imp_cvar_set("g_unlag", "1");
    for (int i = 0; i < PLAYERS; i++) {
        edict_t *ent = players[i];
        float offset = aim * (i & 1 ? -1 : 1);
        yaws[i] = i & 2 ? 180 - offset : offset;
        if (dense)
            ent->health = 10000;
        for (int ago = 1; ago <= HISTORY; ago++) {
            const unlag_pose_t *pose = G_UnlagHistory(ent, h_frames - ago);
            records += pose && (dense ? pose->origin[0] == ent->s.origin[0] - ago
                                      : pose->origin[1] == ent->s.origin[1] - 5 * ago);
        }
        hu_arm(ent, "Super Shotgun");
        hu_command(ent, 0, UNLAG_ESTIMATE, chosen, 0);
        unlag_shot_t shot = G_UnlagBeginShot(ent);
        estimates += shot.estimated && shot.rewind && shot.frame == chosen;
    }
    CHECK(records == PLAYERS * HISTORY && estimates == PLAYERS,
          "history and estimates verified before timing (%d records, %d estimates)", records, estimates);
    if (h_failures)
        return;
    imp_cvar_set("g_unlag", mode);
    hu_srand(0x2315u);
    int clips = h_unlag_clips, messages = h_nmsgs;
    int hits[2] = { 0 }, damage = 0;
    struct timespec wall0, wall1, cpu0, cpu1;
    clock_gettime(CLOCK_MONOTONIC, &wall0);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
    for (int i = 0; i < PLAYERS; i++) {
        if (!dense) {
            hu_command(players[i], BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
            continue;
        }
        int opposite = (i ^ 2) & ~1;
        edict_t *low = players[opposite], *high = players[opposite + 1];
        int low_health = low->health, high_health = high->health;
        hu_command_yaw(players[i], BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, yaws[i]);
        hits[0] += low_health > low->health;
        hits[1] += high_health > high->health;
        damage += low_health - low->health + high_health - high->health;
    }
    h_frame();
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
    clock_gettime(CLOCK_MONOTONIC, &wall1);

    int volleys = 0;
    for (int i = 0; i < PLAYERS; i++)
        volleys += hu_ssg_attacks(players[i]);
    clips = h_unlag_clips - clips;
    CHECK(volleys == PLAYERS && h_count_msgs(H_SVC_MUZZLEFLASH, MZ_SSHOTGUN, messages) == PLAYERS,
          "exactly 32 super shotgun volleys in the timed world frame (%d attacks)", volleys);
    uint32_t rng = hu_rand();

    if (dense) {
        CHECK(hits[0] == PLAYERS && hits[1] == PLAYERS && damage > 0,
              "dense volleys damage players in both fans (%d/%d damaged, %d damage)", hits[0], hits[1], damage);
        CHECK(mode[0] == '0' ? clips == 0 : clips > 0,
              "dense mode %s has the expected historical clip activity (%d clips)", mode, clips);
        printf("DENSE low=%d high=%d damage=%d\n", hits[0], hits[1], damage);
    } else {
        CHECK(!clips, "mode %s clipped %d boxes behind the muzzle (expected 0)", mode, clips);
        edict_t *shooter = players[0], *target = players[1];
        hu_position(shooter, -512, 96 * 2 + 5 * (HISTORY - AGE), 24);
        target->health = 1000;
        imp_cvar_set("g_unlag", "0");
        CHECK(!hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0), "control rail misses the current box");
        int before = h_unlag_clips;
        imp_cvar_set("g_unlag", "1");
        CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0) == 100 && h_unlag_clips == before + 1,
              "control rail hits the historical box with exactly one clip");
    }
    if (h_failures)
        return;
    printf("BENCH mode=%s volleys=%d records=%d estimates=%d clips=%d rng=%" PRIu32 " wall_ns=%lld cpu_ns=%lld\n",
           mode, volleys, records, estimates, clips, rng, hu_elapsed_ns(wall0, wall1), hu_elapsed_ns(cpu0, cpu1));
}

static void sc_unlag_bench(void) { hu_bench(false); }
static void sc_unlag_bench_dense(void) { hu_bench(true); }

#ifdef HARNESS_OPENTDM
static void sc_unlag_timeout(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    TDM_BeginMatch();
    hu_position(shooter, 0, 0, 24);
    hu_position(target, 512, 0, 24);
    target->health = 1000;
    hu_arm(shooter, "Railgun");
    imp_cvar_set("g_unlag", "1");

    imp_cvar_set("g_timeout_captain", "0");
    h_client_command(shooter, "time");
    CHECK(tdm_match_status == MM_TIMEOUT, "match entered timeout through the player command");
    if (tdm_match_status != MM_TIMEOUT)
        return;
    int match_frame = level.framenum;
    h_client_command(shooter, "time");
    int to_fight = level.match_resume_framenum - level.realframenum;
    CHECK(to_fight > 0, "the timeout caller starts the resume countdown");
    if (to_fight <= 0)
        return;
    h_frames_n(to_fight);
    CHECK(tdm_match_status == MM_TIMEOUT && level.framenum == match_frame && target->s.origin[1] == 0,
          "timeout keeps the match clock and pose fixed while world frames advance");
    int paused = h_frames - (h_div + 1) / 2;
    const unlag_pose_t *pose = G_UnlagHistory(target, paused);
    CHECK(pose && pose->origin[1] == 0, "timeout world frame records the frozen target pose");
    hu_command(shooter, 0, UNLAG_ESTIMATE, paused, 0);
    int prints = h_nbprints;
    h_frame();
    bool fight = false;
    for (int i = prints; i < h_nbprints; i++)
        fight |= !strcmp(h_bprints[i].text, "Fight!\n");
    CHECK(tdm_match_status == MM_PLAYING && fight, "Fight! resumes the match on the real clock deadline");
    hu_strafe(target, h_div - 1, 30 / h_div, 30 / h_div);
    int messages = h_nmsgs;
    hu_command(shooter, BUTTON_ATTACK, UNLAG_REPEAT, 0, 0);
    for (int i = 0; i < h_div && !h_count_msgs(H_SVC_MUZZLEFLASH, MZ_RAILGUN, messages); i++)
        h_frame();
    CHECK(target->health == 900 && h_count_msgs(H_SVC_MUZZLEFLASH, MZ_RAILGUN, messages) == 1,
          "first rail after Fight! uses the estimate received during timeout (damage %d)", 1000 - target->health);

    imp_cvar_set("g_unlag", "0");
    CHECK(hu_rail(shooter, target, UNLAG_REPEAT, 0, 0) == 0, "current-world rail misses the former timeout pose");
}

static void sc_unlag_match_start(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    hu_position(target, 512, 0, 24);
    int chosen = h_frames;
    h_frame();
    imp_cvar_set("g_unlag", "2");
    hu_command(shooter, 0, UNLAG_ESTIMATE, chosen, 0);
    CHECK(G_UnlagHistory(target, chosen) && G_UnlagBeginShot(shooter).rewind,
          "before match reset, target history and shooter estimate are usable");

    TDM_ResetLevel();
    CHECK(target->s.origin[0] == 512 && target->health > 0,
          "the level reset leaves this player alive in place");
    CHECK(!G_UnlagHistory(target, chosen) && !G_UnlagBeginShot(shooter).estimated,
          "the level reset discards player history and estimates before any respawn");

    chosen = h_frames;
    h_frame();
    hu_command(shooter, 0, UNLAG_ESTIMATE, chosen, 0);
    CHECK(G_UnlagHistory(target, chosen) && G_UnlagBeginShot(shooter).rewind,
          "before countdown ends, target history and shooter estimate are usable again");

    tdm_match_status = MM_COUNTDOWN;
    level.match_start_framenum = level.framenum;
    h_frame();
    CHECK(tdm_match_status == MM_PLAYING && target->s.origin[0] != 512,
          "end of countdown starts the match and respawns the target");
    CHECK(!G_UnlagHistory(target, chosen) && !G_UnlagBeginShot(shooter).estimated,
          "match start discards the old pose and the old estimate");
    hu_position(shooter, 0, 0, 24);
    hu_position(target, 512, 64, 24);
    target->health = 1000;
    CHECK(hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0) == 0 &&
          hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 64) == 100,
          "pre-match ray misses and the current position is hittable");
}

static void sc_unlag_teammate(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    hu_join(1, "opponent");
    edict_t *target = hu_join(2, "teammate");
    TDM_BeginMatch();
    CHECK(TEAM(shooter) == TEAM(target) && TEAM(target) != TEAM_SPEC, "shooter and target play for the same team");
    teamplayer_t *stats = shooter->client->resp.teamplayerinfo;
    for (int blocked = 0; blocked <= 1; blocked++) {
        imp_cvar_set("dmflags", va("%d", blocked ? DF_NO_FRIENDLY_FIRE : 0));
        hu_position(shooter, 0, 0, 24);
        hu_position(target, 512, 0, 24);
        target->health = 1000;
        int chosen = h_frames;
        h_frame();
        int team_dealt = stats->team_dealt;
        imp_cvar_set("g_unlag", "0");
        int control = hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0);
        float push = target->velocity[0];
        CHECK(control == (blocked ? 0 : 100) && stats->team_dealt - team_dealt == control,
              "current teammate damage and team_dealt match with protection %s", blocked ? "on" : "off");
        hu_position(target, 512, 64, 24);
        team_dealt = stats->team_dealt;
        int clips = h_unlag_clips;
        imp_cvar_set("g_unlag", "1");
        int damage = hu_rail(shooter, target, UNLAG_ESTIMATE, chosen, 0);
        CHECK(damage == control && h_unlag_clips > clips,
              "historical teammate receives the same friendly-fire decision with protection %s",
              blocked ? "on" : "off");
        CHECK(push > 0 && target->velocity[0] == push,
              "historical teammate receives the same knockback with protection %s", blocked ? "on" : "off");
        CHECK(stats->team_dealt - team_dealt == damage,
              "historical teammate damage is counted in team_dealt with protection %s", blocked ? "on" : "off");
    }
}

static void sc_unlag_match_stats(void)
{
    hu_scene(4);
    h_start(h_ents);
    edict_t *shooter = hu_join_shooter();
    edict_t *target = hu_join(1, "target");
    TDM_BeginMatch();
    hu_position(shooter, 0, 0, 24);
    hu_position(target, 128, 0, 24);
    target->health = 1000;
    int chosen = h_frames;
    h_frame();
    teamplayer_t *stats = shooter->client->resp.teamplayerinfo;
    unsigned damage = 0;
    for (int mode = 0; mode <= 1; mode++) {
        int shots = stats->shots_fired[TDMG_SHOTGUN];
        int hits = stats->shots_hit[TDMG_SHOTGUN];
        unsigned dealt = stats->damage_dealt[TDMG_SHOTGUN];
        int health = target->health;
        if (mode)
            hu_position(target, 128, 64, 24);
        imp_cvar_set("g_unlag", mode ? "1" : "0");
        hu_arm(shooter, "Shotgun");
        hu_srand(123);
        hu_command(shooter, BUTTON_ATTACK, UNLAG_ESTIMATE, chosen, 0);
        unsigned delta = stats->damage_dealt[TDMG_SHOTGUN] - dealt;
        if (!mode)
            damage = delta;
        CHECK(stats->shots_fired[TDMG_SHOTGUN] == shots + 1 &&
              stats->shots_hit[TDMG_SHOTGUN] == hits + 1 &&
              delta == damage && delta > 0 && delta == health - target->health,
              "mode %d records one shot, one hit and the same shotgun damage (%u)",
              mode, delta);
    }
}
#endif
