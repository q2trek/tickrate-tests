/*
 * Engine collision fixture of the API3 harness (HARNESS_ENGINE builds).
 *
 * Linked with the unmodified objects of a plain native q2proded build
 * (as engine/scheduler). The scenario boxes are compiled into a real BSP
 * (axial brushes, a BSP tree split on their faces) and loaded with
 * CM_LoadMap; the collision imports are the engine's own: SV_Trace,
 * SV_PointContents, SV_AreaEdicts, PF_LinkEdict/PF_UnlinkEdict, PF_Pmove
 * and the inline model bounds of CM_InlineModel.
 *
 * Inline model bounds are written one unit smaller: the loader spreads them
 * by a unit, so the module sees the scenario box exactly (door move
 * distances are the same as in the box harness).
 */
#include "src/server/server.h"   /* via -I<q2pro> from the engine compile command */
#include "system/hunk.h"

#include <sys/stat.h>
#include <unistd.h>

extern cvar_t *com_fatal_error, *console_prefix, *developer;

typedef struct { float lo[3], hi[3]; } hebox_t;

/* ------------------------------------------------------------ BSP writer */

#define HE_MAX 4096

static struct { float normal[3], dist; } he_planes[HE_MAX];
static struct { int planenum, children[2]; } he_nodes[HE_MAX];
static struct { int contents, cluster, area, firstbrush, numbrushes; } he_leafs[HE_MAX];
static uint16_t he_leafbrushes[HE_MAX];
static int he_nplanes, he_nnodes, he_nleafs, he_nleafbrushes;

static const hebox_t *he_boxes;     /* brushes of the model being built */
static int he_brush0;               /* index of its first brush */

static int he_plane(int axis, float dist, float sign)
{
    if (he_nplanes >= HE_MAX) Com_Error(ERR_FATAL, "harness bsp: too many planes");
    he_planes[he_nplanes].normal[0] = he_planes[he_nplanes].normal[1] = he_planes[he_nplanes].normal[2] = 0;
    he_planes[he_nplanes].normal[axis] = sign;
    he_planes[he_nplanes].dist = sign * dist;
    return he_nplanes++;
}

static int he_leaf(int contents, const int *ids, int n)
{
    if (he_nleafs >= HE_MAX || he_nleafbrushes + n > HE_MAX) Com_Error(ERR_FATAL, "harness bsp: too many leafs");
    he_leafs[he_nleafs].contents = contents;
    he_leafs[he_nleafs].cluster = contents ? 0xffff : 0;
    he_leafs[he_nleafs].area = contents ? 0 : 1;
    he_leafs[he_nleafs].firstbrush = he_nleafbrushes;
    he_leafs[he_nleafs].numbrushes = n;
    for (int i = 0; i < n; i++) he_leafbrushes[he_nleafbrushes++] = he_brush0 + ids[i];
    return ~he_nleafs++;
}

/* region [lo, hi] with the boxes `ids` that may overlap it: split on a box
 * face strictly inside the region; no such face left means every overlapping
 * box covers the whole region (solid leaf) or none overlaps (empty leaf) */
static int he_build(const float lo[3], const float hi[3], const int *ids, int n)
{
    int in[256], m = 0;
    for (int i = 0; i < n; i++) {
        const hebox_t *b = &he_boxes[ids[i]];
        if (b->lo[0] < hi[0] && b->hi[0] > lo[0] && b->lo[1] < hi[1] && b->hi[1] > lo[1] &&
            b->lo[2] < hi[2] && b->hi[2] > lo[2])
            in[m++] = ids[i];
    }
    for (int i = 0; i < m; i++) {
        const hebox_t *b = &he_boxes[in[i]];
        for (int a = 0; a < 3; a++) {
            for (int s = 0; s < 2; s++) {
                float d = s ? b->hi[a] : b->lo[a];
                if (!(d > lo[a] && d < hi[a])) continue;
                if (he_nnodes >= HE_MAX) Com_Error(ERR_FATAL, "harness bsp: too many nodes");
                int node = he_nnodes++;
                he_nodes[node].planenum = he_plane(a, d, 1);
                float flo[3], bhi[3];
                VectorCopy(lo, flo); flo[a] = d;
                VectorCopy(hi, bhi); bhi[a] = d;
                he_nodes[node].children[0] = he_build(flo, hi, in, m);     /* front: x[a] >= d */
                he_nodes[node].children[1] = he_build(lo, bhi, in, m);
                return node;
            }
        }
    }
    return he_leaf(m ? CONTENTS_SOLID : 0, in, m);
}

static void he_put(FILE *f, const void *p, size_t n)
{
    if (fwrite(p, 1, n, f) != n) Com_Error(ERR_FATAL, "harness bsp: write failed");
}
static void he_i32(FILE *f, int32_t v) { he_put(f, &v, 4); }
static void he_i16(FILE *f, int v) { int16_t s = (int16_t)v; he_put(f, &s, 2); }
static void he_f32(FILE *f, float v) { he_put(f, &v, 4); }

