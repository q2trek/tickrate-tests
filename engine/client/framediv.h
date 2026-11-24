/* the API3 engine harness runs these cases with the listen-server
 * objects, so the same executable can write and decode a snapshot. */
#include "shared/framediv.h"

enum { FD_SEED = H_ENT - 1, FD_PLAIN = H_ENT + 1, FD_HIDDEN = H_ENT + 2 };
static bool framediv_request = true;
static bool framediv_partial_extension;
static bool framediv_mvd_fixture;
static int framediv_step, framediv_calls, framediv_receiver_div;
static const opentdm60_framediv_v1_t *framediv_api;

static float framediv_origin(void) { return 200.19f + 10 * framediv_step; }
static float framediv_old_origin(void) { return 170.31f + 10 * framediv_step; }
static float framediv_angle(void) { return 30.9f + 22 * framediv_step; }

static qboolean framediv_presentation(edict_t *receiver, edict_t *ent, customize_entity_t *temp)
{
    if (ent->s.number == FD_HIDDEN) {
        framediv_calls++;
        return qfalse;
    }
    bool partial_extension = framediv_request || framediv_partial_extension;
    if (ent->s.number != H_ENT && (ent->s.number != FD_SEED || !partial_extension))
        return qfalse;

    temp->s = ent->s;
    if (ent->s.number == FD_SEED) {
        temp->x = (entity_state_extension_t){ .morefx = 4 };
        return qtrue;
    }

    framediv_receiver_div = framediv_api && framediv_api->ClientFrameDiv ? framediv_api->ClientFrameDiv(receiver) : -1;
    temp->s.origin[0] = framediv_origin();
    temp->s.origin[1] = 4.39f;
    temp->s.old_origin[0] = framediv_old_origin();
    temp->s.angles[YAW] = framediv_angle();
    if (!partial_extension)
        temp->x = (entity_state_extension_t){ 0 };
    return qtrue;
}

static const game_export_ex_t framediv_exports = {
    .apiversion = GAME_API_VERSION_EX,
    .structsize = sizeof(framediv_exports),
    .CustomizeEntityToClient = framediv_presentation,
};

const game_export_ex_t *h_framediv_exports(const game_import_ex_t *import)
{
    framediv_api = framediv_request ? import->GetExtension(OPENTDM60_FRAMEDIV_V1) : NULL;
    return &framediv_exports;
}

static void h_framediv_game_frame(game_import_t *gi, edict_t *edicts)
{
    if (!framediv_mvd_fixture)
        return;

    // Hold the grid position while the callback presents the shot ahead of it.
    edicts[H_ENT].s.origin[0] = 80.125f;
    gi->linkentity(&edicts[H_ENT]);
    edicts[FD_HIDDEN].inuse = qtrue;
    edicts[FD_HIDDEN].svflags = SVF_NOCLIENT;
    edicts[FD_HIDDEN].s.modelindex = 1;
}

static bool framediv_start(void)
{
    setenv("H_GAME_EXT", "1", 1);
    h_mvd = framediv_mvd_fixture;
    if (!h_join())
        return false;
    CHECK(!framediv_request || (framediv_api && framediv_api->ClientFrameDiv),
          "OPENTDM60_FRAMEDIV_V1 available");
    return true;
}

static bool framediv_at_60(void)
{
    if (h_rate == 60)
        return true;
    INFO("scenario applies to a 60 Hz world");
    h_skipped = true;
    return false;
}

static void framediv_negotiate(client_t *client, int fps)
{
    const byte setting[] = { clc_setting, CLS_FPS, 0, fps, 0 };
    SZ_Clear(&msg_write);
    SZ_InitRead(&msg_read, setting, sizeof(setting));
    MSG_BeginReading();
    SV_ExecuteClientMessage(client);
}

static void framediv_divisor(void)
{
    if (!framediv_start() || !framediv_api || !framediv_api->ClientFrameDiv)
        return;

    client_t *client = h_sv_client();
    CHECK(framediv_api->ClientFrameDiv(NULL) == 0 &&
          framediv_api->ClientFrameDiv(EDICT_NUM(0)) == 0 &&
          framediv_api->ClientFrameDiv(EDICT_NUM(H_ENT)) == 0,
          "non-player edicts return zero");
    client_t *joining = &svs.client_pool[1];
    joining->state = cs_connected;
    CHECK(framediv_api->ClientFrameDiv(joining->edict) == 0, "connecting client returns zero");

    if (h_rate != 60) {
        CHECK(framediv_api->ClientFrameDiv(client->edict) == 1, "full-rate client returns one");
        return;
    }

    static const int divs[] = { 1, 2, 3, 6 };
    for (int i = 0; i < q_countof(divs); i++) {
        int div = divs[i], rate = h_rate / div;
        framediv_negotiate(client, rate);
        CHECK(client->settings[CLS_FPS] == rate && framediv_api->ClientFrameDiv(client->edict) == div,
              "%d snapshots/s: agreed divisor %d", rate, div);
    }
#if USE_FPS
    client->exactfps = false;
    framediv_negotiate(client, 30);
    CHECK(client->settings[CLS_FPS] == 20 && framediv_api->ClientFrameDiv(client->edict) == 3,
          "legacy request 30 agrees 20 and divisor 3");
#endif
}

