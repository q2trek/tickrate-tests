/* The fixture controls the ACK and command contents. Both netchan
 * endpoints and the server packet parser are the production implementations. */
typedef struct {
    int number;
    int world;
} h_shot_time_snapshot_t;

static void h_shot_time_read(void)
{
    Netchan_Process(&cls.netchan);
}

static void h_shot_time_advance(int world)
{
    int budget = (world - sv.framenum + 1) * 1000 / BASE_FRAMERATE;
    while (sv.framenum < world && budget-- > 0) {
        h_now++;
        com_eventTime++;
        com_localTime++;
        SV_Frame(1);
        NET_GetPackets(NS_CLIENT, h_shot_time_read);
    }
    if (sv.framenum != world)
        CHECK(false, "server advanced to world frame %d (wanted %d)", sv.framenum, world);
}

static void h_shot_time_send(void)
{
    Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 1);
    MSG_BeginWriting();
    SV_Frame(0);
    NET_GetPackets(NS_CLIENT, h_shot_time_read);
}

static client_t *h_shot_time_join(void)
{
    h_mvd = false;
    if (!h_join())
        return NULL;
    client_t *client = h_sv_client();
    CHECK(client && client->protocol == PROTOCOL_VERSION_Q2PRO, "spawned Q2PRO client on loopback");
    h_unlag_observe = true;
    return client;
}

static h_shot_time_snapshot_t h_shot_time_next(client_t *client)
{
    int number = client->framenum;
    for (int i = 0; client->framenum == number && i < h_rate; i++)
        h_shot_time_advance(sv.framenum + 1);
    CHECK(client->framenum == number + 1, "snapshot number %d advanced at world frame %d", number, sv.framenum - 1);
    return (h_shot_time_snapshot_t){ number, sv.framenum - 1 };
}

static void h_shot_time_write_move(int ack, int copies, int count, int angle)
{
    usercmd_t cmd = { .msec = 10, .angles = { 0, angle, 0 } };
    const usercmd_t *previous = NULL;
    MSG_BeginWriting();
    MSG_WriteByte((ack < 0 ? clc_move_nodelta : clc_move_batched) | (copies << SVCMD_BITS));
    if (ack >= 0)
        MSG_WriteLong(ack);
    MSG_WriteByte(0);
    for (int i = 0; i <= copies; i++) {
        int n = i < copies ? 1 : count;
        MSG_WriteBits(n, 5);
        for (int j = 0; j < n; j++) {
            MSG_WriteDeltaUsercmd_Enhanced(previous, &cmd);
            previous = &cmd;
        }
    }
    MSG_FlushBits();
}

static void h_shot_time_move(int ack, int copies, int count, int lost, int angle)
{
    cls.netchan.outgoing_sequence += lost;
    h_shot_time_write_move(ack, copies, count, angle);
    h_shot_time_send();
}

static void h_shot_time_expect(int command, unlag_kind_t kind, int frame, const char *label)
{
    unlag_command_t actual = h_unlag_seen[command & 511];
    CHECK(actual.kind == kind && (kind != UNLAG_ESTIMATE || actual.frame == frame),
          "%s: command context %d frame %d, expected context %d frame %d",
          label, actual.kind, actual.frame, kind, frame);
}

static void h_shot_time_ack(int ack, unlag_kind_t kind, int frame, const char *label)
{
    int start = h_thinks;
    h_shot_time_move(ack, 0, 1, 0, 100);
    CHECK(h_thinks == start + 1, "%s: executed one command", label);
    if (h_thinks == start + 1)
        h_shot_time_expect(start, kind, frame, label);
}

static void h_shot_time_print_status(int target, const char *buffer, size_t size)
{
    (void)target;
    printf("status begin\n%.*sstatus end\n", (int)size, buffer);
}

