/* Precise schedules use the client driver's virtual clock with real netchan
 * and server move parsing. UDP peers use loopback. */
static client_t *pacing_join(void)
{
    for (int i = 0; i < 5000; i++) {
        client_t *cl = h_sv_client();
        if (cl && cls.state == ca_active) {
            h_pacing_manual = true;
            return cl;
        }
        h_tick(1);
    }
    CHECK(false, "pacing fixture: client did not spawn");
    return NULL;
}

static client_t *pacing_start(int protocol)
{
    h_pacing_protocol34 = protocol == 34;
    h_mvd = false;
    h_init();
    client_t *client = pacing_join();
    if (!client)
        return NULL;
    bool ok = client->protocol == protocol && SV_FRAMERATE == 60;
    CHECK(ok, "pacing fixture: protocol %d at %d Hz", client->protocol, SV_FRAMERATE);
    return ok ? client : NULL;
}

static char pacing_status[2048];

static void pacing_flush(int target, const char *buffer, size_t len)
{
    (void)target;
    Q_assert(len < sizeof(pacing_status));
    memcpy(pacing_status, buffer, len + 1);
}

typedef struct {
    unsigned gap, held, batch;
    bool present;
} pacing_maxima_t;

static void pacing_capture(void)
{
    char redirect[2048];
    pacing_status[0] = 0;
    Com_BeginRedirect(1, redirect, sizeof(redirect), pacing_flush);
    h_exec("status n");
    Com_EndRedirect();
}

static pacing_maxima_t pacing_row(const char *name)
{
    pacing_maxima_t p = {0};
    const char *row = strstr(pacing_status, va("%s ", name));
    if (row)
        p.present = sscanf(row, "%*s %*f %*f %*f %*f %*u%% %u %u %u",
                           &p.gap, &p.held, &p.batch) == 3;
    return p;
}

static pacing_maxima_t pacing_read(void)
{
    pacing_capture();
    return pacing_row("h_client");
}

static void pacing_send_move(client_t *client, netchan_t *chan, int dropped)
{
    usercmd_t cmd = { .forwardmove = 200, .msec = 10 };

    chan->outgoing_sequence = client->netchan.incoming_sequence + 1 + dropped;
    MSG_BeginWriting();
    MSG_WriteByte(clc_move);
    MSG_WriteByte(0);  /* legacy checksum byte */
    MSG_WriteLong(-1);
    for (int i = 0; i < 3; i++) {
        MSG_WriteDeltaUsercmd(i ? &cmd : NULL, &cmd, 0);
        MSG_WriteByte(0);
    }
    h_pacing_send = true;
    Netchan_Transmit(chan, msg_write.cursize, msg_write.data, 1);
    h_pacing_send = false;
}

static void pacing_move(client_t *client, int dropped)
{
    pacing_send_move(client, &cls.netchan, dropped);
    h_tick(1);
}

static int pacing_window(void)
{
    return 10 * SV_FRAMERATE;
}

static void pacing_steady(client_t *client, int until)
{
    while (sv.framenum < until) {
        int frame = sv.framenum;
        pacing_move(client, 0);
        while (sv.framenum == frame)
            h_tick(1);
    }
}

static void pacing_check(const char *scenario, pacing_maxima_t p, unsigned gap,
                         unsigned low_held, unsigned high_held, unsigned batch)
{
    unsigned error = p.gap > gap ? p.gap - gap : gap - p.gap;
    CHECK(p.present && error <= (1000 + SV_FRAMERATE - 1) / (unsigned)SV_FRAMERATE &&
          p.held >= low_held && p.held <= high_held && p.batch == batch,
          "%s: gap=%u held=%u batch=%u (want %u ms ±1 frame, %u–%u frames, %u commands)%s",
          scenario, p.gap, p.held, p.batch, gap, low_held, high_held, batch,
          p.present ? "" : ", missing columns");
}

static void pacing_cross_window(int before_ms, int gap_ms, unsigned min_held, unsigned max_held,
                                const char *scenario)
{
    client_t *client = pacing_start(34);
    if (!client)
        return;
    int boundary = 2 * pacing_window();
    pacing_steady(client, boundary + 1 - before_ms * SV_FRAMERATE / 1000);
    unsigned last = h_now;
    pacing_move(client, 0);
    while (sv.framenum <= boundary)
        h_tick(1);
    pacing_maxima_t first = pacing_read();
    CHECK(abs((int)(h_now - last) - before_ms) <= 1,
          "pacing fixture: boundary after %u ms of silence", h_now - last);
    pacing_check(scenario, first, 17, 0, 1, 1);
    h_run(gap_ms - (int)(h_now - last));
    pacing_move(client, 0);
    pacing_steady(client, boundary + pacing_window() + 1);
    pacing_check(scenario, pacing_read(), gap_ms, min_held, max_held, 1);
}