static void framediv_ignore_message(client_t *client, const byte *data, size_t len, bool reliable)
{
    (void)client; (void)data; (void)len; (void)reliable;
}

static void framediv_write_frame(client_t *client, int number, int delta)
{
    client->framenum = number;
    client->lastframe = delta;
    SV_BuildClientFrame(client);
    SZ_Clear(&msg_write);
    CHECK(SV_WriteFrameToClient_Enhanced(client, MAX_MSGLEN), "wrote complete frame %d (base %d)", number, delta);
}

static void framediv_parse_frame(void)
{
    SZ_InitRead(&msg_read, msg_write.data, msg_write.cursize);
    MSG_BeginReading();
    CL_ParseServerMessage();
}

static void framediv_receiver(void)
{
    if (!framediv_at_60() || !framediv_start())
        return;

    client_t *local = h_sv_client();
    CHECK(local == svs.client_pool && svs.maxclients >= 4, "four receiver slots");
    if (local != svs.client_pool || svs.maxclients < 4)
        return;

    Cvar_Set("g_features", va("%d", g_features->integer | GMF_CLIENTNUM));
    static entity_packed_t entities[4][64];
    for (int i = 0; i < 4; i++) {
        client_t *client = &svs.client_pool[i];
        client->ge = ge;
        client->csr = local->csr;
        client->cm = local->cm;
        client->protocol = local->protocol;
        client->version = local->version;
        client->esFlags = local->esFlags;
        client->psFlags = local->psFlags;
        client->AddMessage = framediv_ignore_message;
        client->maxclients = svs.maxclients;
        client->state = cs_spawned;
        client->entities = entities[i];
        client->num_entities = q_countof(entities[i]);
        client->next_entity = 0;
#if USE_FPS
        client->exactfps = true;
#endif
        int clientnum = i < 2 ? i : 0;
        gclient_new_t *game_client = (gclient_new_t *)client->edict->client;
        game_client->clientNum = clientnum;
        game_client->ps.pmove.pm_type = i < 2 ? PM_FREEZE : PM_SPECTATOR;

        int expected = i % 2 ? 6 : 1;
        framediv_negotiate(client, h_rate / expected);
        framediv_receiver_div = -1;
        framediv_write_frame(client, 100 + i, -1);
        framediv_parse_frame();
        CHECK(cl.frame.valid && cl.frame.clientNum == clientnum, "receiver %d snapshot pov %d", i, clientnum);
        CHECK(framediv_receiver_div == expected, "receiver divisor slot %d (pov %d): got %d, want %d",
              i, clientnum, framediv_receiver_div, expected);
    }
}

static void framediv_place_entities(int world, int div)
{
    sv.framenum = world - div;
    for (int i = FD_SEED; i <= FD_HIDDEN; i++) {
        edict_t *ent = EDICT_NUM(i);
        ent->inuse = qtrue;
        ent->svflags = i == FD_HIDDEN ? SVF_NOCLIENT : 0;
        ent->s.frame = 0;
        ent->s.modelindex = 1;
        ent->s.origin[0] = 40 + i;
        PF_LinkEdict(ent);
    }
    sv.framenum = world;
    for (int i = FD_SEED; i <= FD_HIDDEN; i++) {
        edict_t *ent = EDICT_NUM(i);
        ent->s.origin[0] = 80 + i;
        ent->s.old_origin[0] = 12;
        PF_LinkEdict(ent);
    }
}

static float framediv_coord(float coord) { return COORD2SHORT(coord) / 8.0f; }

