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

static HANDLE s_thread = NULL;
static volatile bool s_running = false;
static int s_port = 0;

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

    // Preserve only volume bits (0x10/0x20); rebuild trigger/action bits from TCP.
    int buttons = *ffbOffset & 0x30;

    // Player 1 (gun 0)  trigger bit 0x01, action bit 0x02
    if (reload[0]) { *ffbOffset2 = 0; *ffbOffset3 = 0; buttons |= 0x01; }
    else
    {
        *ffbOffset2 = (int)(axisX[0] * 255.0f);
        *ffbOffset3 = (int)(axisY[0] * 255.0f);
        if (trigger[0]) buttons |= 0x01;
    }
    if (action[0]) buttons |= 0x02;

    // Player 2 (gun 1)  trigger bit 0x04, action bit 0x08
    if (reload[1]) { *ffbOffset4 = 0; *ffbOffset5 = 0; buttons |= 0x04; }
    else
    {
        *ffbOffset4 = (int)(axisX[1] * 255.0f);
        *ffbOffset5 = (int)(axisY[1] * 255.0f);
        if (trigger[1]) buttons |= 0x04;
    }
    if (action[1]) buttons |= 0x08;

    // Player 3 (gun 2)  trigger bit 0x40
    if (reload[2]) { *ffbOffset6 = 0; *ffbOffset7 = 0; buttons |= 0x40; }
    else
    {
        *ffbOffset6 = (int)(axisX[2] * 255.0f);
        *ffbOffset7 = (int)(axisY[2] * 255.0f);
        if (trigger[2]) buttons |= 0x40;
    }

    // Player 4 (gun 3)  trigger bit 0x80
    if (reload[3]) { *ffbOffset8 = 0; *ffbOffset9 = 0; buttons |= 0x80; }
    else
    {
        *ffbOffset8 = (int)(axisX[3] * 255.0f);
        *ffbOffset9 = (int)(axisY[3] * 255.0f);
        if (trigger[3]) buttons |= 0x80;
    }

    *ffbOffset = buttons;
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
