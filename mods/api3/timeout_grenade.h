#ifdef HARNESS_OPENTDM
static void (*h_timeout_grenade_explode)(edict_t *);
static vec3_t h_timeout_explosion_origin;
static int h_timeout_explosion_frame, h_timeout_explosion_real;

static void h_timeout_record_explosion(edict_t *ent)
{
    VectorCopy(ent->s.origin, h_timeout_explosion_origin);
    h_timeout_explosion_frame = level.framenum;
    h_timeout_explosion_real = level.realframenum;
    h_timeout_grenade_explode(ent);
}

static void sc_timeout_grenade_fuse(void)
{
    edict_t *player, *opponent;
    h_timeout_pair(&player, &opponent);
    if (!h_tdm_start(player, opponent)) return;
    for (int after = 0; after <= 1; after++) {
        while (level.realframenum % h_div) h_frame();
        int birth = level.framenum;
        vec3_t origin = { 0, 1000, 500 }, dir = { 1, 0, 0 };
        fire_grenade(player, origin, dir, 10, 100, after ? 1.1f : 1.f, 10);
        edict_t *grenade = G_Find(NULL, FOFS(classname), "grenade");
        if (!grenade) imp_error("timeout grenade missing");
        VectorSet(grenade->velocity, 100, 0, 150);
        h_timeout_grenade_explode = grenade->think;
        h_timeout_explosion_frame = h_timeout_explosion_real = -1;
        grenade->think = h_timeout_record_explosion;
        CHECK(grenade->nextthink == birth + (10 + after) * h_div, "fuse lands on the intended match grid frame");

        h_frames_n(10 * h_div);
        h_client_command(player, "time");
        h_frames_n(h_rate);
        CHECK(tdm_match_status == MM_TIMEOUT && grenade->inuse && h_timeout_explosion_frame < 0,
              "grenade does not explode during the frozen world step");
        h_client_command(player, "time");
        int resume = level.match_resume_framenum;
        h_frames_n(resume - level.realframenum + 1);
        CHECK(tdm_match_status == MM_PLAYING && grenade->inuse && h_timeout_explosion_frame < 0,
              "grenade remains live on the resume grid frame");
        h_frames_n(h_div - 1);
        CHECK(grenade->inuse && h_timeout_explosion_frame < 0, "grenade fuse waits until the next grid frame");
        h_frame();
        CHECK(!grenade->inuse && h_timeout_explosion_frame == birth + 11 * h_div &&
              h_timeout_explosion_real == resume + h_div,
              "grenade explodes on the first grid step after resume");
        const vec3_t imported_origin = { 100, 1000, 210 };
        vec3_t error;
        VectorSubtract(h_timeout_explosion_origin, imported_origin, error);
        CHECK(VectorLength(error) <= 0.01f, "grenade explodes at the imported 10 Hz position");
        printf("FUSE %d match=%d after_resume=%d origin=%.4f %.4f %.4f\n", after,
               (h_timeout_explosion_frame - birth) / h_div,
               (h_timeout_explosion_real - resume) / h_div,
               h_timeout_explosion_origin[0], h_timeout_explosion_origin[1], h_timeout_explosion_origin[2]);
    }
}
#endif
