/* T03: CLS_EXACTFPS values on the fork's SV_ParseClientSetting
 * and set_client_fps (server/user.c included directly; only message I/O and
 * logging are stubbed). Only 1 on protocol
 * 36 agrees (repeat: same reply); any other value or protocol gets no reply
 * and leaves the agreed state as it is. */
#include "server/user.c"

server_t sv;
client_t *sv_client;
static client_t peer;
static int in[2], offset, replies, wire[16], written;
static int failures, checks;
static cvar_t zero;
cvar_t *developer = &zero;

int MSG_ReadShort(void) { return in[offset++]; }
void MSG_WriteByte(int c) { wire[written++] = c; }
void MSG_WriteLong(int c) { wire[written++] = c; }
void SV_ClientAddMessage(client_t *client, int flags)
{
    (void)client; (void)flags; replies++;
}
void Com_LPrintf(print_type_t type, const char *fmt, ...)
{
    (void)type; (void)fmt;
}

static void setting(int index, int value)
{
    in[0] = index; in[1] = value; offset = 0;
    SV_ParseClientSetting();
}

static void reset_peer(int protocol)
{
    memset(&peer, 0, sizeof(peer));
    sv_client = &peer;
    sv.framerate = 60;
    sv.frametime = Com_ComputeFrametime(60);
    peer.protocol = protocol;
    peer.framediv = 6;
    peer.framenum = 1;
    replies = written = 0;
}

static void check_value(const char *name, int value, bool exact, int ack, int fps)
{
    replies = written = 0;
    setting(CLS_EXACTFPS, value);
    int cap_replies = replies;
    bool ack_ok = ack ? written == 3 && wire[0] == svc_setting &&
        wire[1] == SVS_EXACTFPS && wire[2] == 1 : written == 0;
    setting(CLS_FPS, 0);
    int actual = sv.framerate / peer.framediv;
    bool pass = peer.exactfps == exact && cap_replies == ack && ack_ok && actual == fps;
    failures += !pass;
    checks++;
    printf("%s protocol=%d capability=%d exact=%d ack_count=%d fps=%d expected=%d %s\n",
           name, peer.protocol, value, peer.exactfps, cap_replies, actual, fps, pass ? "PASS" : "FAIL");
}

int main(void)
{
    const int values[] = {0, 1, 2, -1};
    for (size_t i = 0; i < q_countof(values); i++) {
        reset_peer(PROTOCOL_VERSION_Q2PRO);
        check_value("fresh", values[i], values[i] == 1, values[i] == 1, values[i] == 1 ? 60 : 20);
    }
    reset_peer(PROTOCOL_VERSION_Q2PRO);
    setting(CLS_EXACTFPS, 1);
    check_value("repeat", 1, true, 1, 60);

    const int unknown[] = {0, 2, -1};
    for (size_t i = 0; i < q_countof(unknown); i++) {
        reset_peer(PROTOCOL_VERSION_Q2PRO);
        setting(CLS_EXACTFPS, 1);
        check_value("after-agreement", unknown[i], true, 0, 60);
    }

    reset_peer(PROTOCOL_VERSION_DEFAULT);
    check_value("unsupported-protocol", 1, false, 0, 10);
    reset_peer(PROTOCOL_VERSION_R1Q2);
    check_value("unsupported-protocol", 1, false, 0, 10);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
