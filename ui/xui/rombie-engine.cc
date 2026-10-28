/*
 * Rombie's Xbox engine mode: xemu driven by one Rombie page over a local
 * WebSocket, with no window. See ../rombie-engine.h for how it's started.
 *
 * Message layout matches the PS2 engine's (pcsx2-rombie Session.h) except
 * boot, which carries the Xbox's two firmware files. Every multi-byte value
 * is little-endian. Page to engine (binary, first byte is the type):
 *   0x01 boot      [u8 type][3 pad][u32 mcpx size][u32 flash size]
 *                  [u64 disc size][mcpx bytes][flash bytes][disc name, UTF-8]
 *   0x02 disc data [u8 type][3 pad][u32 request id][bytes] (empty = failed)
 *   0x03 pad       [u8 type][u8 player][2 pad][u32 buttons][f32 lx][f32 ly]
 *                  [f32 rx][f32 ry][f32 lt][f32 rt]        (player 0-3)
 *   0x04 control   [u8 type][u8 op]          (1 pause, 2 resume, 3 exit)
 *   0x05 ping      [u8 type][7 pad][f64 page time, ms]
 *   0x06 save state   [u8 type][u8 slot]     (1-255; replaces the slot)
 *   0x07 load state   [u8 type][u8 slot]
 *   0x08 game data [u8 type][7 pad][blob]    (before boot; empty = a new
 *                  game, which starts from the blank hard disk)
 *   0x09 speed     [u8 type][3 pad][f32 ratio] (1 = normal; more = faster)
 *   0x0E delete state [u8 type][u8 slot]
 * Engine to page (binary):
 *   0x81 disc read [u8 type][3 pad][u32 request id][u64 offset][u32 length]
 *   0x82 frame     [u8 type][3 pad][u32 frame no][f64 engine time, ms]
 *                  [u16 w][u16 h][u32 vblank no][RGBA]
 *   0x83 audio     [u8 type][3 pad][u32 frames][f32 stereo, 48 kHz]
 *   0x85 game data [u8 type][7 pad][blob]    (a second after the game last
 *                  wrote its hard disk, after every state change, before a
 *                  pause and on exit)
 * Engine to page (text, JSON): hello, event, stats (once a second), pong,
 * and slots: {"t":"slots","slots":[{"slot","engine","date"}]} after boot
 * and after every state change.
 * The game data blob is the game's one stored object: its hard disk image,
 * whose save states live inside it as snapshots, and its EEPROM
 * (rombie-engine-media.h has the layout).
 * Pad buttons: bit i is the Duke's button i in xemu's ControllerState order
 * (A, B, X, Y, d-pad left, up, right, down, Back, Start, White, Black,
 * left stick, right stick). Sticks and triggers run -1..1 and 0..1, with +y
 * down as in the browser's standard gamepad mapping.
 *
 * One game per process: the machine boots when the page's boot message
 * arrives and the process ends when the game stops or the page stays gone.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#endif

#include "../rombie-engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
typedef SOCKET engine_socket;
#define ENGINE_INVALID_SOCKET INVALID_SOCKET
#define engine_closesocket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int engine_socket;
#define ENGINE_INVALID_SOCKET (-1)
#define engine_closesocket close
#endif

#include "common.hh"
#include "gl-helpers.hh"
#include "../xemu-input.h"
#include "../xemu-settings.h"
#include "../xemu-version.h"

extern "C" {
#include "system/cpu-timers.h"
}

#include "../rombie-engine-media.h"
#include "../rombie-engine-version.h"

static constexpr int PROTOCOL_VERSION = 1;
static constexpr uint16_t ENGINE_PORT = 47653;

static constexpr uint8_t MSG_BOOT = 0x01;
static constexpr uint8_t MSG_DISC_DATA = 0x02;
static constexpr uint8_t MSG_PAD = 0x03;
static constexpr uint8_t MSG_CONTROL = 0x04;
static constexpr uint8_t MSG_PING = 0x05;
static constexpr uint8_t MSG_SAVE_STATE = 0x06;
static constexpr uint8_t MSG_LOAD_STATE = 0x07;
static constexpr uint8_t MSG_GAME_DATA = 0x08;
static constexpr uint8_t MSG_SPEED = 0x09;
static constexpr uint8_t MSG_DELETE_STATE = 0x0E;
static constexpr uint8_t MSG_DISC_READ = 0x81;
static constexpr uint8_t MSG_FRAME = 0x82;
static constexpr uint8_t MSG_AUDIO = 0x83;
static constexpr uint8_t MSG_GAME_DATA_SAVED = 0x85;

/* A game usually writes several times in a row when it saves; wait for it
 * to finish before sending the image. */
static constexpr double HDD_SETTLE_MS = 1000.0;

static constexpr uint8_t CONTROL_PAUSE = 1;
static constexpr uint8_t CONTROL_RESUME = 2;
static constexpr uint8_t CONTROL_EXIT = 3;

static constexpr size_t FRAME_HEADER = 24;
static constexpr size_t AUDIO_HEADER = 8;
static constexpr size_t AUDIO_RING_FRAMES = 48000; /* 1 s of 48 kHz stereo */
/* The game data blob carries every save state (~50 MB each) with the disk. */
static constexpr size_t MAX_MESSAGE_SIZE = (size_t)1 << 30;

/* The only pages that may drive the engine. */
static constexpr const char *ALLOWED_ORIGINS[] = {
    "https://rombie.app",
    "https://staged.rombie.app",
    "http://localhost:5173",
    "http://127.0.0.1:5173",
};

/* How long a link-started engine waits for its page before booting, and how
 * long a running game outlives its page, so a reload can reconnect. */
static constexpr double STARTUP_WAIT_S = 60.0;
static constexpr double RECONNECT_GRACE_S = 10.0;

enum class Mode { None, Setup, Unregister, Link, Dev };

static Mode s_mode = Mode::None;
static uint16_t s_port = ENGINE_PORT;
static std::string s_dev_origin;
static std::string s_data_dir;
static std::string s_config_path;

static std::mutex s_key_mutex;
static std::string s_key;

static std::atomic<bool> s_page_connected{false};
static std::atomic<bool> s_session_active{false};
static std::atomic<bool> s_booted{false};
static std::atomic<bool> s_vm_ready{false};

/* Deadline bookkeeping (s_key_mutex): when the engine last lost its page. */
static double s_idle_since_ms;
static double s_idle_limit_s = STARTUP_WAIT_S;

static engine_socket s_listener = ENGINE_INVALID_SOCKET;
static engine_socket s_client = ENGINE_INVALID_SOCKET;
static std::mutex s_send_mutex;