static void shot_time_estimate(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    int div = h_rate / H_CRATE;
    h_shot_time_snapshot_t snapshot = h_shot_time_next(client);
    int first = sv.framenum;
    Cvar_Set("sv_calcpings_method", "1");
    com_eventTime += 37;
    h_shot_time_ack(snapshot.number, UNLAG_ESTIMATE, snapshot.world - div, "first good ACK");
    CHECK(h_unlag_world == snapshot.world && h_unlag_rate == h_rate,
          "module clock: frame %d at %d Hz, expected frame %d at %d Hz",
          h_unlag_world, h_unlag_rate, snapshot.world, h_rate);
    h_shot_time_ack(-1, UNLAG_NO_ESTIMATE, 0, "nodelta");
    h_shot_time_ack(client->framenum, UNLAG_NO_ESTIMATE, 0, "future ACK");
    const int delays[] = { div == 6 ? 2 : 1, div, div == 6 ? 9 : div + 1 };
    for (int i = 0; i < q_countof(delays); i++) {
        if (i && delays[i] == delays[i - 1])
            continue;
        h_shot_time_advance(first + delays[i]);
        h_shot_time_ack(snapshot.number, UNLAG_ESTIMATE, snapshot.world - div + min(div, delays[i]),
                   va("same ACK at R0 + %d", delays[i]));
    }
    char status[2048];
    Com_BeginRedirect(1, status, sizeof(status), h_shot_time_print_status);
    h_exec("status");
    Com_EndRedirect();
    CHECK(!h_unlag_outside_bad, "another slot and RunFrame see no active command");
}

static void shot_time_ack_window(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    int div = h_rate / H_CRATE;
    h_shot_time_snapshot_t older = h_shot_time_next(client);
    h_shot_time_snapshot_t good = h_shot_time_next(client);
    int first = sv.framenum;
    h_shot_time_ack(good.number, UNLAG_ESTIMATE, good.world - div, "latest ACK");
    h_shot_time_ack(older.number, UNLAG_NO_ESTIMATE, 0, "ACK older than a valid ACK");

    h_fill_reliable(client);
    h_overflow = 1;
    int sends = h_lp_sends;
    h_shot_time_snapshot_t overflow = h_shot_time_next(client);
    CHECK(h_overflowed == 1 && h_lp_sends > sends, "overflow discarded the snapshot but sent reliable data");
    h_shot_time_ack(overflow.number, UNLAG_NO_ESTIMATE, 0, "overflow ACK");

    int fragment = client->framenum;
    byte payload[MAX_PACKETLEN + 1];
    memset(payload, svc_nop, sizeof(payload));
    Netchan_Transmit(&client->netchan, client->netchan.maxpacketlen + 1, payload, 1);
    CHECK(client->netchan.fragment_pending, "netchan still has a fragment to send");
    client->send_time = svs.realtime;
    client->send_delta = 1000;  // keep SV_SendAsyncPackets from draining it before the snapshot
    h_shot_time_next(client);
    client->send_delta = 0;
    CHECK(client->frames[fragment & UPDATE_MASK].number != fragment && (client->frameflags & FF_SUPPRESSED), "snapshot held behind real netchan fragment");
    h_shot_time_ack(fragment, UNLAG_NO_ESTIMATE, 0, "fragment-held ACK");

    client->rate = 1;
    int suppressed = client->framenum;
    int rate_drops = client->suppress_count;
    for (int i = 0; i < UPDATE_BACKUP && client->framenum - good.number <= UPDATE_BACKUP; i++) {
        for (int i = 0; i < RATE_MESSAGES; i++)
            client->message_size[i] = 100;
        if (client->framenum - good.number == UPDATE_BACKUP) {
            CHECK(client->frames[good.number & UPDATE_MASK].number == good.number, "16-back ring entry survives");
            h_shot_time_ack(good.number, UNLAG_ESTIMATE,
                       good.world - div + min(div, sv.framenum - first), "16-back ACK");
        }
        h_shot_time_next(client);
        if (client->framenum == suppressed + 1)
            h_shot_time_ack(suppressed, UNLAG_NO_ESTIMATE, 0, "rate-suppressed ACK");
    }
    CHECK(client->suppress_count > rate_drops &&
          client->frames[good.number & UPDATE_MASK].number == good.number, "17-back ring entry survives rate suppression");
    h_shot_time_ack(good.number, UNLAG_NO_ESTIMATE, 0, "17-back ACK");
    client->rate = 0;
    h_shot_time_advance(sv.framenum + div * UPDATE_BACKUP);
    CHECK(client->frames[good.number & UPDATE_MASK].number != good.number, "old ring entry overwritten");
    h_shot_time_ack(good.number, UNLAG_NO_ESTIMATE, 0, "evicted ACK");
}

