/*
 * Test game for the client timeline harness, loaded by the real listen
 * server through sys_forcegamelib (dlopen, new API). Edicts past the clients
 * are free for the driver, which shapes them from RunFrame (h_game_runframe)
 * through the real game imports. The hooks are weak: under a plain server
 * (an upstream host of the fleet) the game is an empty world.
 */
#include "shared/shared.h"
#include "shared/game.h"

extern void h_game_runframe(game_import_t *gi, edict_t *edicts, int frame) __attribute__((weak));
extern void h_game_command(game_import_t *gi, edict_t *ent) __attribute__((weak));
extern void h_game_think(edict_t *ent) __attribute__((weak));

#define H_MAX_EDICTS 64
static game_import_t gi;
static game_export_t ge;
static edict_t edicts[H_MAX_EDICTS];
static gclient_new_t clients[16];
static int frames;

static void G_Init(void)
{
    int maxclients = (int)gi.cvar("maxclients", "4", 0)->value;
    if (maxclients > 16) gi.error("maxclients > 16");
    for (int i = 0; i < H_MAX_EDICTS; i++) edicts[i].s.number = i;
    for (int i = 0; i < maxclients; i++) {
        edicts[i + 1].client = (gclient_t *)&clients[i];
        clients[i].ps.fov = 90;
    }
    ge.edicts = edicts;
    ge.edict_size = sizeof(edict_t);
    ge.num_edicts = H_MAX_EDICTS;
    ge.max_edicts = H_MAX_EDICTS;
    char f[16];
    snprintf(f, sizeof(f), "%d", GMF_VARIABLE_FPS | GMF_MVDSPEC);   /* MVD dummy client for mvdrecord */
    gi.cvar_forceset("g_features", f);
}

static void G_Shutdown(void) {}

static void G_SpawnEntities(const char *map, const char *ents, const char *spawn)
{
    (void)map; (void)ents; (void)spawn;
    frames = 0;
    edicts[0].inuse = qtrue;
}

static void G_WriteGame(const char *f, qboolean a) { (void)f; (void)a; }
static void G_ReadGame(const char *f) { (void)f; }
static void G_WriteLevel(const char *f) { (void)f; }
static void G_ReadLevel(const char *f) { (void)f; }

static qboolean G_ClientConnect(edict_t *ent, char *userinfo) { (void)ent; (void)userinfo; return qtrue; }
static void G_ClientBegin(edict_t *ent)
{
    gclient_new_t *cl = (gclient_new_t *)ent->client;
    ent->inuse = qtrue;
    cl->ps.fov = 90;
    cl->ps.pmove.pm_type = PM_FREEZE;
}
static void G_ClientUserinfoChanged(edict_t *ent, char *userinfo) { (void)ent; (void)userinfo; }
static void G_ClientDisconnect(edict_t *ent) { ent->inuse = qfalse; }
static void G_ClientCommand(edict_t *ent) { if (h_game_command) h_game_command(&gi, ent); }
static void G_ClientThink(edict_t *ent, usercmd_t *cmd) { (void)cmd; if (h_game_think) h_game_think(ent); }

static void G_RunFrame(void)
{
    frames++;
    if (h_game_runframe)
        h_game_runframe(&gi, edicts, frames);
}

static void G_ServerCommand(void) {}

q_exported game_export_t *GetGameAPI(game_import_t *import)
{
    gi = *import;
    ge.apiversion = GAME_API_VERSION;  /* new API: the client flags (USE_CLIENT) select it */
    ge.Init = G_Init;
    ge.Shutdown = G_Shutdown;
    ge.SpawnEntities = G_SpawnEntities;
    ge.WriteGame = G_WriteGame;
    ge.ReadGame = G_ReadGame;
    ge.WriteLevel = G_WriteLevel;
    ge.ReadLevel = G_ReadLevel;
    ge.ClientConnect = G_ClientConnect;
    ge.ClientBegin = G_ClientBegin;
    ge.ClientUserinfoChanged = G_ClientUserinfoChanged;
    ge.ClientDisconnect = G_ClientDisconnect;
    ge.ClientCommand = G_ClientCommand;
    ge.ClientThink = G_ClientThink;
    ge.RunFrame = G_RunFrame;
    ge.ServerCommand = G_ServerCommand;
    return &ge;
}
