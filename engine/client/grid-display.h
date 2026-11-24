/* The actual API3 module supplies the private fields; read the result from
 * cl_entities after the engine builds, writes, and parses the snapshot. */
static edict_t *(*display_projectile)(const float *from, const float *to, int age);
static void (*display_phase)(edict_t *shot, int age);
static void (*display_angles)(edict_t *shot, float from, float to);
static void (*display_move)(edict_t *shot, bool rotate, bool rollback);
static void (*display_freeze)(void);

static float display_angle(float angle)
{
    return SHORT2ANGLE((int16_t)(ANGLE2SHORT(angle) & 0xff00));
}

static void display_check(centity_t *decoded, const vec3_t want, const char *message)
{
    vec3_t packed = { framediv_coord(want[0]), framediv_coord(want[1]), framediv_coord(want[2]) };
    CHECK(cl.frame.valid && decoded->serverframe == cl.frame.number &&
          VectorCompare(decoded->current.origin, packed), "%s: got %.3f %.3f %.3f, want %.3f %.3f %.3f",
          message, decoded->current.origin[0], decoded->current.origin[1], decoded->current.origin[2],
          packed[0], packed[1], packed[2]);
}

static edict_t *display_make(float dx)
{
    vec3_t from, to;
    VectorCopy(h_sv_client()->edict->s.origin, from);
    from[2] += 32;
    VectorCopy(from, to);
    to[0] += dx;
    return display_projectile(from, to, 0);
}

static void display_write(client_t *client, int number, int delta, bool parse)
{
    framediv_write_frame(client, number, delta);
    if (parse)
        framediv_parse_frame();
}

static void grid_display(void)
{
    if (!h_join())
        return;

    client_t *client = h_sv_client();
    edict_t *shot = display_make(65);
    centity_t *decoded = &cl_entities[shot->s.number];
    vec3_t to, from;
    VectorCopy(shot->s.origin, to);
    VectorCopy(shot->s.old_origin, from);
    int div = h_rate / 10, number = cl.frame.number + 100;

    for (int d = 1; d <= div; d++) {
        if (div % d)
            continue;
        framediv_negotiate(client, h_rate / d);
#if USE_FPS
        CHECK(client->framediv == d, "receiver negotiated frame divisor %d", d);
#endif
        for (int lost = 0; lost <= (d == 2 || d == 3); lost++) {
            int first = number;
            float positions[3], angles[3];
            int frames = div / d + 1;
            for (int step = 0; step < frames; step++, number++) {
                int j = step * d;
                display_phase(shot, j);
                // A distinguishable angle at every snapshot exposes the two lerp branches.
                display_angles(shot, 350, 10 + 20 * step);
                display_write(client, number, step ? first : -1, !(lost && step == 1));
                float frac = (float)min(j + d, div) / div;
                vec3_t want = { from[0] + 65 * frac, to[1], to[2] };
                float angle = display_angle(350 + (20 + 20 * step) * frac);
                if (step < 3) {
                    positions[step] = framediv_coord(want[0]);
                    angles[step] = angle;
                }
                if (lost && step == 1)
                    continue;
                display_check(decoded, want, va("decoded motion d=%d j=%d loss=%d", d, j, lost));
                CHECK(decoded->current.angles[YAW] == angle,
                      "decoded shortest arc d=%d j=%d loss=%d", d, j, lost);
                if (!step && d < div)
                    CHECK(decoded->current.old_origin[0] == framediv_coord(from[0]),
                          "first appearance uses f0 at d=%d", d);
                if (step == 2 && (d == 2 || d == 3)) {
                    CHECK(cl.frame.delta == first && cl.oldframe.number == first + !lost &&
                          decoded->current.old_origin[0] == positions[0] &&
                          decoded->prev.origin[0] == positions[!lost] &&
                          decoded->prev.angles[YAW] == angles[lost ? 2 : 1],
                          "%s: decoder uses N-%d to start interpolation", lost ? "lost N-1" : "delayed ack", 2 - !lost);
                }
            }
        }
    }
    if (div > 1) {
        framediv_negotiate(client, h_rate);
        edict_t *new_shot = display_projectile(from, to, 1);
        CHECK(new_shot->s.old_origin[0] == to[0], "physical old_origin differs from the mid-window f0");
        display_write(client, number, -1, true);
        centity_t *first = &cl_entities[new_shot->s.number];
        vec3_t want = { from[0] + 65 * (2.0f / div), to[1], to[2] };
        display_check(first, want, "mid-window first appearance decodes f1");
        float f0 = framediv_coord(from[0] + 65 * (1.0f / div));
        CHECK(first->current.old_origin[0] == f0 && first->prev.origin[0] == f0,
              "mid-window first appearance decodes nonzero f0");
    }
}