static int SocketError(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

/* The boot the page sent (s_boot_mutex). */
static std::mutex s_boot_mutex;
static std::condition_variable s_boot_cv;
static bool s_boot_ready;
static std::vector<uint8_t> s_mcpx;
static std::vector<uint8_t> s_flash;
static std::vector<uint8_t> s_game_data; /* the stored blob; empty = new */
static bool s_game_data_ok = true;

static bool s_hdd_dirty; /* UI thread: written since last sent */
static double s_hdd_written_ms;
/* After boot, and when a page (re)connects to a running game: send it the
 * slots and the current image. */
static std::atomic<bool> s_resend_media{false};
static void SendGameData(void);
static uint64_t s_disc_size;
static std::string s_disc_name;

/* Control work for the UI thread, run inside the main-loop lock. */
static std::mutex s_work_mutex;
static std::deque<std::function<void()>> s_work;

/* Pads as the page last sent them (s_pad_mutex). */
struct PadState {
    uint32_t buttons;
    float axes[4]; /* lx, ly, rx, ry */
    float triggers[2]; /* lt, rt */
};
static std::mutex s_pad_mutex;
static PadState s_pads[4];

static constexpr float MAX_SPEED = 10.0f;
static std::atomic<double> s_speed{1.0};

/* Pending disc reads (s_disc_mutex). */
struct PendingRead {
    void *dst;
    uint32_t length;
    rombie_disc_read_cb cb;
    void *opaque;
};
static std::mutex s_disc_mutex;
static std::unordered_map<uint32_t, PendingRead> s_reads;
static uint32_t s_next_read_id;

/* Frame handoff: capture writes `back`, swaps it in as `pending`. */
static std::mutex s_frame_mutex;
static std::condition_variable s_wake;
static std::vector<uint8_t> s_frame_back;
static std::vector<uint8_t> s_frame_pending;
static bool s_frame_ready;

/* Audio ring, f32 interleaved stereo. */
static std::mutex s_audio_mutex;
static float s_audio_ring[AUDIO_RING_FRAMES * 2];
static size_t s_audio_read, s_audio_write; /* frame counts, free-running */

struct Stats {
    std::atomic<uint32_t> pad_messages{0};
    std::atomic<uint32_t> guest_polls[4] = {};
    std::atomic<uint32_t> guest_buttons_seen{0}; /* OR of what the guest read */
    std::atomic<uint32_t> vblanks{0};
    std::atomic<uint32_t> frames_sent{0};
    std::atomic<uint32_t> frames_dropped{0};
    std::atomic<uint32_t> audio_frames{0};
    std::atomic<uint32_t> audio_overflow{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> readback_us{0};
    std::atomic<uint32_t> readbacks{0};
    std::atomic<uint32_t> disc_reads{0};
    std::atomic<uint64_t> disc_bytes{0};
    std::atomic<uint64_t> disc_wait_us{0};
    std::atomic<uint32_t> width{0}, height{0};
};
static Stats s_stats;

static double NowMs(void)
{
    return (double)g_get_monotonic_time() / 1000.0;
}

/* Ends the process from anywhere in the engine's own code. Not exit(): that
 * runs the static destructors of xemu's UI, which crash when the UI never
 * started (a --version, setup or no-page run) and would show the player a
 * crash report. */
[[noreturn]] static void QuitNow(int code)
{
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

static void ReportError(const std::string &message)
{
#ifdef _WIN32
    if (s_mode == Mode::Dev) {
        fprintf(stderr, "%s\n", message.c_str());
    } else {
        wchar_t *wide = (wchar_t *)g_utf8_to_utf16(message.c_str(), -1, NULL,
                                                   NULL, NULL);
        MessageBoxW(NULL, wide ? wide : L"Error", L"Rombie Xbox Engine",
                    MB_OK | MB_ICONERROR);
        g_free(wide);
    }
#else
    fprintf(stderr, "%s\n", message.c_str());
#endif
}

static void ReportInfo(const std::string &message)
{
#ifdef _WIN32
    wchar_t *wide =
        (wchar_t *)g_utf8_to_utf16(message.c_str(), -1, NULL, NULL, NULL);
    MessageBoxW(NULL, wide ? wide : L"", L"Rombie Xbox Engine",
                MB_OK | MB_ICONINFORMATION);
    g_free(wide);
#else
    fprintf(stderr, "%s\n", message.c_str());
#endif
}

static std::string CurrentKey(void)
{
    std::unique_lock<std::mutex> lock(s_key_mutex);
    return s_key;
}

static void SetKey(std::string key)
{
    std::unique_lock<std::mutex> lock(s_key_mutex);
    s_key = std::move(key);
}

static void MarkIdle(double limit_s)
{
    std::unique_lock<std::mutex> lock(s_key_mutex);
    s_idle_since_ms = NowMs();
    s_idle_limit_s = limit_s;
}

/* Seconds past the idle deadline; negative while within it. */
static double IdleOverrunS(void)
{
    std::unique_lock<std::mutex> lock(s_key_mutex);
    return (NowMs() - s_idle_since_ms) / 1000.0 - s_idle_limit_s;
}

/* Keys come from Rombie (random, URL-safe). */
static bool IsValidKey(const std::string &key)
{
    if (key.size() < 16 || key.size() > 128) {
        return false;
    }
    for (char ch : key) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_')) {
            return false;
        }
    }
    return true;
}

static std::string QueryParam(const std::string &url, const char *name)
{
    const size_t query = url.find('?');
    if (query == std::string::npos) {
        return "";
    }
    const std::string want = std::string(name) + "=";
    size_t at = query + 1;
    while (at < url.size()) {
        size_t end = url.find('&', at);
        if (end == std::string::npos) {
            end = url.size();
        }
        if (url.compare(at, want.size(), want) == 0) {
            return url.substr(at + want.size(), end - at - want.size());
        }
        at = end + 1;
    }
    return "";
}

static bool IsAllowedOrigin(const std::string &origin)
{
    for (const char *allowed : ALLOWED_ORIGINS) {
        if (origin == allowed) {
            return true;
        }
    }
    return !s_dev_origin.empty() && origin == s_dev_origin;
}

/* --------------------------------------------------------------------------
 * Registration of the rombie-xbox:// link (current user, no admin).
 */

#ifdef _WIN32
static std::wstring ExePath(void)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len =
            GetModuleFileNameW(NULL, path.data(), (DWORD)path.size());
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

static bool RegisterLinkHandler(std::string *error)
{
    const std::wstring exe = ExePath();
    const std::wstring icon = L"\"" + exe + L"\",0";
    const std::wstring command = L"\"" + exe + L"\" --link \"%1\"";
    const std::wstring description = L"URL:Rombie Xbox Engine";

    HKEY key;
    LSTATUS status =
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\rombie-xbox",
                        0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    if (status == ERROR_SUCCESS) {
        const auto set = [key](const wchar_t *subkey, const wchar_t *name,
                               const std::wstring &value) {
            return RegSetKeyValueW(key, subkey, name, REG_SZ, value.c_str(),
                                   (DWORD)((value.size() + 1) *
                                           sizeof(wchar_t)));
        };
        status = set(NULL, NULL, description);
        if (status == ERROR_SUCCESS) {
            status = set(NULL, L"URL Protocol", L"");
        }
        if (status == ERROR_SUCCESS) {
            status = set(L"DefaultIcon", NULL, icon);
        }
        if (status == ERROR_SUCCESS) {
            status = set(L"shell\\open\\command", NULL, command);
        }
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) {
        *error = "Windows refused the change (error " +
                 std::to_string(status) + ").";
        return false;
    }
    return true;
}

static bool UnregisterLinkHandler(void)
{
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER,
                                          L"Software\\Classes\\rombie-xbox");
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}
#endif

/* --------------------------------------------------------------------------
 * WebSocket plumbing, one localhost client (RFC 6455, no extensions).
 */

static bool SendAll(engine_socket sock, const uint8_t *data, size_t size)
{
    while (size > 0) {
        int sent = send(sock, (const char *)data, (int)size, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= (size_t)sent;
    }
    return true;
}

static bool SendMessageParts(uint8_t opcode, const uint8_t *a, size_t a_size,
                             const uint8_t *b, size_t b_size)
{
    std::unique_lock<std::mutex> lock(s_send_mutex);
    if (!s_page_connected.load(std::memory_order_acquire)) {
        return false;
    }

    const uint64_t len = a_size + b_size;
    uint8_t header[10];
    size_t header_size = 2;
    header[0] = 0x80 | opcode; /* FIN, no fragmentation */
    if (len < 126) {
        header[1] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)len;
        header_size = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)(len >> (56 - 8 * i));
        }
        header_size = 10;
    }

    bool ok = SendAll(s_client, header, header_size) &&
              (a_size == 0 || SendAll(s_client, a, a_size)) &&
              (b_size == 0 || SendAll(s_client, b, b_size));
    if (ok) {
        s_stats.bytes_sent.fetch_add(header_size + len,
                                     std::memory_order_relaxed);
    } else {
        s_page_connected.store(false, std::memory_order_release);
    }
    return ok;
}