static void shot_time_commands(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    int div = h_rate / H_CRATE;
    h_shot_time_snapshot_t snapshot = h_shot_time_next(client);
    int start = h_thinks;
    client->command_msec = 0;
    Cvar_Set("sv_enforcetime", "1");
    h_shot_time_move(snapshot.number, 0, 1, 0, 1234);
    CHECK(h_thinks == start && client->lastcmd.angles[1] == 1234, "first ACK carried a rejected command with new angles");
    h_shot_time_advance(sv.framenum + 1);
    client->command_msec = 200;
    h_shot_time_move(snapshot.number, 0, 1, 2, 4321);
    CHECK(h_thinks == start + 3 && h_think_cmds[start & 511].angles[1] == 1234,
          "two lastcmd runs and one new command reached the module");
    h_shot_time_expect(start, UNLAG_NO_ESTIMATE, 0, "first executed repeat of the rejected original");
    h_shot_time_expect(start + 1, UNLAG_REPEAT, 0, "second executed repeat");
    h_shot_time_expect(start + 2, UNLAG_ESTIMATE, snapshot.world - div + 1, "R0 came from the rejected packet");

    snapshot = h_shot_time_next(client);
    start = h_thinks;
    h_shot_time_move(snapshot.number, 1, 2, 1, 7890);
    CHECK(h_thinks == start + 3, "one recovered and two new commands reached the module");
    for (int i = 0; i < 3; i++)
        h_shot_time_expect(start + i, UNLAG_ESTIMATE, snapshot.world - div, "shared packet estimate");

    snapshot = h_shot_time_next(client);
    usercmd_t cmd = { .msec = 10 };
    start = h_thinks;
    MSG_BeginWriting();
    MSG_WriteByte(clc_move);
    MSG_WriteLong(snapshot.number);
    MSG_WriteDeltaUsercmd(NULL, &cmd, 0);
    MSG_WriteByte(0);
    MSG_WriteDeltaUsercmd(&cmd, &cmd, 0);
    MSG_WriteByte(0);
    MSG_WriteDeltaUsercmd(&cmd, &cmd, 0);
    MSG_WriteByte(0);
    h_shot_time_send();
    CHECK(h_thinks == start + 1, "old move parser executed a command");
    h_shot_time_expect(start, UNLAG_ESTIMATE, snapshot.world - div, "old move ACK");
}

