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

// Last parsed state — reapplied every 1 ms to override TeknoParrot named-pipe
// writes (which zero the position on Wine because raw input doesn't work).
struct TcpState
{
    int  x[4], y[4]; // 0-255
    int  buttons;    // trigger/action bits (volume bits excluded)
};
static TcpState  s_state    = {};
static volatile bool s_hasData  = false;

static HANDLE s_thread      = NULL;
static HANDLE s_applyThread = NULL;
static volatile bool s_running = false;
static int s_port = 0;

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

// Dedicated apply thread: reapplies the last TCP state at ~0.1ms intervals
// using QueryPerformanceCounter so Windows timer resolution doesn't limit us.
static DWORD WINAPI ApplyThread(LPVOID)
{
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    // 0.1ms = freq / 10000
    const LONGLONG interval = freq.QuadPart / 10000;

    while (s_running)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart - last.QuadPart >= interval)
        {
            ApplyState();
            last = now;
        }
        // Yield without sleeping so we stay within the 0.1ms window.
        SwitchToThread();
    }
    return 0;
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

    int buttons = 0;

    // Player 1  trigger 0x01  action 0x02
    if (reload[0]) { s_state.x[0] = 0; s_state.y[0] = 0; buttons |= 0x01; }
    else
    {
        s_state.x[0] = (int)(axisX[0] * 255.0f);
        s_state.y[0] = (int)(axisY[0] * 255.0f);
        if (trigger[0]) buttons |= 0x01;
    }
    if (action[0]) buttons |= 0x02;

    // Player 2  trigger 0x04  action 0x08
    if (reload[1]) { s_state.x[1] = 0; s_state.y[1] = 0; buttons |= 0x04; }
    else
    {
        s_state.x[1] = (int)(axisX[1] * 255.0f);
        s_state.y[1] = (int)(axisY[1] * 255.0f);
        if (trigger[1]) buttons |= 0x04;
    }
    if (action[1]) buttons |= 0x08;

    // Player 3  trigger 0x40
    if (reload[2]) { s_state.x[2] = 0; s_state.y[2] = 0; buttons |= 0x40; }
    else
    {
        s_state.x[2] = (int)(axisX[2] * 255.0f);
        s_state.y[2] = (int)(axisY[2] * 255.0f);
        if (trigger[2]) buttons |= 0x40;
    }

    // Player 4  trigger 0x80
    if (reload[3]) { s_state.x[3] = 0; s_state.y[3] = 0; buttons |= 0x80; }
    else
    {
        s_state.x[3] = (int)(axisX[3] * 255.0f);
        s_state.y[3] = (int)(axisY[3] * 255.0f);
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
    s_applyThread = CreateThread(nullptr, 0, ApplyThread, nullptr, 0, nullptr);
    s_thread      = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
}

void TcpInputServer_Stop()
{
    s_running = false;
    if (s_thread)      { WaitForSingleObject(s_thread,      3000); CloseHandle(s_thread);      s_thread      = nullptr; }
    if (s_applyThread) { WaitForSingleObject(s_applyThread, 3000); CloseHandle(s_applyThread); s_applyThread = nullptr; }
}
