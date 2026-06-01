#include <StdInc.h>
#include <winsock2.h>
#include "TcpInputServer.h"

#pragma comment(lib, "ws2_32.lib")

// Packet format (46 bytes), compatible with DemulShooter/batocera-wine-guns:
// X[4](float) Y[4](float) EnableInputsHack(byte) HideCrosshairs(byte)
// Trigger[4](byte) Reload[4](byte) Action[4](byte)
#define TCP_PACKET_SIZE 46

extern int* ffbOffset;   // button bit flags
extern int* ffbOffset2;  // P1 X axis (0-255)
extern int* ffbOffset3;  // P1 Y axis (0-255)
extern int* ffbOffset4;  // P2 X axis (0-255)
extern int* ffbOffset5;  // P2 Y axis (0-255)
extern int* ffbOffset6;  // P3 X axis (0-255)
extern int* ffbOffset7;  // P3 Y axis (0-255)
extern int* ffbOffset8;  // P4 X axis (0-255)
extern int* ffbOffset9;  // P4 Y axis (0-255)

extern linb::ini config;

// Last parsed state — reapplied every 1 ms to override TeknoParrot named-pipe
// writes (which zero the position on Wine because raw input doesn't work).
struct TcpState
{
    int  x[4], y[4]; // 0-255
    int  buttons;    // trigger/action bits (volume bits excluded)
};
static TcpState  s_state    = {};
static volatile bool s_hasData  = false;

static HANDLE s_thread = NULL;
static volatile bool s_running = false;
static int s_port = 0;

// Read "Player N Relative Sensitivity" from teknoparrot.ini.
// A value > 1 means TeknoParrot would scale mouse movement up; we divide the
// range around 0.5 by the same factor so TCP input stays consistent.
static float GetSensitivity(int playerOneBased)
{
    char key[64];
    sprintf_s(key, "Player %d Relative Sensitivity", playerOneBased);
    std::string val = config["General"][key];
    if (!val.empty())
    {
        float s = (float)atof(val.c_str());
        if (s > 0.0f) return s;
    }
    return 1.0f;
}

static int ApplySensitivity(float normalized, float sensitivity)
{
    // Compress movement range around centre: same effect as reducing
    // TeknoParrot's relative sensitivity multiplier.
    float centered = (normalized - 0.5f) / sensitivity + 0.5f;
    float clamped  = centered < 0.0f ? 0.0f : (centered > 1.0f ? 1.0f : centered);
    return (int)(clamped * 255.0f);
}

static void ApplyState()
{
    if (!s_hasData) return;
    // Preserve volume bits (0x10/0x20); overwrite trigger/action bits.
    *ffbOffset  = (*ffbOffset & 0x30) | s_state.buttons;
    *ffbOffset2 = s_state.x[0]; *ffbOffset3 = s_state.y[0];
    *ffbOffset4 = s_state.x[1]; *ffbOffset5 = s_state.y[1];
    *ffbOffset6 = s_state.x[2]; *ffbOffset7 = s_state.y[2];
    *ffbOffset8 = s_state.x[3]; *ffbOffset9 = s_state.y[3];
}

static void ParsePacket(const BYTE* buf)
{
    float axisX[4] = {}, axisY[4] = {};
    bool trigger[4] = {}, reload[4] = {}, action[4] = {};

    int off = 0;
    for (int i = 0; i < 4; i++) { memcpy(&axisX[i], buf + off, 4); off += 4; }
    for (int i = 0; i < 4; i++) { memcpy(&axisY[i], buf + off, 4); off += 4; }
    off += 2; // skip EnableInputsHack, HideCrosshairs
    for (int i = 0; i < 4; i++) trigger[i] = buf[off++] != 0;
    for (int i = 0; i < 4; i++) reload[i]  = buf[off++] != 0;
    for (int i = 0; i < 4; i++) action[i]  = buf[off++] != 0;

    float sens[4] = {
        GetSensitivity(1), GetSensitivity(2),
        GetSensitivity(3), GetSensitivity(4)
    };

    int buttons = 0;

    // Player 1  trigger 0x01  action 0x02
    if (reload[0]) { s_state.x[0] = 0; s_state.y[0] = 0; buttons |= 0x01; }
    else
    {
        s_state.x[0] = ApplySensitivity(axisX[0], sens[0]);
        s_state.y[0] = ApplySensitivity(axisY[0], sens[0]);
        if (trigger[0]) buttons |= 0x01;
    }
    if (action[0]) buttons |= 0x02;

    // Player 2  trigger 0x04  action 0x08
    if (reload[1]) { s_state.x[1] = 0; s_state.y[1] = 0; buttons |= 0x04; }
    else
    {
        s_state.x[1] = ApplySensitivity(axisX[1], sens[1]);
        s_state.y[1] = ApplySensitivity(axisY[1], sens[1]);
        if (trigger[1]) buttons |= 0x04;
    }
    if (action[1]) buttons |= 0x08;

    // Player 3  trigger 0x40
    if (reload[2]) { s_state.x[2] = 0; s_state.y[2] = 0; buttons |= 0x40; }
    else
    {
        s_state.x[2] = ApplySensitivity(axisX[2], sens[2]);
        s_state.y[2] = ApplySensitivity(axisY[2], sens[2]);
        if (trigger[2]) buttons |= 0x40;
    }

    // Player 4  trigger 0x80
    if (reload[3]) { s_state.x[3] = 0; s_state.y[3] = 0; buttons |= 0x80; }
    else
    {
        s_state.x[3] = ApplySensitivity(axisX[3], sens[3]);
        s_state.y[3] = ApplySensitivity(axisY[3], sens[3]);
        if (trigger[3]) buttons |= 0x80;
    }

    s_state.buttons = buttons;
    s_hasData = true;
}

static void HandleClient(SOCKET sock)
{
    BYTE buf[TCP_PACKET_SIZE * 16];
    int buffered = 0;

    while (s_running)
    {
        // 1 ms timeout so we reapply state every tick even with no new data.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        timeval tv = { 0, 1000 };

        if (select(0, &fds, nullptr, nullptr, &tv) > 0)
        {
            int n = recv(sock, (char*)buf + buffered, (int)(sizeof(buf) - buffered), 0);
            if (n <= 0) break;
            buffered += n;

            int processed = 0;
            while (buffered - processed >= TCP_PACKET_SIZE)
            {
                ParsePacket(buf + processed);
                processed += TCP_PACKET_SIZE;
            }
            if (processed > 0)
            {
                int remaining = buffered - processed;
                if (remaining > 0)
                    memmove(buf, buf + processed, remaining);
                buffered = remaining;
            }
        }

        // Reapply last known state to override any TeknoParrot named-pipe
        // writes that zero the position between TCP updates.
        ApplyState();
    }
}

static DWORD WINAPI ServerThread(LPVOID)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { WSACleanup(); return 1; }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    setsockopt(listenSock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)s_port);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listenSock, 1) != 0)
    {
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    while (s_running)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listenSock, &fds);
        timeval tv = { 1, 0 };
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0)
            continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        HandleClient(client);
        closesocket(client);
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}

bool TcpInputServer_IsRunning() { return s_running; }

void TcpInputServer_Start(int port)
{
    if (s_running) return;
    s_port = port;
    s_running = true;
    s_thread = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
}

void TcpInputServer_Stop()
{
    s_running = false;
    if (s_thread)
    {
        WaitForSingleObject(s_thread, 3000);
        CloseHandle(s_thread);
        s_thread = nullptr;
    }
}