static char he_dir[256];
static char he_map[512];

static void he_cleanup(void)
{
    if (!he_dir[0]) return;
    unlink(he_map);
    rmdir(va("%s/baseq2/maps", he_dir));
    rmdir(va("%s/baseq2", he_dir));
    rmdir(he_dir);
}

/* model 0: world boxes; model k (1..ninline): inline[k] */
static void he_write_bsp(const hebox_t *world, int nworld, const hebox_t *inl, int ninline)
{
    static hebox_t brushes[256];
    static struct { float mins[3], maxs[3]; int headnode; } models[64];
    int nbrushes = 0, ids[256];

    he_leaf(CONTENTS_SOLID, NULL, 0);      /* leaf 0: the solid leaf */
    for (int k = 0; k <= ninline; k++) {
        const hebox_t *src = k ? &inl[k] : world;
        int n = k ? 1 : nworld;
        float lo[3], hi[3];
        he_boxes = brushes + nbrushes;
        he_brush0 = nbrushes;
        for (int i = 0; i < n; i++) { brushes[nbrushes++] = src[i]; ids[i] = i; }
        for (int a = 0; a < 3; a++) {
            lo[a] = n ? src[0].lo[a] : -1;
            hi[a] = n ? src[0].hi[a] : 1;
            for (int i = 1; i < n; i++) { lo[a] = min(lo[a], src[i].lo[a]); hi[a] = max(hi[a], src[i].hi[a]); }
            models[k].mins[a] = lo[a] + (k ? 1 : 0);
            models[k].maxs[a] = hi[a] - (k ? 1 : 0);
            lo[a] -= 64; hi[a] += 64;      /* the tree region: a margin of empty space */
        }
        int first = he_nnodes;
        models[k].headnode = he_build(lo, hi, ids, n);
        if (k == 0 && (models[0].headnode != 0 || first != 0))
            Com_Error(ERR_FATAL, "harness bsp: world without a node");
    }

    static int sides[256 * 6];
    for (int i = 0; i < nbrushes; i++)
        for (int a = 0; a < 3; a++) {
            sides[6 * i + 2 * a] = he_plane(a, brushes[i].hi[a], 1);
            sides[6 * i + 2 * a + 1] = he_plane(a, brushes[i].lo[a], -1);
        }

    FILE *f = fopen(he_map, "wb");
    if (!f) Com_Error(ERR_FATAL, "harness bsp: cannot write %s", he_map);
    static const char ents[] = "{\n\"classname\" \"worldspawn\"\n}\n";
    struct { int ofs, len; } lumps[HEADER_LUMPS] = { 0 };
    int pos = 8 + HEADER_LUMPS * 8;
    he_put(f, (const char[]){ 'I', 'B', 'S', 'P' }, 4);
    he_i32(f, BSPVERSION);
    long dir = ftell(f);
    he_put(f, lumps, sizeof(lumps));
#define LUMP(n) (lumps[n].ofs = pos)
#define END(n) (lumps[n].len = (int)ftell(f) - lumps[n].ofs, pos = (int)ftell(f))
    LUMP(0); he_put(f, ents, sizeof(ents) - 1); END(0);
    LUMP(1);
    for (int i = 0; i < he_nplanes; i++) {
        for (int a = 0; a < 3; a++) he_f32(f, he_planes[i].normal[a]);
        he_f32(f, he_planes[i].dist);
        he_i32(f, 0);       /* type: recomputed by the loader */
    }
    END(1);
    /* visibility: one cluster that sees itself */
    LUMP(3); he_i32(f, 1); he_i32(f, 12); he_i32(f, 12); he_put(f, (const byte[]){ 0xff }, 1); END(3);
    LUMP(4);
    for (int i = 0; i < he_nnodes; i++) {
        he_i32(f, he_nodes[i].planenum);
        he_i32(f, he_nodes[i].children[0]);
        he_i32(f, he_nodes[i].children[1]);
        for (int j = 0; j < 8; j++) he_i16(f, 0);      /* mins, maxs, firstface, numfaces */
    }
    END(4);
    LUMP(8);
    for (int i = 0; i < he_nleafs; i++) {
        he_i32(f, he_leafs[i].contents);
        he_i16(f, he_leafs[i].cluster);
        he_i16(f, he_leafs[i].area);
        for (int j = 0; j < 8; j++) he_i16(f, 0);      /* mins, maxs, leaffaces */
        he_i16(f, he_leafs[i].firstbrush);
        he_i16(f, he_leafs[i].numbrushes);
    }
    END(8);
    LUMP(10); for (int i = 0; i < he_nleafbrushes; i++) he_i16(f, he_leafbrushes[i]); END(10);
    LUMP(13);
    for (int k = 0; k <= ninline; k++) {
        for (int a = 0; a < 3; a++) he_f32(f, models[k].mins[a]);
        for (int a = 0; a < 3; a++) he_f32(f, models[k].maxs[a]);
        for (int a = 0; a < 3; a++) he_f32(f, 0);
        he_i32(f, models[k].headnode);
        he_i32(f, 0); he_i32(f, 0);
    }
    END(13);
    LUMP(14); for (int i = 0; i < nbrushes; i++) { he_i32(f, 6 * i); he_i32(f, 6); he_i32(f, CONTENTS_SOLID); } END(14);
    LUMP(15); for (int i = 0; i < 6 * nbrushes; i++) { he_i16(f, sides[i]); he_i16(f, -1); } END(15);
    LUMP(17); for (int i = 0; i < 2; i++) { he_i32(f, 0); he_i32(f, 0); } END(17);
#undef LUMP
#undef END
    for (int n = 0; n < HEADER_LUMPS; n++)
        if (!lumps[n].len) lumps[n].ofs = pos;
    fseek(f, dir, SEEK_SET);
    he_put(f, lumps, sizeof(lumps));
    fclose(f);
}