static bool SendText(const std::string &text)
{
    return SendMessageParts(0x1, (const uint8_t *)text.data(), text.size(),
                            nullptr, 0);
}

static void SendEvent(const char *what, const std::string &detail = "")
{
    std::string escaped;
    for (char ch : detail) {
        if (ch == '"' || ch == '\\') {
            escaped += '\\';
        }
        escaped += ((unsigned char)ch >= 0x20) ? ch : ' ';
    }
    SendText(std::string("{\"t\":\"event\",\"what\":\"") + what +
             "\",\"detail\":\"" + escaped + "\"}");
}

static bool RecvAll(engine_socket sock, uint8_t *data, size_t size)
{
    while (size > 0) {
        int got = recv(sock, (char *)data, (int)size, 0);
        if (got <= 0) {
            return false;
        }
        data += got;
        size -= (size_t)got;
    }
    return true;
}

struct Request {
    std::string path;
    std::string origin;
    bool websocket = false;
    bool handoff = false;
    std::string ws_key;
};

struct Response {
    int status = 403;
    uint16_t close_code = 0;
    std::string close_reason;
};

static std::string HeaderValue(const std::string &request, const char *name)
{
    /* Case-insensitive header lookup, value up to CR. */
    const size_t name_len = strlen(name);
    size_t at = 0;
    while ((at = request.find('\n', at)) != std::string::npos) {
        at++;
        if (request.size() - at < name_len + 1) {
            break;
        }
        if (g_ascii_strncasecmp(request.c_str() + at, name, name_len) == 0 &&
            request[at + name_len] == ':') {
            size_t start = at + name_len + 1;
            while (start < request.size() && request[start] == ' ') {
                start++;
            }
            return request.substr(start, request.find('\r', start) - start);
        }
    }
    return "";
}

static bool ReadRequest(engine_socket sock, Request *request)
{
    std::string raw;
    char ch;
    while (raw.size() < 8192 && raw.find("\r\n\r\n") == std::string::npos) {
        if (recv(sock, &ch, 1, 0) != 1) {
            return false;
        }
        raw.push_back(ch);
    }
    if (raw.compare(0, 4, "GET ") != 0) {
        return false;
    }
    request->path = raw.substr(4, raw.find(' ', 4) - 4);
    request->origin = HeaderValue(raw, "Origin");
    request->ws_key = HeaderValue(raw, "Sec-WebSocket-Key");
    request->websocket = !request->ws_key.empty() &&
                         !HeaderValue(raw, "Upgrade").empty();
    request->handoff = !HeaderValue(raw, "X-Rombie-Handoff").empty();
    return true;
}

static bool SendStatus(engine_socket sock, int status)
{
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " \r\nConnection: close\r\n"
        "Content-Length: 0\r\n\r\n";
    return SendAll(sock, (const uint8_t *)response.data(), response.size());
}

static bool SendUpgrade(engine_socket sock, const std::string &ws_key)
{
    const std::string joined =
        ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(sum, (const guchar *)joined.data(), joined.size());
    uint8_t digest[20];
    gsize digest_len = sizeof(digest);
    g_checksum_get_digest(sum, digest, &digest_len);
    g_checksum_free(sum);
    gchar *accept = g_base64_encode(digest, digest_len);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " +
                                 std::string(accept) + "\r\n\r\n";
    g_free(accept);
    return SendAll(sock, (const uint8_t *)response.data(), response.size());
}

/* Close frame with a code the page's script can read. */
static void SendClose(engine_socket sock, uint16_t code,
                      const std::string &reason)
{
    std::vector<uint8_t> payload(2 + reason.size());
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    memcpy(payload.data() + 2, reason.data(), reason.size());
    uint8_t header[2] = { 0x88, (uint8_t)payload.size() };
    SendAll(sock, header, 2);
    SendAll(sock, payload.data(), payload.size());
}

/* Who may connect: only Rombie's own pages, only with the current key, and
 * only one at a time. Refusals a page should understand complete the
 * handshake and close with a code: 4001 stale key, 4002 another page is
 * playing, 4003 the last page's session is still closing (try again in a
 * moment). A handoff request is a newer engine start handing this one its
 * key (see the PS2 engine's HandOffKey). */
static Response Decide(const Request &request)
{
    if (request.handoff) {
        if (!request.origin.empty()) {
            return { 403 };
        }
        if (s_page_connected.load()) {
            return { 409 };
        }
        const std::string key = QueryParam(request.path, "key");
        if (!IsValidKey(key)) {
            return { 400 };
        }
        SetKey(key);
        MarkIdle(STARTUP_WAIT_S);
        fprintf(stderr, "rombie-engine: took the key from a newer link\n");
        return { 204 };
    }
    if (!request.websocket || !IsAllowedOrigin(request.origin)) {
        fprintf(stderr, "rombie-engine: refused origin '%s'\n",
                request.origin.c_str());
        return { 403 };
    }
    if (s_page_connected.load()) {
        return { 101, 4002, "busy" };
    }
    if (s_session_active.load()) {
        return { 101, 4003, "stopping" };
    }
    if (QueryParam(request.path, "key") != CurrentKey()) {
        fprintf(stderr, "rombie-engine: refused a stale key from '%s'\n",
                request.origin.c_str());
        return { 101, 4001, "key" };
    }
    return { 101 };
}

static void QueueWork(std::function<void()> work)
{
    std::unique_lock<std::mutex> lock(s_work_mutex);
    s_work.push_back(std::move(work));
}

/* --------------------------------------------------------------------------
 * Message handling (session thread).
 */

template <typename T>
static T ReadValue(const std::vector<uint8_t> &data, size_t offset)
{
    T value;
    memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

static void HandleBoot(const std::vector<uint8_t> &data)
{
    if (data.size() < 20) {
        return;
    }
    const uint32_t mcpx_size = ReadValue<uint32_t>(data, 4);
    const uint32_t flash_size = ReadValue<uint32_t>(data, 8);
    const uint64_t disc_size = ReadValue<uint64_t>(data, 12);
    if (mcpx_size != 512) {
        SendEvent("error", "The boot message's MCPX ROM isn't 512 bytes.");
        return;
    }
    if (flash_size == 0 || flash_size % 65536 != 0 ||
        flash_size > 16 * 1024 * 1024 ||
        data.size() < 20 + (size_t)mcpx_size + flash_size) {
        SendEvent("error", "The boot message's flash BIOS is missing or the "
                           "wrong size.");
        return;
    }
    if (disc_size == 0) {
        SendEvent("error", "The boot message's disc size is zero.");
        return;
    }
    if (s_booted.load()) {
        SendEvent("error", "A game is already running in this engine.");
        return;
    }

    std::unique_lock<std::mutex> lock(s_boot_mutex);
    if (!s_game_data_ok) {
        /* Booting on a blank disk instead would hand that back as the
         * game's data, replacing the real one. */
        lock.unlock();
        SendEvent("error", "Boot failed: the game's stored data isn't an "
                           "Xbox engine image.");
        return;
    }
    s_mcpx.assign(data.begin() + 20, data.begin() + 20 + mcpx_size);
    s_flash.assign(data.begin() + 20 + mcpx_size,
                   data.begin() + 20 + mcpx_size + flash_size);
    s_disc_size = disc_size;
    s_disc_name.assign((const char *)data.data() + 20 + mcpx_size + flash_size,
                       data.size() - 20 - mcpx_size - flash_size);
    s_boot_ready = true;
    lock.unlock();
    s_boot_cv.notify_all();
}

static void HandleDiscData(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return;
    }
    const uint32_t id = ReadValue<uint32_t>(data, 4);

    PendingRead read;
    {
        std::unique_lock<std::mutex> lock(s_disc_mutex);
        const auto it = s_reads.find(id);
        if (it == s_reads.end()) {
            return;
        }
        read = it->second;
        s_reads.erase(it);
    }
    const bool ok = data.size() - 8 == read.length;
    if (ok) {
        memcpy(read.dst, data.data() + 8, read.length);
    }
    read.cb(read.opaque, ok);
}

