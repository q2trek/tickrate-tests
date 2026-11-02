/*
 * Observer game library for the scheduler harness, loaded by the real
 * dedicated server through sys_forcegamelib (dlopen, API 3). It only
 * reports engine callbacks to the driver; scenario actions inside RunFrame
 * (AddCommandString, error) are run by the driver through gi.
 */
#include "shared/shared.h"
#include "shared/game.h"

enum { H_EV_SPAWN = 1, H_EV_RUNFRAME, H_EV_THINK, H_EV_BEGIN, H_EV_SHUTDOWN };

extern void h_game_event(int kind, int a, int b);
extern void h_game_runframe(game_import_t *gi, edict_t *edicts, int n);

#define H_MAX_EDICTS 64
static game_import_t gi;
static game_export_t ge;
static edict_t edicts[H_MAX_EDICTS];
static gclient_old_t clients[16];  /* API 3 client prefix as the server reads it */
static int frames;

static void G_Init(void)
{
    int maxclients = (int)gi.cvar("maxclients", "4", 0)->value;
    if (maxclients > 16) gi.error("maxclients > 16");
    for (int i = 0; i < H_MAX_EDICTS; i++) edicts[i].s.number = i;
    for (int i = 0; i < maxclients; i++) edicts[i + 1].client = (gclient_t *)&clients[i];
    ge.edicts = edicts;
    ge.edict_size = sizeof(edict_t);
    ge.num_edicts = maxclients + 1;
    ge.max_edicts = H_MAX_EDICTS;
    /* at SCHED_RATE != 10 advertise variable FPS as the mods do (a fixed
     * engine ignores it) */
    const char *rate = getenv("SCHED_RATE");
    if (rate && atoi(rate) != 10) {
        char f[16];
        snprintf(f, sizeof(f), "%d", GMF_VARIABLE_FPS);
        gi.cvar_forceset("g_features", f);
    }
}

static void G_Shutdown(void) { h_game_event(H_EV_SHUTDOWN, 0, 0); }

static void G_SpawnEntities(const char *map, const char *ents, const char *spawn)
{
    (void)map; (void)ents; (void)spawn;
    frames = 0;
    edicts[0].inuse = qtrue;
    h_game_event(H_EV_SPAWN, 0, 0);
}

static void G_WriteGame(const char *f, qboolean a) { (void)f; (void)a; }
static void G_ReadGame(const char *f) { (void)f; }
static void G_WriteLevel(const char *f) { (void)f; }
static void G_ReadLevel(const char *f) { (void)f; }

static qboolean G_ClientConnect(edict_t *ent, char *userinfo) { (void)ent; (void)userinfo; return qtrue; }
static void G_ClientBegin(edict_t *ent) { ent->inuse = qtrue; h_game_event(H_EV_BEGIN, ent->s.number, 0); }
static void G_ClientUserinfoChanged(edict_t *ent, char *userinfo) { (void)ent; (void)userinfo; }
static void G_ClientDisconnect(edict_t *ent) { ent->inuse = qfalse; }
static void G_ClientCommand(edict_t *ent) { (void)ent; }
static void G_ClientThink(edict_t *ent, usercmd_t *cmd) { (void)ent; h_game_event(H_EV_THINK, cmd->msec, cmd->buttons); }

static void G_RunFrame(void)
{
    frames++;
    h_game_event(H_EV_RUNFRAME, frames, 0);
    h_game_runframe(&gi, edicts, frames);
}

static void G_ServerCommand(void) {}

q_exported game_export_t *GetGameAPI(game_import_t *import)
{
    gi = *import;
    ge.apiversion = GAME_API_VERSION;
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
