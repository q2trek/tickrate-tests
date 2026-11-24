#ifdef HARNESS_OPENTDM
static edict_t *h_timeout_flying_object(int movetype, float y)
{
    edict_t *ent = G_Spawn();
    ent->movetype = movetype;
    VectorSet(ent->s.origin, 0, y, 1024);
    VectorSet(ent->velocity, 100, 0, 0);
    VectorSet(ent->avelocity, 0, 100, 0);
    gi.linkentity(ent);
    return ent;
}

static void sc_match_timeout_toss(void)
{
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    for (int phase = 0; phase < h_div; phase++) {
        h_real_phase(phase);
        edict_t *airborne = h_timeout_flying_object(MOVETYPE_BOUNCE, 1000);
        edict_t *supported = h_timeout_flying_object(MOVETYPE_TOSS, 1500);
        h_frames_n(h_div);
        int pad = h_timeout_window() - level.realframenum;
        h_client_command(a, "time");
        h_frames_n(pad + 1);
        vec3_t origin, velocity, angles;
        VectorCopy(airborne->s.origin, origin);
        VectorCopy(airborne->velocity, velocity);
        VectorCopy(airborne->s.angles, angles);

        airborne->think = h_timeout_record_think;
        airborne->nextthink = level.framenum + 1;
        h_view_resume(a);
        CHECK(!airborne->count && VectorCompare(airborne->s.origin, origin),
              "phase %d resume grid frame does not run flight or think", phase);
        int still = 1;
        if (h_div > 1) {
            supported->groundentity = world;
            supported->groundentity_linkcount = world->linkcount;
        }
        for (int i = 0; i < h_div - 1; i++) {
            h_frame();
            if (i == 0)
                CHECK(airborne->count == 1, "phase %d think runs during the resume hold", phase);
            still &= VectorCompare(airborne->s.origin, origin) &&
                VectorCompare(airborne->velocity, velocity) &&
                VectorCompare(airborne->s.angles, angles);
        }
        CHECK(still, "phase %d flight waits for every remaining world frame after resume", phase);
        supported->groundentity = NULL;
        float x = supported->s.origin[0];
        h_frame();
        CHECK(airborne->s.origin[0] > origin[0] && airborne->count == 1,
              "phase %d flight resumes on the next grid frame after one think", phase);
        CHECK(supported->s.origin[0] > x, "phase %d support clears the resume hold", phase);
        G_FreeEdict(airborne);
        G_FreeEdict(supported);
    }
}

void CopyToBodyQue(edict_t *ent);

static void sc_match_timeout_body_slot(void)
{
    edict_t *a, *b;
    h_timeout_pair(&a, &b);
    if (!h_tdm_start(a, b)) return;
    edict_t *corpse = h_timeout_flying_object(MOVETYPE_TOSS, 1000);
    edict_t *body = EDICT(game.maxclients + level.body_que + 1);
    CopyToBodyQue(corpse);
    VectorCopy(corpse->velocity, body->velocity);
    h_frames_n(h_div);

    int pad = h_timeout_window() - level.realframenum;
    h_client_command(a, "time");
    h_frames_n(pad + 1);
    h_view_resume(a);

    for (int i = 0; i < BODY_QUEUE_SIZE; i++)
        CopyToBodyQue(corpse);
    CHECK(EDICT(game.maxclients + (level.body_que + BODY_QUEUE_SIZE - 1) % BODY_QUEUE_SIZE + 1) == body,
          "body queue wraps to the original slot");
    float x = body->s.origin[0];
    h_frame();
    CHECK(body->s.origin[0] > x, "reused body slot moves on its first world frame");
}
#endif