static void FailAllDiscReads(void)
{
    std::unordered_map<uint32_t, PendingRead> reads;
    {
        std::unique_lock<std::mutex> lock(s_disc_mutex);
        reads.swap(s_reads);
    }
    for (auto &entry : reads) {
        entry.second.cb(entry.second.opaque, false);
    }
}

static float ClampUnit(float v, float lo)
{
    return (v != v) ? 0.0f : (v < lo ? lo : (v > 1.0f ? 1.0f : v));
}

static void HandlePad(const std::vector<uint8_t> &data)
{
    if (data.size() < 32 || data[1] >= 4) {
        return;
    }
    PadState pad;
    pad.buttons = ReadValue<uint32_t>(data, 4);
    for (int i = 0; i < 4; i++) {
        pad.axes[i] = ClampUnit(ReadValue<float>(data, 8 + i * 4), -1.0f);
    }
    for (int i = 0; i < 2; i++) {
        pad.triggers[i] = ClampUnit(ReadValue<float>(data, 24 + i * 4), 0.0f);
    }
    s_stats.pad_messages.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(s_pad_mutex);
    s_pads[data[1]] = pad;
}

static void HandleControl(const std::vector<uint8_t> &data)
{
    if (data.size() < 2) {
        return;
    }
    const uint8_t op = data[1];
    QueueWork([op]() {
        if (!s_vm_ready.load()) {
            return;
        }
        if (op == CONTROL_PAUSE && runstate_is_running()) {
            /* A save written just before pausing shouldn't wait for the
             * game to resume. */
            if (s_hdd_dirty) {
                SendGameData();
            }
            vm_stop(RUN_STATE_PAUSED);
            SendEvent("paused");
        } else if (op == CONTROL_RESUME && !runstate_is_running()) {
            vm_start();
            SendEvent("resumed");
        } else if (op == CONTROL_EXIT) {
            if (s_booted.load()) {
                SendGameData();
            }
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        }
    });
}

static void HandlePing(const std::vector<uint8_t> &data)
{
    if (data.size() < 16) {
        return;
    }
    const double page_ms = ReadValue<double>(data, 8);
    char pong[128];
    snprintf(pong, sizeof(pong),
             "{\"t\":\"pong\",\"page\":%.3f,\"engine\":%.3f}", page_ms,
             NowMs());
    SendText(pong);
}

static void HandleSpeed(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return;
    }
    const float ratio = ReadValue<float>(data, 4);
    const double speed = ratio > 1.0f ? (ratio < MAX_SPEED ? ratio : MAX_SPEED)
                                      : 1.0; /* NaN too */
    QueueWork([speed]() {
        /* The guest's clock, its vblanks and its audio frames all speed up
         * together (the latter two read rombie_engine_speed()). */
        cpu_set_vm_speed(speed);
        s_speed.store(speed);
        SendEvent("speed", std::to_string(speed));
    });
}

/* --------------------------------------------------------------------------
 * The game's writable media: its hard disk image (with the save states
 * inside) and EEPROM, one blob the page stores per game.
 */

struct GameDataView {
    const uint8_t *eeprom;
    size_t eeprom_size;
    const uint8_t *hdd;
    size_t hdd_size;
};

static bool ParseGameData(const std::vector<uint8_t> &blob, GameDataView *view)
{
    if (blob.size() < 20 || memcmp(blob.data(), "RXBX", 4) != 0 ||
        ReadValue<uint32_t>(blob, 4) != 1) {
        return false;
    }
    const uint32_t eeprom_size = ReadValue<uint32_t>(blob, 8);
    const uint64_t hdd_size = ReadValue<uint64_t>(blob, 12);
    if (eeprom_size > 4096 || hdd_size < 4 ||
        20 + (uint64_t)eeprom_size + hdd_size != blob.size()) {
        return false;
    }
    view->eeprom = blob.data() + 20;
    view->eeprom_size = eeprom_size;
    view->hdd = blob.data() + 20 + eeprom_size;
    view->hdd_size = (size_t)hdd_size;
    return memcmp(view->hdd, "QFI\xfb", 4) == 0; /* a qcow2 image */
}

/* Main thread, before QEMU starts: the stored disk and EEPROM become the
 * files xemu opens. */
static bool WriteGameData(const std::vector<uint8_t> &blob)
{
    GameDataView view;
    if (!ParseGameData(blob, &view)) {
        return false;
    }
    if (view.eeprom_size == 0) {
        g_remove(g_config.sys.files.eeprom_path);
    } else if (!g_file_set_contents(g_config.sys.files.eeprom_path,
                                    (const gchar *)view.eeprom,
                                    (gssize)view.eeprom_size, NULL)) {
        return false;
    }
    return g_file_set_contents(g_config.sys.files.hdd_path,
                               (const gchar *)view.hdd, (gssize)view.hdd_size,
                               NULL);
}

static void HandleGameData(const std::vector<uint8_t> &data)
{
    if (data.size() < 8 || s_booted.load()) {
        return;
    }
    std::unique_lock<std::mutex> lock(s_boot_mutex);
    s_game_data.assign(data.begin() + 8, data.end());
    GameDataView view;
    s_game_data_ok = s_game_data.empty() || ParseGameData(s_game_data, &view);
}

/* UI thread: hands the page the game's blob as it is now. */
static void SendGameData(void)
{
    s_hdd_dirty = false;
    uint8_t *blob = nullptr;
    size_t size = 0;
    char *error = nullptr;
    if (!rombie_media_read_blob(&blob, &size, &error)) {
        SendEvent("error", error ? error : "Reading the game's data failed.");
        g_free(error);
        return;
    }
    uint8_t header[8] = { MSG_GAME_DATA_SAVED };
    SendMessageParts(0x2, header, sizeof(header), blob, size);
    g_free(blob);
}

static void SendSlots(void)
{
    gchar *json = rombie_media_slots_json();
    SendText(std::string("{\"t\":\"slots\",\"slots\":") + json + "}");
    g_free(json);
}

static void HandleState(const std::vector<uint8_t> &data)
{
    if (data.size() < 2 || data[1] == 0) {
        return;
    }
    const uint8_t op = data[0];
    const int slot = data[1];
    QueueWork([op, slot]() {
        if (!s_vm_ready.load() || !s_booted.load()) {
            return;
        }
        char *error = nullptr;
        bool ok;
        const char *done;
        if (op == MSG_SAVE_STATE) {
            ok = rombie_media_save_state(slot, ROMBIE_XBOX_ENGINE_VERSION,
                                         &error);
            done = "state-saved";
        } else if (op == MSG_LOAD_STATE) {
            ok = rombie_media_load_state(slot, ROMBIE_XBOX_ENGINE_VERSION,
                                         &error);
            done = "state-loaded";
        } else {
            ok = rombie_media_delete_state(slot, &error);
            done = "state-deleted";
        }

        if (ok) {
            SendEvent(done, std::to_string(slot));
        } else if (error && g_str_has_prefix(error, "version:")) {
            /* Made on another engine version: QEMU can't load it. */
            SendEvent("state-version",
                      std::to_string(slot) + " " + (error + 8));
        } else {
            SendEvent("error", error ? error : "The save state failed.");
        }
        g_free(error);

        SendSlots();
        if (ok) {
            /* The image changed: a save or delete rewrote its snapshots, a
             * load put its disk back to the state's. */
            SendGameData();
        }
    });
}