static void grid_display_angles(void)
{
    if (h_rate == 10) {
        h_skipped = true;
        puts("  SKIP shortest arc requires intermediate snapshots");
        return;
    }
    if (!h_join())
        return;

    client_t *client = h_sv_client();
    edict_t *shot = display_make(65);
    centity_t *decoded = &cl_entities[shot->s.number];
    int div = h_rate / 10, number = cl.frame.number + 100;
    const float start[] = { 350, 0 }, end[] = { 10, 180 }, arc[] = { 20, -180 };
    for (int i = 0; i < 2; i++) {
        display_angles(shot, start[i], end[i]);
        for (int j = 0; j < div; j++, number++) {
            display_phase(shot, j);
            display_write(client, number, j ? number - 1 : -1, true);
            float angle = display_angle(start[i] + arc[i] * (j + 1) / div);
            CHECK(decoded->serverframe == cl.frame.number && decoded->current.angles[YAW] == angle,
                  "decoded angle arc %.0f phase %d: got %.3f, want %.3f",
                  arc[i], j, decoded->current.angles[YAW], angle);
        }
    }
}

static void grid_display_mover(bool rotate, bool rollback, const char *message)
{
    if (h_rate == 10) {
        h_skipped = true;
        puts("  SKIP mover presentation requires intermediate snapshots");
        return;
    }
    if (!h_join())
        return;

    client_t *client = h_sv_client();
    int div = h_rate / 10;
    int number = cl.frame.number + 100;
    for (int dx = 0; dx <= 12; dx += 12) {
        edict_t *shot = display_make(dx);
        centity_t *decoded = &cl_entities[shot->s.number];
        vec3_t before, want;
        VectorCopy(shot->s.origin, before);
        display_move(shot, rotate, rollback);
        VectorCopy(before, want);
        if (!rollback) {
            want[2] += 8;
            if (rotate) {
                want[0] -= 24;
                want[1] += 24;
            }
        }
        CHECK(fabsf(shot->s.origin[0] - want[0]) < 0.01f &&
              fabsf(shot->s.origin[1] - want[1]) < 0.01f && fabsf(shot->s.origin[2] - want[2]) < 0.01f,
              "physical carrier position %.3f %.3f %.3f with rotation %d, rollback %d",
              shot->s.origin[0], shot->s.origin[1], shot->s.origin[2], rotate, rollback);
        VectorCopy(shot->s.origin, want);
        want[0] -= dx * (1 - 1.0f / div);
        display_write(client, number++, -1, true);
        display_check(decoded, want, message);
    }
}

static void grid_display_push(void) { grid_display_mover(false, false, "decoded translated projectile"); }
static void grid_display_rotate(void) { grid_display_mover(true, false, "decoded rotated projectile"); }
static void grid_display_undo(void) { grid_display_mover(false, true, "decoded rolled back projectile"); }

static bool (*display_in_timeout)(void);
static void (*display_schedule_resume)(edict_t *shot);
static void (*display_free_flight_frame)(void);