/* ------------------------------------------------------------- interface */

void he_init(void);
void he_load_map(const float *world, int nworld, const float *inl, int ninline);
void he_set_game(void *exports);
void he_inline_bounds(const char *name, float *mins, float *maxs);
trace_t he_trace(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, edict_t *pass, int mask);
int he_pointcontents(const vec3_t p);
int he_boxedicts(const vec3_t mins, const vec3_t maxs, edict_t **list, int maxcount, int areatype);
void he_link(edict_t *ent);
void he_unlink(edict_t *ent);
void he_pmove(void *pm);

void he_init(void)
{
    Z_Init();
    Hunk_Init();
    MSG_Init();
    Cbuf_Init();
    Cmd_Init();
    Cvar_Init();
    com_fatal_error = Cvar_Get("com_fatal_error", "1", 0);     /* an engine error ends the run */
    console_prefix = Cvar_Get("console_prefix", "", 0);         /* engine prints: no console, dropped */
    developer = Cvar_Get("developer", "0", 0);
    const char *tmp = getenv("TMPDIR");
    Q_snprintf(he_dir, sizeof(he_dir), "%s/api3-engine-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(he_dir)) { he_dir[0] = 0; Com_Error(ERR_FATAL, "harness: mkdtemp failed"); }
    atexit(he_cleanup);
    mkdir(va("%s/baseq2", he_dir), 0700);
    mkdir(va("%s/baseq2/maps", he_dir), 0700);
    Q_snprintf(he_map, sizeof(he_map), "%s/baseq2/maps/harness.bsp", he_dir);
    sys_basedir = Cvar_Get("basedir", he_dir, CVAR_NOSET);
    sys_homedir = Cvar_Get("homedir", "", CVAR_NOSET);
    sys_libdir = Cvar_Get("libdir", he_dir, CVAR_NOSET);
    FS_Init();
    BSP_Init();
    CM_Init();
    svs.csr = cs_remap_old;
    PmoveInit(&svs.pmp);
}

void he_load_map(const float *world, int nworld, const float *inl, int ninline)
{
    he_write_bsp((const hebox_t *)world, nworld, (const hebox_t *)inl, ninline);
    int ret = CM_LoadMap(&sv.cm, "maps/harness.bsp");
    if (ret) Com_Error(ERR_FATAL, "harness: CM_LoadMap: %s", BSP_ErrorString(ret));
    if (sv.cm.cache->nummodels != ninline + 1) Com_Error(ERR_FATAL, "harness: %d models", sv.cm.cache->nummodels);
    sv.state = ss_game;
}

void he_set_game(void *exports)
{
    ge = exports;
    SV_ClearWorld();
}

void he_inline_bounds(const char *name, float *mins, float *maxs)
{
    const mmodel_t *mod = CM_InlineModel(&sv.cm, name);
    VectorCopy(mod->mins, mins);
    VectorCopy(mod->maxs, maxs);
}

trace_t he_trace(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, edict_t *pass, int mask)
{
    return SV_Trace(start, mins, maxs, end, pass, mask);
}

int he_pointcontents(const vec3_t p) { return SV_PointContents(p); }
int he_boxedicts(const vec3_t mins, const vec3_t maxs, edict_t **list, int maxcount, int areatype)
{
    return SV_AreaEdicts(mins, maxs, list, maxcount, areatype);
}
void he_link(edict_t *ent) { PF_LinkEdict(ent); }
void he_unlink(edict_t *ent) { PF_UnlinkEdict(ent); }
void he_pmove(void *pm) { PF_Pmove(pm); }