static void HandleBinary(const std::vector<uint8_t> &message)
{
    switch (message[0]) {
    case MSG_BOOT:
        HandleBoot(message);
        break;
    case MSG_GAME_DATA:
        HandleGameData(message);
        break;
    case MSG_SAVE_STATE:
    case MSG_LOAD_STATE:
    case MSG_DELETE_STATE:
        HandleState(message);
        break;
    case MSG_DISC_DATA:
        HandleDiscData(message);
        break;
    case MSG_PAD:
        HandlePad(message);
        break;
    case MSG_CONTROL:
        HandleControl(message);
        break;
    case MSG_PING:
        HandlePing(message);
        break;
    case MSG_SPEED:
        HandleSpeed(message);
        break;
    default:
        break;
    }
}

/* Serve one client until it goes. Browsers split big messages (a boot with
 * its firmware) into continuation frames, so data frames are collected until
 * the one marked final; control frames may arrive in between. */
static void ReadClient(engine_socket sock)
{
    std::vector<uint8_t> payload;
    std::vector<uint8_t> message;
    uint8_t message_opcode = 0;
    for (;;) {
        uint8_t head[2];
        if (!RecvAll(sock, head, 2)) {
            return;
        }
        const bool fin = (head[0] & 0x80) != 0;
        const uint8_t opcode = head[0] & 0x0F;
        const bool masked = (head[1] & 0x80) != 0;
        uint64_t len = head[1] & 0x7F;
        if (len == 126) {
            uint8_t ext[2];
            if (!RecvAll(sock, ext, 2)) {
                return;
            }
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!RecvAll(sock, ext, 8)) {
                return;
            }
            len = 0;
            for (int i = 0; i < 8; i++) {
                len = (len << 8) | ext[i];
            }
        }
        uint8_t mask[4] = {};
        if (masked && !RecvAll(sock, mask, 4)) {
            return;
        }
        if (len > MAX_MESSAGE_SIZE) {
            return;
        }
        payload.resize(len);
        if (len > 0 && !RecvAll(sock, payload.data(), len)) {
            return;
        }
        if (masked) {
            for (size_t i = 0; i < payload.size(); i++) {
                payload[i] ^= mask[i & 3];
            }
        }

        switch (opcode) {
        case 0x0: /* continuation */
        case 0x1:
        case 0x2:
            if (opcode != 0x0) {
                message_opcode = opcode;
                message.clear();
            }
            if (message.size() + payload.size() > MAX_MESSAGE_SIZE) {
                return;
            }
            message.insert(message.end(), payload.begin(), payload.end());
            if (fin) {
                if (message_opcode == 0x2 && !message.empty()) {
                    HandleBinary(message);
                }
                message.clear();
            }
            break;
        case 0x8:
            return;
        case 0x9:
            SendMessageParts(0xA, payload.data(), payload.size(), nullptr, 0);
            break;
        default:
            break;
        }
    }
}

/* One page, from its first message to its last. Runs beside the accept loop
 * so a second page can be turned away while this one plays. */
static void SessionThread(engine_socket sock, std::string origin)
{
    char hello[256];
    snprintf(hello, sizeof(hello),
             "{\"t\":\"hello\",\"engine\":\"xemu-rombie\",\"version\":"
             "\"%s\",\"protocol\":%d,\"xemu\":\"%s\"}",
             ROMBIE_XBOX_ENGINE_VERSION, PROTOCOL_VERSION, xemu_version);
    SendText(hello);
    fprintf(stderr, "rombie-engine: page connected from '%s'\n",
            origin.c_str());
    if (s_booted.load()) {
        SendEvent("booted", s_disc_name);
        s_resend_media.store(true);
    }

    ReadClient(sock);

    s_page_connected.store(false, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(s_send_mutex);
        engine_closesocket(sock);
        if (s_client == sock) {
            s_client = ENGINE_INVALID_SOCKET;
        }
    }
    FailAllDiscReads();
    {
        std::unique_lock<std::mutex> lock(s_pad_mutex);
        memset(s_pads, 0, sizeof(s_pads));
    }
    /* A page that comes back (a reload) starts at normal speed. */
    if (s_speed.load() != 1.0) {
        QueueWork([]() {
            cpu_set_vm_speed(1.0);
            s_speed.store(1.0);
        });
    }
    MarkIdle(s_booted.load() ? RECONNECT_GRACE_S : STARTUP_WAIT_S);
    s_session_active.store(false);
    fprintf(stderr, "rombie-engine: page disconnected\n");
}

/* Accepts on the socket Listen() opened, for the life of the process. */
static void ServerThread(void)
{
    for (;;) {
        engine_socket sock = accept(s_listener, nullptr, nullptr);
        if (sock == ENGINE_INVALID_SOCKET) {
            break;
        }
        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
                   sizeof(one));

        Request request;
        if (!ReadRequest(sock, &request)) {
            engine_closesocket(sock);
            continue;
        }
        const Response response = Decide(request);
        if (response.status != 101) {
            SendStatus(sock, response.status);
            engine_closesocket(sock);
            continue;
        }
        if (!SendUpgrade(sock, request.ws_key)) {
            engine_closesocket(sock);
            continue;
        }
        if (response.close_code != 0) {
            SendClose(sock, response.close_code, response.close_reason);
            engine_closesocket(sock);
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(s_send_mutex);
            s_client = sock;
        }
        s_session_active.store(true);
        s_page_connected.store(true, std::memory_order_release);
        std::thread(SessionThread, sock, request.origin).detach();
    }
}

/* --------------------------------------------------------------------------
 * Sender thread: frames, audio and stats, so a slow page never blocks the
 * UI or APU threads.
 */

static void SendPendingAudio(std::vector<uint8_t> &message)
{
    size_t frames;
    {
        std::unique_lock<std::mutex> lock(s_audio_mutex);
        frames = s_audio_write - s_audio_read;
        if (frames < 256) {
            return;
        }
        message.resize(AUDIO_HEADER + frames * 2 * sizeof(float));
        float *out = (float *)&message[AUDIO_HEADER];
        for (size_t i = 0; i < frames; i++) {
            const size_t slot = ((s_audio_read + i) % AUDIO_RING_FRAMES) * 2;
            *out++ = s_audio_ring[slot];
            *out++ = s_audio_ring[slot + 1];
        }
        s_audio_read += frames;
    }
    message[0] = MSG_AUDIO;
    memset(&message[1], 0, 3);
    const uint32_t count = (uint32_t)frames;
    memcpy(&message[4], &count, sizeof(count));
    SendMessageParts(0x2, message.data(), message.size(), nullptr, 0);
    s_stats.audio_frames.fetch_add(count, std::memory_order_relaxed);
}