static void grid_display_timeout(void)
{
    if (!getenv("CL_DISPLAY_OPENTDM") || h_rate == 10) {
        h_skipped = true;
        puts("  SKIP timeout presentation applies to rated OpenTDM");
        return;
    }
    if (!h_join())
        return;
    client_t *client = h_sv_client();
    edict_t *shot = display_make(65);
    centity_t *decoded = &cl_entities[shot->s.number];
    vec3_t to;
    VectorCopy(shot->s.origin, to);
    display_freeze();
    int div = h_rate / 10, number = cl.frame.number + 100;
    for (int j = 0; j <= div; j++, number++) {
        vec3_t want = { to[0] - 65.0f + 65.0f * ((float)min(j + 1, div) / div), to[1], to[2] };
        display_write(client, number, j ? number - 1 : -1, true);
        display_check(decoded, want, "decoded timeout motion");
        ge->RunFrame();
    }
    display_schedule_resume(shot);
    bool resumed = false;
    bool stepped = false;
    for (int tick = 0; tick <= 2 * div && !stepped; tick++, number++) {
        display_free_flight_frame();
        stepped = shot->s.origin[0] != to[0];
        vec3_t want;
        VectorCopy(to, want);
        if (stepped) {
            CHECK(resumed && shot->s.origin[0] == to[0] + 65,
                  "one 65-unit grid step after timeout resume");
            want[0] += 65.0f / div;
        }
        display_write(client, number, number - 1, true);
        display_check(decoded, want, stepped ? "decoded first new grid step after timeout" :
                                              "decoded settled position through timeout resume");
        resumed |= !display_in_timeout();
    }
    CHECK(resumed && stepped, "timeout resumed and the projectile reached the next grid frame");
}

static int (*display_grid_frame)(void);
static edict_t *(*display_fire)(edict_t *owner);
static void (*display_flight_trace)(bool enable);
static bool (*display_flight_trace_active)(void);

static client_t *display_receiver(client_t *local, int i, int pov, int fps)
{
    static entity_packed_t packets[32][1024];
    client_t *to = &svs.client_pool[i];
    to->ge = ge;
    to->csr = local->csr;
    to->cm = local->cm;
    to->protocol = local->protocol;
    to->version = local->version;
    to->esFlags = local->esFlags;
    to->psFlags = local->psFlags;
    to->AddMessage = framediv_ignore_message;
    to->maxclients = svs.maxclients;
    to->state = cs_spawned;
    to->entities = packets[i];
    to->num_entities = q_countof(packets[i]);
    to->next_entity = 0;
#if USE_FPS
    to->exactfps = true;
#endif
    gclient_old_t *game_client = to->edict->client;
    game_client->clientNum = pov;
    game_client->ps.fov = 90;
    game_client->ps.pmove.pm_type = i == pov ? PM_FREEZE : PM_SPECTATOR;
    framediv_negotiate(to, fps);
    return to;
}

static void grid_display_roles(void)
{
    h_mvd = false;
    if (!h_join())
        return;

    client_t *local = h_sv_client();
    int div = h_rate / 10;
    // Start between grid frames. The mod's clock helpers own the phase.
    if (display_grid_frame())
        ge->RunFrame();
    edict_t *shot = display_fire(local->edict);
    CHECK(shot, "rocket spawned using the actual mod");
    CHECK(!display_flight_trace_active(), "firing restores the original trace import");
    if (!shot)
        return;

    vec3_t from;
    VectorCopy(shot->s.origin, from);
    int number = cl.frame.number + 100;
    if (div > 1) {
        display_write(local, number++, -1, true);
        CHECK(cl_entities[shot->s.number].serverframe != cl.frame.number,
              "rocket hidden from the decoder until its first step");
    }
    for (int j = 0; j < div && !display_grid_frame(); j++)
        display_free_flight_frame();
    display_free_flight_frame();
    CHECK(shot->inuse && shot->s.origin[0] - from[0] == 65,
          "the first physical rocket step is 65 units");
    int shot_num = shot->s.number;
    vec3_t to;
    VectorCopy(shot->s.origin, to);
    Cvar_Set("g_features", va("%d", g_features->integer | GMF_CLIENTNUM));
    for (int i = 0; i < 4; i++) {
        int d = i % 2 ? div : 1;
        int pov = i < 2 ? i : 0;
        client_t *receiver = display_receiver(local, i, pov, h_rate / d);
#if USE_FPS
        CHECK(receiver->framediv == d, "role %d receives divisor %d", i, d);
#endif
        display_write(receiver, number++, -1, true);
        CHECK(cl.frame.clientNum == pov, "role %d decodes viewpoint %d", i, pov);
        vec3_t want = { from[0] + (to[0] - from[0]) * ((float)d / div), to[1], to[2] };
        display_check(&cl_entities[shot_num], want, va("decoded receiver role %d divisor %d", i, d));
        if (i == 2) {
            display_phase(shot, div - 1);
            display_write(receiver, number++, -1, true);
            display_check(&cl_entities[shot_num], to, "fast follower reaches the grid position");
            display_phase(shot, 0);
        }
    }
    CHECK(VectorCompare(shot->s.origin, to), "snapshot customization preserves physical origin");
}