static void pacing_window_boundary(void)
{
    pacing_cross_window(50, 150, 8, 10, "A pause across the window boundary");
}

static void pacing_unfinished_silence(void)
{
    pacing_cross_window(2000, 7000, 418, 421, "Silence that has not ended");
}

/* The second client has its own netchan and uses local UDP. Both send on h_now. */
static client_t *pacing_join_peer(netchan_t *chan)
{
    netadr_t server, sender;
    NET_Config(NET_NONE);
    Cvar_Set("net_ip", "127.0.0.1");
    Cvar_Set("net_port", "0");
    Cvar_Set("net_clientport", "0");
    Cvar_Set("net_enable_ipv6", "0");
    NET_Config(NET_CLIENT | NET_SERVER);
    bool bound = NET_GetAddress(NS_SERVER, &server) && NET_GetAddress(NS_CLIENT, &sender);
    CHECK(bound, "pacing fixture: peer and server have local UDP sockets");
    if (!bound)
        return NULL;

    Netchan_OutOfBand(NS_CLIENT, &server, "getchallenge\n");
    h_tick(1);
    int challenge = 0;
    for (int i = 0; i < MAX_CHALLENGES; i++)
        if (NET_IsEqualAdr(&svs.challenges[i].adr, &sender))
            challenge = svs.challenges[i].challenge;
    Netchan_OutOfBand(NS_CLIENT, &server, "connect 34 17 %d \"\\name\\steady\"\n", challenge);
    h_tick(1);
    Netchan_Setup(chan, NS_CLIENT, NETCHAN_OLD, &server, 17, MAX_PACKETLEN_WRITABLE_DEFAULT, 34);
    MSG_BeginWriting();
    MSG_WriteByte(clc_stringcmd);
    MSG_WriteString("new");
    MSG_WriteByte(clc_stringcmd);
    MSG_WriteString("\177c version pacing-test");
    MSG_WriteByte(clc_stringcmd);
    MSG_WriteString(va("begin %d", sv.spawncount));
    Netchan_Transmit(chan, msg_write.cursize, msg_write.data, 1);
    h_tick(1);

    client_t *peer = &svs.client_pool[1];
    bool spawned = peer->state == cs_spawned && peer->protocol == 34 && !strcmp(peer->name, "steady");
    CHECK(spawned, "pacing fixture: second protocol-34 client spawned (state %d)", peer->state);
    return spawned ? peer : NULL;
}

static void pacing_single_pause(void)
{
    client_t *client = pacing_start(34);
    if (!client)
        return;
    static netchan_t chan;
    client_t *peer = pacing_join_peer(&chan);
    if (!peer)
        return;
    int pause_at = pacing_window() + 60;
    unsigned before = 0, gap = 0;
    int recovered = 0;
    while (sv.framenum <= 2 * pacing_window()) {
        int frame = sv.framenum;
        if (frame == pause_at)
            before = h_now;
        if (frame == pause_at + 9) {
            gap = h_now - before;
            recovered = h_thinks;
            pacing_send_move(client, &cls.netchan, 8);
        } else if (frame <= pause_at || frame > pause_at + 9) {
            pacing_send_move(client, &cls.netchan, 0);
        }
        pacing_send_move(peer, &chan, 0);
        h_tick(1);
        if (frame == pause_at + 9)
            recovered = h_thinks - recovered - 1;  /* exclude the steady peer */
        while (sv.framenum == frame)
            h_tick(1);
    }
    CHECK(gap == 150 && recovered == 9, "pacing fixture: gap=%u ms, replayed and new commands=%d", gap, recovered);
    pacing_capture();
    pacing_check("A single long pause is visible", pacing_row("h_client"), 150, 8, 10, 3);
    pacing_maxima_t steady = pacing_row("steady");
    CHECK(steady.present && steady.gap > 0 && steady.gap <= 34 && steady.held <= 1 && steady.batch == 1,
          "Steady peer in the same window: gap=%u held=%u batch=%u%s", steady.gap, steady.held, steady.batch,
          steady.present ? "" : ", missing columns");
}

static void pacing_batch_exclusions(void)
{
    client_t *client = pacing_start(34);
    if (!client)
        return;
    for (int reject = 0; reject <= 1; reject++) {
        int boundary = (2 + reject) * pacing_window();
        pacing_steady(client, boundary - 10);
        int thinks = h_thinks;
        int dup = client->pace[PACE_DUPS], rej = client->pace[PACE_REJECTS];
        int allowance = client->command_msec;
        client->command_msec = (5 - reject) * 10;
        pacing_move(client, 4);  /* two lastcmd repeats, two backups and one new */
        CHECK(h_thinks - thinks == 5 - reject && client->pace[PACE_DUPS] - dup == 2 &&
              client->pace[PACE_REJECTS] - rej == reject, "pacing fixture: two repeats, %d rejected", reject);
        client->command_msec = allowance;
        pacing_steady(client, boundary + 1);
        pacing_check("What a batch does not count", pacing_read(), 17, 0, 1, 3 - reject);
    }
}