static void framediv_snapshot(void)
{
    if (!framediv_at_60() || !framediv_start())
        return;

    client_t *client = h_sv_client();
    for (int div = 2; div <= 3; div++) {
        framediv_negotiate(client, h_rate / div);
        CHECK(framediv_api && framediv_api->ClientFrameDiv && framediv_api->ClientFrameDiv(client->edict) == div,
              "snapshot recipient agreed divisor %d", div);
        for (int lost = 0; lost <= 1; lost++) {
            int n = 100 + 10 * div + 3 * lost;
            centity_t *shot = &cl_entities[H_ENT];
            float origin[3], yaw[3];
            for (framediv_step = 0; framediv_step < 3; framediv_step++) {
                framediv_place_entities(1000 + n * 6 + framediv_step * div, div);
                framediv_write_frame(client, n + framediv_step, framediv_step ? n : -1);
                origin[framediv_step] = framediv_coord(framediv_origin());
                if (lost && framediv_step == 1)
                    continue;
                framediv_parse_frame();
                yaw[framediv_step] = shot->current.angles[YAW];
                CHECK(cl.frame.valid && cl.frame.number == n + framediv_step &&
                      shot->serverframe == cl.frame.number && shot->current.origin[0] == origin[framediv_step] &&
                      shot->current.origin[1] == framediv_coord(4.39f) &&
                      shot->current.angles[YAW] == SHORT2ANGLE(ANGLE2SHORT(framediv_angle()) & 0xff00),
                      "div %d loss %d step %d: decoded origin and angle", div, lost, framediv_step);
                if (!framediv_step) {
                    CHECK(shot->current.old_origin[0] == framediv_coord(framediv_old_origin()),
                          "custom old_origin at first appearance: got %.3f, want %.3f",
                          shot->current.old_origin[0], framediv_coord(framediv_old_origin()));
                    CHECK(cl_entities[FD_SEED].current.morefx == 4 && shot->current.morefx == 0,
                          "zero customized extension after seeded entity: seed %d, shot %d",
                          cl_entities[FD_SEED].current.morefx, shot->current.morefx);
                    CHECK(cl_entities[FD_PLAIN].current.old_origin[0] == 40 + FD_PLAIN,
                          "unmodified entity uses world history");
                    CHECK(cl_entities[FD_HIDDEN].serverframe != cl.frame.number && !framediv_calls,
                          "SVF_NOCLIENT entity excluded before customization");
                } else if (framediv_step == 2) {
                    CHECK(cl.frame.delta == n && cl.oldframe.number == n + !lost,
                          "delta base %d, last received %d", cl.frame.delta, cl.oldframe.number);
                    CHECK(shot->current.old_origin[0] == origin[0] && shot->prev.origin[0] == origin[!lost],
                          "%s: delta old_origin %.3f from N-2, lerp %.3f from N-%d",
                          lost ? "lost N-1" : "delayed ack", shot->current.old_origin[0], shot->prev.origin[0], 2 - !lost);
                    float previous_yaw = yaw[lost ? 2 : 1];
                    CHECK(shot->prev.angles[YAW] == previous_yaw,
                          "div %d %s: previous yaw %.3f, want %.3f",
                          div, lost ? "lost N-1" : "delayed ack", shot->prev.angles[YAW], previous_yaw);
                }
            }
        }
    }
}

static void framediv_legacy(void)
{
    framediv_request = false;
    if (!framediv_at_60() || !framediv_start())
        return;

    client_t *client = h_sv_client();
    // Captured with the same fixture and the previous packet builder.
    static const char *const expected[2][3] = {
        {
            "14640000f80001ff030804100100005a8188800109019005c000000000009180"
            "80010a007005c00000000000818880010b01b005c000000000000000",
            "14650000f80001ff030804100100005a81888001090190051003000000009180"
            "80010a007005200300000000818880010b01b0053003000000000000",
            "14660000f80001ff030804100100005a81888001090190051003000000009180"
            "80010a007005200300000000818880010b01b0053003000000000000",
        },
        {
            "14670000f80001ff030804100100005a8188800109019005c000000000009780"
            "80010a00520c460015a40a00000000818880010b01b005c000000000000000",
            "14680000f80001ff030804100100005a81888001090190051003000000009780"
            "80010a00520c460015200300000000818880010b01b0053003000000000000",
            "14690000f80001ff030804100100005a81888001090190051003000000009780"
            "80010a00520c460015200300000000818880010b01b0053003000000000000",
        },
    };
    static const int divs[] = { 1, 2, 6 };
    for (int custom = 0; custom <= 1; custom++) {
        gex = custom ? &framediv_exports : NULL;
        for (int i = 0; i < q_countof(divs); i++) {
            int div = divs[i];
            framediv_negotiate(client, h_rate / div);
            framediv_place_entities(1000 + 10 * (3 * custom + i), div);
            framediv_write_frame(client, 100 + 3 * custom + i, -1);
            char hex[512];
            CHECK(msg_write.cursize < sizeof(hex) / 2, "legacy packet fits fixture buffer");
            if (msg_write.cursize >= sizeof(hex) / 2)
                return;
            for (unsigned j = 0; j < msg_write.cursize; j++)
                snprintf(hex + 2 * j, 3, "%02x", msg_write.data[j]);
            CHECK(!strcmp(hex, expected[custom][i]),
                  "legacy packet callback %d div %d matches the reference packet: %s", custom, div, hex);
            framediv_parse_frame();
            float old = div > 1 ? 40 + H_ENT : custom ? framediv_coord(framediv_old_origin()) : 12;
            CHECK(cl_entities[H_ENT].current.old_origin[0] == old,
                  "legacy old_origin callback %d div %d: got %.3f, want %.3f",
                  custom, div, cl_entities[H_ENT].current.old_origin[0], old);
        }
    }

    framediv_partial_extension = true;
    framediv_negotiate(client, 30);
    framediv_place_entities(1060, 2);
    framediv_write_frame(client, 106, -1);
    framediv_parse_frame();
    const centity_t *seed = &cl_entities[FD_SEED], *shot = &cl_entities[H_ENT];
    CHECK(cl.frame.valid && cl.frame.number == 106 &&
          seed->serverframe == 106 && shot->serverframe == 106 &&
          seed->current.morefx == 4 && shot->current.morefx == 0,
          "zero customized extension without a request: seed %d, shot %d",
          seed->current.morefx, shot->current.morefx);
}