static int (*display_age)(const edict_t *shot);
static void (*display_remove)(edict_t *shot);

static bool display_grid_sample(client_t *client, int number, int frame, float *x)
{
    client->state = cs_spawned;
    client->framenum = frame;
    SV_BuildClientFrame(client);
    client->state = cs_free;

    const client_frame_t *sample = &client->frames[frame & UPDATE_MASK];
    for (unsigned i = 0; i < sample->num_entities; i++) {
        const entity_packed_t *ent = &client->entities[(sample->first_entity + i) % client->num_entities];
        if (ent->number != number)
            continue;
        MSG_BeginWriting();
        MSG_WriteDeltaEntity(NULL, ent, MSG_ES_FORCE | MSG_ES_NEWENTITY);
        SZ_InitRead(&msg_read, msg_write.data, msg_write.cursize);
        MSG_BeginReading();
        uint64_t bits;
        entity_state_t decoded = { 0 };
        entity_state_extension_t ext = { 0 };
        int num = MSG_ParseEntityBits(&bits, 0);
        MSG_ParseDeltaEntity(&decoded, &ext, num, bits, 0);
        SZ_Clear(&msg_write);
        *x = decoded.origin[0];
        return num == number;
    }
    return false;
}

static void grid_display_record(void)
{
    if (!h_join())
        return;
    h_exec("record " H_SHA);
    h_exec("mvdrecord " H_SHA);
    client_t *fast = h_sv_client();
    int slot = 0;
    while (slot < svs.maxclients && svs.client_pool[slot].state != cs_free)
        slot++;
    CHECK(slot < svs.maxclients, "spare slot for the recording's 10 Hz receiver");
    if (slot == svs.maxclients)
        return;
    client_t *slow = display_receiver(fast, slot, slot, 10);
    slow->state = cs_free;
    edict_t *rocket = display_fire(fast->edict);
    CHECK(rocket, "recorded flight spawned");
    if (!rocket)
        return;

    int number = rocket->s.number;
    float start = rocket->s.origin[0];
    centity_t *decoded = &cl_entities[number];
    int div = h_rate / 10, keyshots = 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/projectile.txt", getenv("CL_HOME"));
    FILE *oracle = fopen(path, "w");
    CHECK(oracle, "opened recording oracle");
    if (!oracle)
        return;
    fprintf(oracle, "%d\n", number);
    int last = cl.frame.number, first_key = 0;
    display_flight_trace(true);
    for (int msec = 0; msec < 700; msec++) {
        h_tick(1);
        if (cl.frame.number == last)
            continue;
        last = cl.frame.number;
        int age = display_age(rocket);
        if (!CL_FRAMESYNC || age < 0)
            continue;
        if (!keyshots)
            first_key = last;
        CHECK(last == first_key + keyshots * div && decoded->serverframe == last,
              "recording has consecutive visible keyframes from first appearance");
        float to = rocket->s.origin[0];
        float want = framediv_coord(to - 65 + 65 * ((float)min(age + 1, div) / div));
        CHECK(decoded->current.origin[0] == want, "recorded client keyframe decoded %.3f, want %.3f at age %d",
              decoded->current.origin[0], want, age);
        float slow_x = 0;
        float want_grid = framediv_coord(start + 65 * (keyshots + 1));
        CHECK(display_grid_sample(slow, number, last / div, &slow_x) && slow_x == want_grid,
              "same flight 10 Hz frame %d decoded %.3f, want %.3f", keyshots, slow_x, want_grid);
        fprintf(oracle, "%.9g %.9g\n", want, slow_x);
        keyshots++;
    }
    display_flight_trace(false);
    fclose(oracle);
    CHECK(keyshots >= 2, "recorded %d visible projectile keyframes", keyshots);
    CHECK(!display_flight_trace_active(), "recorded flight restores the original trace import");
    display_remove(rocket);
    h_run(120);
    h_exec("mvdstop");
    h_exec("stop");
}