static void pacing_duplicate_datagram(void)
{
    client_t *client = pacing_start(34);
    if (!client)
        return;
    pacing_steady(client, pacing_window() + 10);
    pacing_move(client, 0);
    h_run(74);
    int packets = client->pace[PACE_PACKETS], thinks = h_thinks;
    __real_NET_SendPacket(NS_CLIENT, h_pacing_packet, h_pacing_len, &cls.netchan.remote_address);
    h_run(75);
    CHECK(client->pace[PACE_PACKETS] == packets && h_thinks == thinks,
          "pacing fixture: duplicate was received without advancing packet or move count");
    pacing_move(client, 0);
    pacing_steady(client, 2 * pacing_window() + 1);
    pacing_check("Duplicate datagram is ignored", pacing_read(), 150, 8, 10, 1);
}

static void pacing_batch_five(void)
{
    client_t *client = pacing_start(36);
    if (!client)
        return;
    Cvar_Set("cl_batchcmds", "1");
    Cvar_Set("cl_maxpackets", "0");
    Cvar_Set("cl_packetdup", "0");
    unsigned seq = cls.netchan.outgoing_sequence;
    cl.history[(seq - 1) & CMD_MASK].cmdNumber = cl.cmdNumber;
    for (int i = 0; i < 5; i++)
        cl.cmds[++cl.cmdNumber & CMD_MASK] = (usercmd_t){ .forwardmove = 200, .msec = 10 };
    int thinks = h_thinks;
    h_pacing_send = true;
    CL_SendCmd();
    h_pacing_send = false;
    h_tick(1);
    CHECK(client->netchan.incoming_sequence == seq && h_thinks - thinks == 5,
          "pacing fixture: one protocol-36 packet delivered five commands (seq %u, thinks %d)",
          client->netchan.incoming_sequence, h_thinks - thinks);
    while (sv.framenum <= pacing_window())
        h_tick(1);
    pacing_maxima_t p = pacing_read();
    CHECK(p.present && p.batch == 5, "Protocol 36 batch of five commands: batch=%u%s",
          p.batch, p.present ? "" : ", missing columns");
}

static void pacing_reset(const char *command, bool map)
{
    client_t *client = pacing_start(34);
    if (!client)
        return;
    pacing_steady(client, pacing_window() + 10);
    gclient_new_t *player = client->edict->client;
    player->ps.pmove.origin[0] = 0;
    h_run(1000);
    int spawn = sv.spawncount, epoch = h_epoch;
    h_pacing_manual = false;
    h_pacing_pin_origin = true;
    h_exec(map ? va("gamemap %s", h_map()) : command);
    /* Run through the first spawned sample before letting the player move. */
    bool sampled = false;
    for (int i = 0; i < 5000; i++) {
        int frame = sv.framenum;
        h_tick(1);
        if (sv.framenum != frame && client->state == cs_spawned) {
            sampled = true;
            break;
        }
    }
    CHECK(sampled && player->ps.pmove.origin[0] == 0 && player->ps.pmove.velocity[0] != 0,
          "pacing fixture: first sample after %s has the same origin and nonzero velocity", command);
    player->ps.pmove.velocity[0] = 0;
    client_t *rejoined = pacing_join();
    h_pacing_pin_origin = false;
    if (!rejoined)
        return;
    CHECK(rejoined == client && h_epoch > epoch && (spawn != sv.spawncount) == map,
          "pacing fixture: same slot after %s (map changed %d)", command, spawn != sv.spawncount);
    int boundary = (sv.framenum / pacing_window() + 1) * pacing_window();
    pacing_steady(rejoined, boundary + 1);
    pacing_check(map ? "New map discards unfinished pause" : "Reconnect discards unfinished pause",
                 pacing_read(), 17, 0, 0, 1);
}

static void pacing_reconnect(void) { pacing_reset("reconnect", false); }
static void pacing_new_map(void) { pacing_reset("gamemap", true); }

static void pacing_host_hitch(void)
{
    client_t *client = pacing_start(34);
    if (!client)
        return;
    pacing_steady(client, pacing_window() + 10);
    pacing_move(client, 0);
    h_tick(1000);
    pacing_move(client, 0);
    pacing_steady(client, 2 * pacing_window() + 1);
    pacing_check("Packet gap uses unclamped host time", pacing_read(), 1000, 1, 8, 1);
}