static void framediv_reload(void)
{
    if (!framediv_at_60() || !framediv_start())
        return;

    client_t *client = h_sv_client();
    framediv_negotiate(client, 30);
    framediv_request = false;
    SV_InitGameProgs();
    client->ge = ge;
    framediv_place_entities(1000, 2);
    framediv_write_frame(client, 100, -1);
    framediv_parse_frame();
    CHECK(cl_entities[H_ENT].current.old_origin[0] == 40 + H_ENT,
          "reload without request restores legacy old_origin");

    framediv_request = true;
    SV_InitGameProgs();
    CHECK(framediv_api && framediv_api->ClientFrameDiv && framediv_api->ClientFrameDiv(client->edict) == 2,
          "reloaded game requests working divisor again");
}

static void framediv_record(void)
{
    framediv_mvd_fixture = true;
    setenv("H_GAME_MVD_VISIBLE", "1", 1);
    if (!framediv_start())
        return;
    h_exec("record " H_SHA);
    h_exec("mvdrecord " H_SHA);
    h_run(1000);
    CHECK(cl_entities[H_ENT].current.origin[0] == framediv_coord(framediv_origin()) && !framediv_calls,
          "live snapshot presents the projectile but excludes the hidden projectile");
    h_exec("mvdstop");
    h_exec("stop");
}

static void framediv_play_dm2(void)
{
    Q_strlcpy(h_home, getenv("CL_HOME"), sizeof(h_home));
    setenv("CL_REPLAY_DM2", "+demo\n" H_SHA ".dm2", 1);
    h_init_replay("CL_REPLAY_DM2", NULL);
    bool active = h_replay_active(10000);
    CHECK(active && !svs.initialized && cl_entities[H_ENT].serverframe == cl.frame.number &&
          cl_entities[H_ENT].current.origin[0] == framediv_coord(framediv_origin()),
          "DM2 playback without a server shows customized origin %.3f", cl_entities[H_ENT].current.origin[0]);
}

static void framediv_mvd(void)
{
    if (!framediv_at_60())
        return;

    h_make_home();
    // The fixture's synthetic projectile can be outside the spectator's PVS.
    setenv("CL_REPLAY_MVD", ""
           "+set\nsv_novis\n1\n+mvdplay\n" H_SHA ".mvd2", 1);
    if (!h_child("framediv_record")) {
        CHECK(!h_child("framediv_play_dm2"), "client demo from the same recording session");
        h_init_replay("CL_REPLAY_MVD", NULL);
        bool active = h_replay_active(10000);
        CHECK(active, "MVD playback active");
        if (active) {
            CHECK(cl_entities[H_ENT].serverframe == cl.frame.number &&
                  cl_entities[H_ENT].current.origin[0] == 80.125f,
                  "MVD viewer shows physical grid origin %.3f", cl_entities[H_ENT].current.origin[0]);
            CHECK(cl_entities[FD_HIDDEN].serverframe != cl.frame.number,
                  "MVD viewer sees no hidden projectile");
        }
    } else {
        CHECK(false, "frame divisor MVD recording");
    }

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/baseq2/demos/" H_SHA ".dm2", h_home);
    unlink(dir);
    snprintf(dir, sizeof(dir), "%s/baseq2/demos/" H_SHA ".mvd2", h_home);
    unlink(dir);
    snprintf(dir, sizeof(dir), "%s/baseq2/demos", h_home);
    rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/baseq2", h_home);
    rmdir(dir);
    rmdir(h_home);
}