static void grid_display_play(const char *file, bool mvd)
{
    Q_strlcpy(h_home, getenv("CL_HOME"), sizeof(h_home));
    char path[512];
    snprintf(path, sizeof(path), "%s/projectile.txt", h_home);
    FILE *oracle = fopen(path, "r");
    CHECK(oracle, "read recording oracle");
    if (!oracle)
        return;
    int number;
    float frames[10][2];
    int read = fscanf(oracle, "%d", &number);
    int n = 0;
    while (n < q_countof(frames) && fscanf(oracle, "%f %f", &frames[n][0], &frames[n][1]) == 2)
        n++;
    fclose(oracle);
    CHECK(read == 1 && n >= 2, "recording has multiple expected projectile frames");
    if (read != 1 || n < 2)
        return;

    setenv("CL_DISPLAY_REPLAY", file, 1);
    h_init_replay("CL_DISPLAY_REPLAY", NULL);
    CHECK(h_replay_active(10000), "%s playback active", mvd ? "MVD" : "DM2");
    int last = -1, seen = 0, first = 0;
    centity_t *decoded = &cl_entities[number];
    const char *format = mvd ? "MVD" : "DM2";
    for (int msec = 0; msec < 1500; msec++, h_tick(1)) {
        if (!cl.frame.valid || cl.frame.number == last)
            continue;
        last = cl.frame.number;
        bool visible = decoded->serverframe == last && decoded->current.modelindex;
        if (!visible && !seen)
            continue;
        if (!seen)
            first = last;
        if (seen >= n) {
            CHECK(!visible, "%s has no extra visible frame after the recorded flight", format);
            break;
        }
        float want = frames[seen][mvd];
        CHECK(visible && last == first + seen && decoded->current.origin[0] == want,
              "%s playback frame %d: decoded %.3f, want %.3f", format, seen, decoded->current.origin[0], want);
        seen++;
    }
    CHECK(seen == n, "%s viewer decoded all %d projectile frames (got %d)", format, n, seen);
    if (!mvd)
        CHECK(!svs.initialized, "DM2 plays without a game server");
}

static void grid_display_dm2(void)
{
    grid_display_play("+demo\n" H_SHA ".dm2", false);
}

static void grid_display_mvd(void)
{
    if (h_rate != 60) {
        h_skipped = true;
        puts("  SKIP replay comparison uses 60 Hz and 10 Hz views");
        return;
    }
    h_make_home();
    int record = h_child("grid_display_record");
    CHECK(record == 0, "recorded the same rocket as DM2 and MVD");
    if (record == 0) {
        CHECK(h_child("grid_display_dm2") == 0, "client demo readback");
        grid_display_play(""
                          "+set\nsv_novis\n1\n+mvdplay\n" H_SHA ".mvd2", true);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/projectile.txt", h_home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/baseq2/demos/" H_SHA ".dm2", h_home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/baseq2/demos/" H_SHA ".mvd2", h_home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/baseq2/demos", h_home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/baseq2", h_home);
    rmdir(path);
    rmdir(h_home);
}

static bool (*display_partial_push)(edict_t *shot, vec3_t from);

static void grid_display_partial(void)
{
    if (!h_join())
        return;

    int div = h_rate / 10;
    while (div > 1 && display_grid_frame())
        ge->RunFrame();
    edict_t *shot = display_make(12);
    vec3_t want, from;
    VectorCopy(shot->s.origin, want);
    want[0] -= 24;
    want[1] += 24;
    int links = shot->linkcount;
    CHECK(display_partial_push(shot, from), "partial rider accepted, late blocker rolled the mover back");
    CHECK(DistanceSquared(shot->s.origin, want) < 0.0001f,
          "partial push preserves the legacy rotation-only position");
    vec3_t start;
    VectorCopy(want, start);
    start[0] -= 12;
    CHECK(DistanceSquared(from, start) < 0.0001f,
          "partial push grid_from tracks retained displacement");
    CHECK(shot->linkcount - links == (div > 1 ? 2 : 1),
          "partial push updates the rated projectile's world link");
    want[0] = start[0] + 12.0f / div;
    display_write(h_sv_client(), cl.frame.number + 100, -1, true);
    display_check(&cl_entities[shot->s.number], want, "decoded projectile after partial push and late blocker");
}