static void SenderThread(void)
{
    std::vector<uint8_t> frame;
    std::vector<uint8_t> audio;
    double last_stats_ms = NowMs();

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s_frame_mutex);
            s_wake.wait_for(lock, std::chrono::milliseconds(5));
            if (s_frame_ready) {
                frame.swap(s_frame_pending);
                s_frame_ready = false;
            }
        }

        if (!s_page_connected.load(std::memory_order_acquire)) {
            frame.clear();
            continue;
        }

        SendPendingAudio(audio);

        if (!frame.empty()) {
            if (SendMessageParts(0x2, frame.data(), frame.size(), nullptr,
                                 0)) {
                s_stats.frames_sent.fetch_add(1, std::memory_order_relaxed);
            }
            frame.clear();
        }

        const double now = NowMs();
        if (now - last_stats_ms >= 1000.0) {
            const double seconds = (now - last_stats_ms) / 1000.0;
            last_stats_ms = now;
            const uint32_t vblanks = s_stats.vblanks.exchange(0);
            const uint32_t pad_messages = s_stats.pad_messages.exchange(0);
            uint32_t polls[4];
            for (int i = 0; i < 4; i++) {
                polls[i] = s_stats.guest_polls[i].exchange(0);
            }
            const uint32_t guest_seen = s_stats.guest_buttons_seen.exchange(0);
            const uint32_t sent = s_stats.frames_sent.exchange(0);
            const uint32_t dropped = s_stats.frames_dropped.exchange(0);
            const uint32_t audio_frames = s_stats.audio_frames.exchange(0);
            const uint32_t overflow = s_stats.audio_overflow.exchange(0);
            const uint64_t bytes = s_stats.bytes_sent.exchange(0);
            const uint64_t readback_us = s_stats.readback_us.exchange(0);
            const uint32_t readbacks = s_stats.readbacks.exchange(0);
            const uint32_t reads = s_stats.disc_reads.exchange(0);
            const uint64_t read_bytes = s_stats.disc_bytes.exchange(0);
            const uint64_t read_us = s_stats.disc_wait_us.exchange(0);
            char stats[512];
            snprintf(stats, sizeof(stats),
                     "{\"t\":\"stats\",\"vbl\":%.1f,\"speed\":%.1f,"
                     "\"padMsgs\":%u,"
                     "\"guestPolls\":[%u,%u,%u,%u],\"guestSeen\":%u,"
                     "\"fps\":%.1f,\"mbps\":%.1f,"
                     "\"readback_us\":%u,\"drop\":%u,\"audio\":%u,"
                     "\"overflow\":%u,\"dr\":%u,\"drBytes\":%llu,"
                     "\"drMs\":%.2f,\"w\":%u,\"h\":%u}",
                     vblanks / seconds, s_speed.load(),
                     pad_messages,
                     polls[0], polls[1], polls[2], polls[3], guest_seen,
                     sent / seconds, bytes / seconds / 1e6,
                     readbacks ? (unsigned)(readback_us / readbacks) : 0,
                     dropped, audio_frames, overflow, reads,
                     (unsigned long long)read_bytes,
                     reads ? read_us / 1000.0 / reads : 0.0,
                     s_stats.width.load(), s_stats.height.load());
            SendText(stats);
        }
    }
}

/* --------------------------------------------------------------------------
 * Launch modes, data folder, and the boot wait.
 */

bool rombie_engine_enabled(void)
{
    return s_mode == Mode::Link || s_mode == Mode::Dev;
}

bool rombie_engine_is_link_launch(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--link") == 0) {
            return true;
        }
    }
    return false;
}

const char *rombie_engine_config_path(void)
{
    return s_config_path.c_str();
}

static std::string Usage(void)
{
    return std::string("Rombie Xbox engine ") + ROMBIE_XBOX_ENGINE_VERSION +
           " (xemu " + xemu_version + ")\n\n"
           "Run it once with no arguments to set it up; Rombie starts it "
           "from then on.\n\n"
           "  --unregister     remove the rombie-xbox:// link registration\n"
           "  --link <url>     (used by the link) serve the page that opened "
           "rombie-xbox://start?key=...\n"
           "  --key <key>      development: serve pages presenting <key>\n"
           "    --port <port>  listen on 127.0.0.1:<port> (default " +
           std::to_string(ENGINE_PORT) + ")\n"
           "    --origin <o>   also serve pages from origin <o>\n"
           "  --version        print the version";
}

void rombie_engine_early_init(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) {
            continue;
        }
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc && argv[i + 1];
        if (arg == "--version") {
            printf("%s\n", ROMBIE_XBOX_ENGINE_VERSION);
            QuitNow(EXIT_SUCCESS);
        } else if (arg == "--unregister") {
            s_mode = Mode::Unregister;
            argv[i] = NULL;
        } else if (arg == "--setup") {
            s_mode = Mode::Setup;
            argv[i] = NULL;
        } else if (arg == "--link" && has_value) {
            s_mode = Mode::Link;
            const std::string key = QueryParam(argv[i + 1], "key");
            argv[i] = argv[i + 1] = NULL;
            i++;
            if (!IsValidKey(key)) {
                ReportError("Rombie sent the Xbox engine a start link it "
                            "doesn't understand. Update the Xbox engine, or "
                            "start the game again.");
                QuitNow(EXIT_FAILURE);
            }
            SetKey(key);
        } else if (arg == "--key" && has_value) {
            s_mode = Mode::Dev;
            SetKey(argv[i + 1]);
            argv[i] = argv[i + 1] = NULL;
            i++;
        } else if (arg == "--port" && has_value) {
            const long port = strtol(argv[i + 1], NULL, 10);
            argv[i] = argv[i + 1] = NULL;
            i++;
            if (port <= 0 || port > 0xFFFF) {
                ReportError("Invalid port.");
                QuitNow(EXIT_FAILURE);
            }
            s_port = (uint16_t)port;
        } else if (arg == "--origin" && has_value) {
            s_dev_origin = argv[i + 1];
            argv[i] = argv[i + 1] = NULL;
            i++;
        }
    }

#ifndef _WIN32
    if (s_mode != Mode::None) {
        fprintf(stderr, "The Rombie Xbox engine is Windows-only.\n");
        QuitNow(EXIT_FAILURE);
    }
#else
    /* No arguments at all is the setup run (the PS2 engine convention). */
    if (argc <= 1) {
        s_mode = Mode::Setup;
    }

    if (s_mode == Mode::Dev && CurrentKey().size() < 8) {
        ReportError("A --key of at least 8 characters is required.\n\n" +
                    Usage());
        QuitNow(EXIT_FAILURE);
    }
    /* The link always uses the one port Rombie knows, and only Rombie's
     * own origins. */
    if (s_mode == Mode::Link) {
        s_port = ENGINE_PORT;
        s_dev_origin.clear();
    }

    if (s_mode == Mode::Setup) {
        std::string error;
        if (!RegisterLinkHandler(&error)) {
            ReportError("The Xbox engine couldn't set itself up: " + error);
            QuitNow(EXIT_FAILURE);
        }
        ReportInfo("The Xbox engine is set up.\n\n"
                   "You don't need to start it yourself: Rombie starts it "
                   "whenever you play an Xbox game. If you move this folder, "
                   "run the engine once more from its new place.");
        QuitNow(EXIT_SUCCESS);
    }
    if (s_mode == Mode::Unregister) {
        if (!UnregisterLinkHandler()) {
            ReportError("The Xbox engine's link couldn't be removed.");
            QuitNow(EXIT_FAILURE);
        }
        ReportInfo("The Xbox engine's link is removed. You can delete its "
                   "folder now.");
        QuitNow(EXIT_SUCCESS);
    }
    if (s_mode == Mode::None) {
        return;
    }

    /* A private data folder, so the engine never shares settings or images
     * with an xemu the user may have installed themselves. */
    PWSTR local_app_data = NULL;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL,
                                    &local_app_data))) {
        ReportError("Can't find this user's local app data folder.");
        QuitNow(EXIT_FAILURE);
    }
    gchar *base = g_utf16_to_utf8((const gunichar2 *)local_app_data, -1, NULL,
                                  NULL, NULL);
    CoTaskMemFree(local_app_data);
    s_data_dir = std::string(base) + "\\RombieXbox";
    g_free(base);
    if (s_mode == Mode::Dev && s_port != ENGINE_PORT) {
        s_data_dir += "-" + std::to_string(s_port);
    }
    g_mkdir_with_parents(s_data_dir.c_str(), 0700);
    s_config_path = s_data_dir + "\\xemu.toml";

    if (s_mode == Mode::Link) {
        /* One log per run, overwritten by the next, beside the engine's other
         * files: the first place to look when a player reports a problem. */
        const std::string log = s_data_dir + "\\engine.log";
        if (freopen(log.c_str(), "w", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
        }
        freopen(log.c_str(), "a", stdout);
        fprintf(stderr, "Rombie Xbox engine %s (xemu %s)\n",
                ROMBIE_XBOX_ENGINE_VERSION, xemu_version);
    }
#endif
}