static void shot_time_empty_new_group(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    h_shot_time_snapshot_t snapshot = h_shot_time_next(client);
    client->pacing.window.batch = 0;   /* exclude packets from joining */
    int start = h_thinks;
    int frame = snapshot.world - h_rate / H_CRATE;
    h_shot_time_ack(snapshot.number, UNLAG_ESTIMATE, frame, "command A");
    h_shot_time_move(snapshot.number, 1, 0, 0, 4321);
    CHECK(h_thinks == start + 1, "parsing the unused backup leaves the module's estimate from A alone");
    h_shot_time_move(snapshot.number, 0, 1, 1, 200);
    int a = h_think_cmds[start & 511].angles[1];
    int b = h_think_cmds[(start + 1) & 511].angles[1];
    int c = h_think_cmds[(start + 2) & 511].angles[1];
    CHECK(h_thinks == start + 3 && a == 100 && b == 4321 && c == 200,
          "legacy packet sequence: module saw %d, %d, %d; expected 100, 4321, 200", a, b, c);
    h_shot_time_expect(start + 1, UNLAG_NO_ESTIMATE, 0, "first execution of the unused backup");
    h_shot_time_expect(start + 2, UNLAG_ESTIMATE, frame, "command C");

    gclient_new_t *gc = client->edict->client;
    gc->ps.pmove.velocity[0] = 100;
    h_pacing_pin_origin = true;
    h_shot_time_advance(sv.framenum + 3);
    h_pacing_pin_origin = false;
    h_shot_time_advance(10 * h_rate + 1);
    pacing_maxima_t status = pacing_read();
    CHECK(status.present && status.batch == 1 && status.held == 3,
          "completed status n: heldfrm=%u batch=%u (expected 3 and 1)", status.held, status.batch);

    snapshot = h_shot_time_next(client);
    start = h_thinks;
    h_shot_time_move(snapshot.number, 1, 0, 1, 5432);
    CHECK(h_thinks == start + 1 && h_think_cmds[start & 511].angles[1] == 5432,
          "packet loss recovered the backup from the empty new group");
    h_shot_time_expect(start, UNLAG_ESTIMATE, snapshot.world - h_rate / H_CRATE, "recovered backup");
    h_shot_time_move(snapshot.number, 0, 1, 1, 300);
    CHECK(h_thinks == start + 3 && h_think_cmds[(start + 1) & 511].angles[1] == 5432,
          "next packet repeated the recovered backup");
    h_shot_time_expect(start + 1, UNLAG_REPEAT, 0, "repeat of the executed backup");
}

static void shot_time_rejected_backup(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    h_shot_time_snapshot_t snapshot = h_shot_time_next(client);
    Cvar_Set("sv_enforcetime", "1");
    client->command_msec = 200;
    h_shot_time_ack(snapshot.number, UNLAG_ESTIMATE, snapshot.world - h_rate / H_CRATE, "command A");
    int start = h_thinks;
    client->command_msec = 0;
    h_shot_time_move(snapshot.number, 1, 0, 1, 4321);
    CHECK(h_thinks == start && client->lastcmd.angles[1] == 4321 && client->lastcmd_unseen,
          "selected backup B rejected for time is saved despite a zero-count new group");

    client->command_msec = 200;
    h_shot_time_move(snapshot.number, 0, 1, 2, 7890);
    CHECK(h_thinks == start + 3 && h_think_cmds[start & 511].angles[1] == 4321 &&
          h_think_cmds[(start + 1) & 511].angles[1] == 4321,
          "both repeats use rejected backup B even after previously executing A");
    h_shot_time_expect(start, UNLAG_NO_ESTIMATE, 0, "first executed repeat of the rejected backup");
    h_shot_time_expect(start + 1, UNLAG_REPEAT, 0, "second executed repeat of the rejected backup");
    h_shot_time_expect(start + 2, UNLAG_ESTIMATE, snapshot.world - h_rate / H_CRATE, "next new command");
}

static sizebuf_t h_shot_time_nested_packet;
static unlag_command_t h_shot_time_after_nested;

static void h_shot_time_nest(const unlag_api_v1_t *api, edict_t *outer, edict_t *other)
{
    /* The plain listen build has no OTX bot host. Enter the same think wrapper
     * through the move parser, preserving the outer dispatcher's input. */
    client_t nested = { .edict = other, .state = cs_spawned,
                        .protocol = PROTOCOL_VERSION_Q2PRO, .command_msec = 100 };
    sizebuf_t saved_message = msg_read;
    client_t *saved_client = sv_client;
    h_unlag_nested = NULL;
    msg_read = h_shot_time_nested_packet;
    SV_ExecuteClientMessage(&nested);
    msg_read = saved_message;
    sv_client = saved_client;
    sv_player = outer;
    h_shot_time_after_nested = api->Command(outer);
}