/* Another engine already holds the port. If it has no page, it takes this
 * link's key over a plain HTTP request a browser can't forge (the custom
 * header would need a CORS preflight, which the engine never answers). */
static bool TryHandOffKey(void)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    bool handled = false;

    engine_socket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock != ENGINE_INVALID_SOCKET) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
        DWORD timeout_ms = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                   sizeof(timeout_ms));
#endif
        if (connect(sock, (const sockaddr *)&addr, sizeof(addr)) == 0) {
            const std::string request =
                "GET /handoff?key=" + CurrentKey() +
                " HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Rombie-Handoff: 1\r\n"
                "Connection: close\r\n\r\n";
            char reply[64] = {};
            if (SendAll(sock, (const uint8_t *)request.data(),
                        request.size()) &&
                recv(sock, reply, sizeof(reply) - 1, 0) > 0) {
                /* 204: an idle engine took the key. 409: it's busy and the
                 * page hears "busy" from it. Either way this process is
                 * done. */
                handled = strncmp(reply, "HTTP/1.1 204", 12) == 0 ||
                          strncmp(reply, "HTTP/1.1 409", 12) == 0;
            }
        }
        engine_closesocket(sock);
    }
    return handled;
}

/* Binds and listens on 127.0.0.1 only. On failure, *error is the socket
 * error (WSAGetLastError on Windows). */
static bool Listen(int *error)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    engine_socket sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == ENGINE_INVALID_SOCKET) {
        *error = SocketError();
        return false;
    }
#ifdef _WIN32
    /* Nobody else may share the port, not even a socket that asked to. */
    BOOL exclusive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive,
               sizeof(exclusive));
#endif
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (const sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(sock, 4) != 0) {
        *error = SocketError();
        engine_closesocket(sock);
        return false;
    }
    s_listener = sock;
    return true;
}

void rombie_engine_wait_for_boot(void)
{
    if (!rombie_engine_enabled()) {
        return;
    }

    /* The page is the only input, display and speaker. */
    xemu_settings_set_string(&g_config.sys.files.bootrom_path, "");
    xemu_settings_set_string(&g_config.sys.files.flashrom_path, "");
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");
    const std::string eeprom = s_data_dir + "\\eeprom.bin";
    xemu_settings_set_string(&g_config.sys.files.eeprom_path, eeprom.c_str());
    const std::string hdd = s_data_dir + "\\hdd-scratch.qcow2";
    xemu_settings_set_string(&g_config.sys.files.hdd_path, hdd.c_str());
    g_config.general.show_welcome = false;

    int listen_error = 0;
    if (!Listen(&listen_error)) {
        /* An engine of ours already holds the port: an idle one takes this
         * link's key and serves the page; a busy one tells the page so.
         * Either way this process is done. */
        if (s_mode == Mode::Link && TryHandOffKey()) {
            QuitNow(EXIT_SUCCESS);
        }
        fprintf(stderr, "rombie-engine: can't listen on 127.0.0.1:%u "
                        "(socket error %d)\n",
                (unsigned)s_port, listen_error);
        ReportError("The Xbox engine needs port " + std::to_string(s_port) +
                    " on this computer, but another program is using it. "
                    "Close that program, then start the game in Rombie "
                    "again.");
        QuitNow(2);
    }
    fprintf(stderr,
            "rombie-engine: %s listening on 127.0.0.1:%u (protocol %d)\n",
            ROMBIE_XBOX_ENGINE_VERSION, (unsigned)s_port, PROTOCOL_VERSION);

    std::thread(ServerThread).detach();
    std::thread(SenderThread).detach();
    MarkIdle(STARTUP_WAIT_S);

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s_boot_mutex);
            s_boot_cv.wait_for(lock, std::chrono::milliseconds(50));
            if (s_boot_ready) {
                break;
            }
        }
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        if (s_mode == Mode::Link && !s_session_active.load() &&
            IdleOverrunS() > 0.0) {
            fprintf(stderr, "rombie-engine: no page; exiting\n");
            QuitNow(EXIT_SUCCESS);
        }
    }

    /* The game's own hard disk and EEPROM, as the page last stored them; a
     * new game starts from the bundled blank disk and a fresh EEPROM, which
     * xemu generates when the file is missing. */
    if (!s_game_data.empty()) {
        if (!WriteGameData(s_game_data)) {
            SendEvent("error", "Boot failed: the game's stored data couldn't "
                               "be written to the engine's folder.");
            QuitNow(EXIT_FAILURE);
        }
        s_game_data = {};
    } else {
        g_remove(g_config.sys.files.eeprom_path);
#ifdef _WIN32
        std::wstring exe = ExePath();
        const size_t slash = exe.find_last_of(L"\\/");
        const std::wstring base_image =
            exe.substr(0, slash + 1) + L"xbox_hdd.qcow2";
        wchar_t *hdd_wide = (wchar_t *)g_utf8_to_utf16(
            g_config.sys.files.hdd_path, -1, NULL, NULL, NULL);
        const bool copied =
            CopyFileW(base_image.c_str(), hdd_wide, FALSE) != 0;
        g_free(hdd_wide);
        if (!copied) {
            ReportError("The engine's blank hard disk image (xbox_hdd.qcow2) "
                        "is missing. Unzip the whole download again.");
            QuitNow(EXIT_FAILURE);
        }
#endif
    }

    s_booted.store(true);
    s_resend_media.store(true);
    fprintf(stderr, "rombie-engine: booting '%s' (%llu bytes)\n",
            s_disc_name.c_str(), (unsigned long long)s_disc_size);
    SendEvent("booted", s_disc_name);
}

void rombie_engine_run_pending(void)
{
    if (!rombie_engine_enabled()) {
        return;
    }
    if (!s_vm_ready.load()) {
        s_vm_ready.store(true);
    }

    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(s_work_mutex);
            if (s_work.empty()) {
                break;
            }
            work = std::move(s_work.front());
            s_work.pop_front();
        }
        work();
    }

    if (s_booted.load()) {
        if (s_resend_media.exchange(false)) {
            SendSlots();
            SendGameData();
        }
        if (rombie_media_hdd_written()) {
            s_hdd_dirty = true;
            s_hdd_written_ms = NowMs();
        }
        if (s_hdd_dirty && NowMs() - s_hdd_written_ms >= HDD_SETTLE_MS) {
            SendGameData();
        }
    }

    /* A running game whose page stays gone has nobody to play it. */
    if (s_booted.load() && !s_session_active.load() &&
        IdleOverrunS() > 0.0) {
        fprintf(stderr, "rombie-engine: page gone; shutting down\n");
        MarkIdle(STARTUP_WAIT_S); /* don't request again each frame */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
    }
}

/* --------------------------------------------------------------------------
 * The firmware and disc the page supplied.
 */