static void shot_time_nested_think(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    h_shot_time_snapshot_t snapshot = h_shot_time_next(client);
    int expected = snapshot.world - h_rate / H_CRATE;
    h_shot_time_write_move(-1, 0, 1, 300);
    byte packet[MAX_PACKETLEN];
    memcpy(packet, msg_write.data, msg_write.cursize);
    SZ_InitRead(&h_shot_time_nested_packet, packet, msg_write.cursize);
    int start = h_thinks;
    h_unlag_nested = h_shot_time_nest;
    h_shot_time_move(snapshot.number, 0, 1, 0, 200);
    h_unlag_nested = NULL;

    CHECK(h_thinks == start + 2 && h_unlag_slots[start & 511] == client->number &&
          h_unlag_slots[(start + 1) & 511] != client->number, "outer and nested commands reached different slots");
    h_shot_time_expect(start, UNLAG_ESTIMATE, expected, "outer think before nesting");
    h_shot_time_expect(start + 1, UNLAG_NO_ESTIMATE, 0, "nested nodelta think");
    CHECK(!h_unlag_outside_bad, "nested think cannot see the outer slot's command");
    CHECK(h_shot_time_after_nested.kind == UNLAG_ESTIMATE && h_shot_time_after_nested.frame == expected,
          "outer think after nesting: context %d frame %d, expected context %d frame %d",
          h_shot_time_after_nested.kind, h_shot_time_after_nested.frame, UNLAG_ESTIMATE, expected);
}

static void h_shot_time_rate(int rate)
{
    MSG_BeginWriting();
    MSG_WriteByte(clc_setting);
    MSG_WriteShort(CLS_FPS);
    MSG_WriteShort(rate);
    h_shot_time_send();
}

static void shot_time_rate_change(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    h_shot_time_rate(20);
    h_shot_time_snapshot_t slow = h_shot_time_next(client);
    h_shot_time_rate(60);
    h_shot_time_ack(slow.number, UNLAG_ESTIMATE, slow.world - 3, "20 Hz ACK after requesting 60");

    int key = 120;
    h_shot_time_advance(key - 1);
    h_shot_time_snapshot_t fast = h_shot_time_next(client);
    CHECK(fast.world == key - 1, "60 Hz snapshot immediately before the keyframe");
    h_shot_time_ack(fast.number, UNLAG_ESTIMATE, key - 2, "60 Hz ACK before requesting 10");
    h_shot_time_rate(10);
    slow = h_shot_time_next(client);
    CHECK(slow.world == key, "10 Hz snapshot on the adjacent keyframe");
    h_shot_time_ack(slow.number, UNLAG_ESTIMATE, key - 6, "10 Hz ACK moves the estimated frame backwards");
}

static void shot_time_lifecycle(void)
{
    h_mvd = false;
    h_update = BASE_FRAMERATE;
    h_init();
    h_shot_time_advance(120);
    CHECK(!h_sv_client(), "no spawned client 120 frames into the map");
    bool joined = h_wait_active(5000);
    CHECK(joined, "client entered mid-map");
    client_t *client = h_sv_client();
    if (!joined || !client)
        return;
    h_unlag_observe = true;
    int div = h_rate / BASE_FRAMERATE;
    h_shot_time_advance(sv.framenum + div * (UPDATE_BACKUP + 3));
    h_shot_time_snapshot_t old = h_shot_time_next(client);
    h_shot_time_ack(old.number, UNLAG_ESTIMATE, old.world - div, "mid-map ACK");

    int epoch = h_epoch;
    h_exec(va("gamemap %s", h_map()));
    for (int i = 0; h_epoch == epoch && i < 5000; i++)
        h_tick(1);
    joined = h_epoch != epoch && h_wait_active(5000);
    CHECK(joined, "client entered the next map");
    client = h_sv_client();
    if (!joined || !client)
        return;

    h_shot_time_snapshot_t fresh = h_shot_time_next(client);
    h_shot_time_ack(fresh.number, UNLAG_ESTIMATE, fresh.world - div, "new map ACK");
    client->rate = 1;
    for (int i = 0; client->framenum <= old.number && i <= old.number; i++) {
        for (int j = 0; j < RATE_MESSAGES; j++)
            client->message_size[j] = 100;
        h_shot_time_next(client);
    }
    CHECK(client->framenum == old.number + 1 &&
          client->frames[old.number & UPDATE_MASK].number == old.number, "previous map entry is still in the ACK window");
    h_shot_time_ack(old.number, UNLAG_NO_ESTIMATE, 0, "previous map ACK");
}

static void shot_time_channel_reset(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    h_shot_time_snapshot_t old = h_shot_time_next(client);
    h_shot_time_ack(old.number, UNLAG_ESTIMATE, old.world - h_rate / H_CRATE, "old channel ACK");
    unsigned epoch = svs.unlag_epoch;

    /* MVD switches call this reset without spawning a new server. */
    SV_ClientReset(client);
    CHECK(client->state == cs_connected && client->framenum == 1 &&
          svs.unlag_epoch == epoch, "client reset restarts numbering in the same server epoch");
    client->state = cs_spawned;
    client->rate = 1;
    while (client->framenum <= old.number) {
        for (int i = 0; i < RATE_MESSAGES; i++)
            client->message_size[i] = 100;
        h_shot_time_next(client);
        if (h_failures)
            return;
    }
    const client_frame_t *frame = &client->frames[old.number & UPDATE_MASK];
    CHECK(client->framenum == old.number + 1 && frame->number == old.number &&
          frame->unlag.world == old.world && frame->unlag.epoch == epoch,
          "same-number snapshot was suppressed and the old entry is still in the ring");
    h_shot_time_ack(old.number, UNLAG_NO_ESTIMATE, 0, "same-number ACK after channel reset");
}

static void shot_time_non_network(void)
{
    client_t *client = h_shot_time_join();
    if (!client)
        return;
    h_shot_time_snapshot_t snapshot = h_shot_time_next(client);
    int expected = snapshot.world - h_rate / H_CRATE;
    unlag_kind_t kind = h_kind == K_OLDSV ? UNLAG_OUTSIDE : UNLAG_ESTIMATE;
    CHECK(h_unlag_rate == h_rate, "extended API reports %d Hz", h_unlag_rate);
    h_shot_time_ack(snapshot.number, kind, expected, "network command");
    CHECK(h_unlag_slots[(h_thinks - 1) & 511] == client->number, "network context belongs to the sender");

    int pace[PACE_MAX];
    memcpy(pace, client->pace, sizeof(pace));
    int start = h_thinks;
    edict_t *dummy = EDICT_NUM(client->number ? 1 : 2);
    usercmd_t cmd = { .msec = BASE_FRAMETIME };
    /* The MVD dummy calls the module directly with sv_client/sv_player set. */
    sv_client = &svs.client_pool[dummy->s.number - 1];
    sv_player = dummy;
    ge->ClientThink(dummy, &cmd);
    sv_client = NULL;
    sv_player = NULL;
    CHECK(h_thinks == start + 1 && h_unlag_slots[start & 511] == dummy->s.number - 1,
          "direct ClientThink reached another slot");
    h_shot_time_expect(start, UNLAG_OUTSIDE, 0, "direct ClientThink");
    CHECK(!memcmp(pace, client->pace, sizeof(pace)), "direct ClientThink did not alter network movement counters");
    h_shot_time_ack(snapshot.number, kind, expected, "next network command");
    CHECK(!h_unlag_outside_bad, "slot lookup never returned another client's context");
}