bool rombie_engine_get_flash(const uint8_t **data, uint32_t *size)
{
    if (!rombie_engine_enabled() || s_flash.empty()) {
        return false;
    }
    *data = s_flash.data();
    *size = (uint32_t)s_flash.size();
    return true;
}

bool rombie_engine_get_mcpx(const uint8_t **data, uint32_t *size)
{
    if (!rombie_engine_enabled() || s_mcpx.empty()) {
        return false;
    }
    *data = s_mcpx.data();
    *size = (uint32_t)s_mcpx.size();
    return true;
}

uint64_t rombie_engine_disc_size(void)
{
    return s_disc_size;
}

void rombie_engine_disc_read(uint64_t offset, uint32_t length, void *dst,
                             rombie_disc_read_cb cb, void *opaque)
{
    uint32_t id;
    {
        std::unique_lock<std::mutex> lock(s_disc_mutex);
        if (!s_page_connected.load(std::memory_order_acquire)) {
            lock.unlock();
            cb(opaque, false);
            return;
        }
        id = ++s_next_read_id;
        s_reads.emplace(id, PendingRead{ dst, length, cb, opaque });
    }

    uint8_t request[20] = { MSG_DISC_READ };
    memcpy(&request[4], &id, sizeof(id));
    memcpy(&request[8], &offset, sizeof(offset));
    memcpy(&request[16], &length, sizeof(length));
    if (!SendMessageParts(0x2, request, sizeof(request), nullptr, 0)) {
        std::unique_lock<std::mutex> lock(s_disc_mutex);
        if (s_reads.erase(id)) {
            lock.unlock();
            cb(opaque, false);
        }
        return;
    }
    s_stats.disc_reads.fetch_add(1, std::memory_order_relaxed);
    s_stats.disc_bytes.fetch_add(length, std::memory_order_relaxed);
}

/* --------------------------------------------------------------------------
 * Frames and audio (same capture as the Phase 2 spike).
 */

void rombie_engine_capture(uint32_t tex, bool flip)
{
    if (!rombie_engine_enabled() ||
        !s_page_connected.load(std::memory_order_acquire) || tex == 0) {
        return;
    }
    const double start_ms = NowMs();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    int width = 0, height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return;
    }

    /* Rendered into a same-size FBO exactly as RenderFramebufferToPng does,
     * with the flip inverted so the rows read back top-down. */
    static Fbo *fbo;
    static int fbo_width, fbo_height;
    if (!fbo || fbo_width != width || fbo_height != height) {
        delete fbo;
        fbo = new Fbo(width, height);
        fbo_width = width;
        fbo_height = height;
    }

    fbo->Target();
    const bool blend = glIsEnabled(GL_BLEND);
    if (blend) {
        glDisable(GL_BLEND);
    }
    float scale[2] = { 1.0, 1.0 };
    RenderFramebuffer(tex, width, height, !flip, scale);
    if (blend) {
        glEnable(GL_BLEND);
    }

    const size_t pixel_bytes = (size_t)width * height * 4;
    s_frame_back.resize(FRAME_HEADER + pixel_bytes);
    glPixelStorei(GL_PACK_ROW_LENGTH, width);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                 &s_frame_back[FRAME_HEADER]);
    fbo->Restore();

    /* The Xbox framebuffer's alpha isn't display opacity. */
    uint32_t *pixels = (uint32_t *)&s_frame_back[FRAME_HEADER];
    for (size_t i = 0; i < (size_t)width * height; i++) {
        pixels[i] |= 0xFF000000u;
    }

    static uint32_t frame_no;
    frame_no++;
    const double ready_ms = NowMs();
    const uint16_t w16 = (uint16_t)width, h16 = (uint16_t)height;
    s_frame_back[0] = MSG_FRAME;
    memset(&s_frame_back[1], 0, 3);
    memcpy(&s_frame_back[4], &frame_no, sizeof(frame_no));
    memcpy(&s_frame_back[8], &ready_ms, sizeof(ready_ms));
    memcpy(&s_frame_back[16], &w16, sizeof(w16));
    memcpy(&s_frame_back[18], &h16, sizeof(h16));
    memcpy(&s_frame_back[20], &frame_no, sizeof(frame_no)); /* vblank no */

    {
        std::unique_lock<std::mutex> lock(s_frame_mutex);
        if (s_frame_ready) {
            s_stats.frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        s_frame_pending.swap(s_frame_back);
        s_frame_ready = true;
    }
    s_wake.notify_one();

    s_stats.width.store((uint32_t)width, std::memory_order_relaxed);
    s_stats.height.store((uint32_t)height, std::memory_order_relaxed);
    s_stats.readback_us.fetch_add((uint64_t)((NowMs() - start_ms) * 1000.0),
                                  std::memory_order_relaxed);
    s_stats.readbacks.fetch_add(1, std::memory_order_relaxed);
}

void rombie_engine_submit_audio(const int16_t frames[][2], int count)
{
    if (!rombie_engine_enabled() ||
        !s_page_connected.load(std::memory_order_acquire)) {
        return;
    }

    /* Fast-forward makes `speed` times the sound per second; the page plays
     * one second's worth, so only that share of the blocks goes to it. */
    static double credit = 1.0;
    credit += 1.0 / s_speed.load();
    if (credit < 1.0) {
        return;
    }
    credit = credit > 2.0 ? 1.0 : credit - 1.0;

    std::unique_lock<std::mutex> lock(s_audio_mutex);
    for (int i = 0; i < count; i++) {
        if (s_audio_write - s_audio_read >= AUDIO_RING_FRAMES) {
            s_stats.audio_overflow.fetch_add(count - i,
                                             std::memory_order_relaxed);
            break;
        }
        const size_t slot = (s_audio_write % AUDIO_RING_FRAMES) * 2;
        s_audio_ring[slot] = frames[i][0] / 32768.0f;
        s_audio_ring[slot + 1] = frames[i][1] / 32768.0f;
        s_audio_write++;
    }
}

/* --------------------------------------------------------------------------
 * Input, speed and vblanks.
 */

static int16_t ToAxis(float v)
{
    return (int16_t)(v >= 0.0f ? v * 32767.0f : v * 32768.0f);
}

void rombie_engine_read_pad(int player, uint16_t *buttons, int16_t axis[6])
{
    PadState pad;
    {
        std::unique_lock<std::mutex> lock(s_pad_mutex);
        pad = s_pads[player & 3];
    }
    /* Bits 0-13; the Guide extension (14) is not the page's to press. */
    *buttons = (uint16_t)(pad.buttons & 0x3FFF);
    axis[CONTROLLER_AXIS_LTRIG] = ToAxis(pad.triggers[0]);
    axis[CONTROLLER_AXIS_RTRIG] = ToAxis(pad.triggers[1]);
    axis[CONTROLLER_AXIS_LSTICK_X] = ToAxis(pad.axes[0]);
    axis[CONTROLLER_AXIS_LSTICK_Y] = ToAxis(-pad.axes[1]);
    axis[CONTROLLER_AXIS_RSTICK_X] = ToAxis(pad.axes[2]);
    axis[CONTROLLER_AXIS_RSTICK_Y] = ToAxis(-pad.axes[3]);
}

double rombie_engine_speed(void)
{
    return s_speed.load(std::memory_order_relaxed);
}

void rombie_engine_on_vblank(void)
{
    s_stats.vblanks.fetch_add(1, std::memory_order_relaxed);
}

void rombie_engine_on_pad_poll(int port, uint16_t buttons)
{
    if (!rombie_engine_enabled() || port < 0 || port > 3) {
        return;
    }
    s_stats.guest_polls[port].fetch_add(1, std::memory_order_relaxed);
    s_stats.guest_buttons_seen.fetch_or(buttons, std::memory_order_relaxed);
}
