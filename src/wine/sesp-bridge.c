#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "sesp_codec.h"

static unsigned char *client_write = NULL;
static DWORD client_write_len = 0;
static int sesp_connect_reply_enabled = 0;
static uint32_t sesp_connect_reply = 1;
static int sesp_auto_reply = 0;
static uint8_t sesp_auto_reply_type = 4;
static char display_name_override[128];
static char display_device_id_override[256];
static char device_serial_override[128];
static uint32_t display_width_override = 0;
static uint32_t display_height_override = 0;
static int sesp_synthetic_headpose = 0;
static int sesp_headpose_live = 0;
static uint32_t sesp_synthetic_headpose_hz = 30;
static uint32_t sesp_headpose_udp_port = 0;
static int etdefaultpipe_mode = 0;
static int client_pipe_suffix_scan = 0;
static char etdefaultpipe_entry[512] = "127.0.0.1";

struct PosePacket {
    double x_cm;
    double y_cm;
    double z_cm;
    double yaw_deg;
    double pitch_deg;
    double roll_deg;
    double source_code;
    double packet_counter;
    double gaze_x;
    double gaze_y;
    double gaze_valid;
    double gaze_counter;
};

struct LivePoseState {
    CRITICAL_SECTION lock;
    struct PosePacket pose;
    ULONGLONG last_tick;
    unsigned long long packets;
    int has_pose;
};

static struct LivePoseState live_pose;
static int live_pose_initialized = 0;
static volatile LONG live_pose_udp_running = 0;
static HANDLE live_pose_udp_thread = NULL;

static double finite_or_zero(double value)
{
    return isfinite(value) ? value : 0.0;
}

static int snapshot_live_pose(struct PosePacket *out, DWORD max_age_ms)
{
    int fresh = 0;
    if (!live_pose_initialized || !out)
        return 0;

    EnterCriticalSection(&live_pose.lock);
    if (live_pose.has_pose) {
        *out = live_pose.pose;
        fresh = (GetTickCount64() - live_pose.last_tick) <= max_age_ms;
    }
    LeaveCriticalSection(&live_pose.lock);
    return fresh;
}

static DWORD WINAPI live_pose_udp_worker(LPVOID arg)
{
    uint32_t port = *(uint32_t *)arg;
    WSADATA wsa;
    SOCKET sock = INVALID_SOCKET;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    DWORD timeout_ms = 250;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    while (InterlockedCompareExchange(&live_pose_udp_running, 1, 1) == 1) {
        struct PosePacket packet;
        memset(&packet, 0, sizeof(packet));
        int got = recv(sock, (char *)&packet, sizeof(packet), 0);
        if (got == (int)sizeof(packet) || got == (int)(sizeof(double) * 8)) {
            packet.x_cm = finite_or_zero(packet.x_cm);
            packet.y_cm = finite_or_zero(packet.y_cm);
            packet.z_cm = finite_or_zero(packet.z_cm);
            packet.yaw_deg = finite_or_zero(packet.yaw_deg);
            packet.pitch_deg = finite_or_zero(packet.pitch_deg);
            packet.roll_deg = finite_or_zero(packet.roll_deg);
            packet.source_code = finite_or_zero(packet.source_code);
            packet.packet_counter = finite_or_zero(packet.packet_counter);
            packet.gaze_x = finite_or_zero(packet.gaze_x);
            packet.gaze_y = finite_or_zero(packet.gaze_y);
            packet.gaze_valid = finite_or_zero(packet.gaze_valid);
            packet.gaze_counter = finite_or_zero(packet.gaze_counter);

            EnterCriticalSection(&live_pose.lock);
            live_pose.pose = packet;
            live_pose.last_tick = GetTickCount64();
            live_pose.packets++;
            live_pose.has_pose = 1;
            LeaveCriticalSection(&live_pose.lock);
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static unsigned char *hex_decode(const char *hex, DWORD *out_len)
{
    size_t n = strlen(hex);
    unsigned char *out = (unsigned char *)calloc(n / 2 + 1, 1);
    DWORD len = 0;
    int hi = -1;
    if (!out)
        return NULL;

    for (size_t i = 0; i < n; ++i) {
        int v = hex_nibble(hex[i]);
        if (v < 0)
            continue;
        if (hi < 0)
            hi = v;
        else {
            out[len++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
    }
    *out_len = len;
    return out;
}

static int set_client_write_hex(const char *hex)
{
    free(client_write);
    client_write = hex_decode(hex, &client_write_len);
    return client_write != NULL;
}

static int set_client_write_text(const char *text)
{
    size_t len = strlen(text);
    unsigned char *buf = (unsigned char *)calloc(len + 1, 1);
    if (!buf)
        return 0;
    memcpy(buf, text, len);
    free(client_write);
    client_write = buf;
    client_write_len = (DWORD)(len + 1);
    return 1;
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void write_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static volatile LONG licence_announced = 0;

static void note_licence(const unsigned char *data, DWORD len)
{
    if (!sesp_contains_licence(data, (uint32_t)len))
        return;
    if (InterlockedExchange(&licence_announced, 1) == 0) {
        fputs("licence\n", stdout);
        fflush(stdout);
    }
}

static void drain_service_pipe(HANDLE pipe, uint32_t *channel,
                               char *client_name_out, size_t client_name_size)
{
    unsigned char buf[65536];
    *channel = 0;
    client_name_out[0] = '\0';

    for (;;) {
        DWORD to_read = sizeof(buf);
        DWORD got = 0;
        BOOL ok = ReadFile(pipe, buf, to_read, &got, NULL);
        if (!ok || !got)
            return;
        note_licence(buf, got);
        if (got >= 6) {
            const char *client_name = (const char *)(buf + 4);
            DWORD name_room = got - 4;
            DWORD name_len = 0;
            while (name_len < name_room && client_name[name_len] != '\0')
                ++name_len;
            if (name_len < name_room) {
                *channel = read_le32(buf);
                size_t copy_len = name_len;
                if (copy_len >= client_name_size)
                    copy_len = client_name_size - 1;
                memcpy(client_name_out, client_name, copy_len);
                client_name_out[copy_len] = '\0';
            }
        }
        if (got < sizeof(buf))
            return;
    }
}

static void write_client_payload(HANDLE client, uint32_t channel)
{
    if (client_write && client_write_len) {
        DWORD wrote = 0;
        WriteFile(client, client_write, client_write_len, &wrote, NULL);
        return;
    }

    if (sesp_connect_reply_enabled && channel >= 0x2711 && channel <= 0x2714) {
        unsigned char reply[4];
        reply[0] = (unsigned char)sesp_connect_reply;
        reply[1] = (unsigned char)(sesp_connect_reply >> 8);
        reply[2] = (unsigned char)(sesp_connect_reply >> 16);
        reply[3] = (unsigned char)(sesp_connect_reply >> 24);
        DWORD wrote = 0;
        WriteFile(client, reply, sizeof(reply), &wrote, NULL);
    }
}

static int read_flatbuf_root_type(const unsigned char *payload, DWORD payload_len, uint32_t *seq, uint8_t *type)
{
    return sesp_read_root_type(payload, (uint32_t)payload_len, seq, type);
}

static unsigned char *make_sesp_frame(const unsigned char *payload, uint32_t payload_len, DWORD *out_len)
{
    uint32_t n = 0;
    unsigned char *frame = sesp_make_frame(payload, payload_len, &n);
    *out_len = n;
    return frame;
}

static unsigned char *make_sesp_response_initialize(uint32_t seq, DWORD *out_len)
{
    uint32_t n = 0;
    unsigned char *frame = sesp_make_response_initialize(seq, &n);
    *out_len = n;
    return frame;
}

static unsigned char *make_sesp_status_response(uint32_t seq, uint8_t reply_type, DWORD *out_len)
{
    uint32_t n = 0;
    unsigned char *frame = sesp_make_status_response(seq, reply_type, &n);
    *out_len = n;
    return frame;
}

static unsigned char *make_sesp_subscription_headpose(
    uint32_t seq,
    uint64_t timestamp,
    float pos_x,
    float pos_y,
    float pos_z,
    float rot_x,
    float rot_y,
    float rot_z,
    int valid,
    DWORD *out_len)
{
    struct SespHeadpose pose;
    memset(&pose, 0, sizeof(pose));
    pose.timestamp_us = timestamp;
    pose.pos[0] = pos_x;
    pose.pos[1] = pos_y;
    pose.pos[2] = pos_z;
    pose.rot[0] = rot_x;
    pose.rot[1] = rot_y;
    pose.rot[2] = rot_z;
    pose.pos_valid = valid;
    pose.rot_valid[0] = valid;
    pose.rot_valid[1] = valid;
    pose.rot_valid[2] = valid;
    uint32_t n = 0;
    unsigned char *frame = sesp_make_headpose(seq, &pose, &n);
    *out_len = n;
    return frame;
}

struct SyntheticHeadposeWriter {
    HANDLE client;
    volatile LONG running;
};

static DWORD WINAPI synthetic_headpose_worker(LPVOID arg)
{
    struct SyntheticHeadposeWriter *writer = (struct SyntheticHeadposeWriter *)arg;
    uint32_t seq = 1;
    int phase = 0;
    DWORD sleep_ms = sesp_synthetic_headpose_hz ? (DWORD)(1000U / sesp_synthetic_headpose_hz) : 33;
    if (sleep_ms < 5)
        sleep_ms = 5;

    while (InterlockedCompareExchange(&writer->running, 1, 1) == 1) {
        const double deg_to_rad = 3.14159265358979323846 / 180.0;
        float pos_x = 0.0f, pos_y = 0.0f, pos_z = 600.0f;
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        int valid = 1;

        struct PosePacket live;
        memset(&live, 0, sizeof(live));
        int live_active = snapshot_live_pose(&live, 750);

        if (live_active) {
            pos_x = (float)(live.x_cm * 10.0);
            pos_y = (float)(live.y_cm * 10.0);
            pos_z = (float)(600.0 + live.z_cm * 10.0);
            yaw = (float)(live.yaw_deg * deg_to_rad);
            pitch = (float)(live.pitch_deg * deg_to_rad);
            roll = (float)(live.roll_deg * deg_to_rad);
            valid = 1;
        } else if (sesp_synthetic_headpose) {
            int saw = phase % 120;
            float centered = (float)(saw < 60 ? saw : 120 - saw) / 60.0f;
            yaw = (centered - 0.5f) * 0.30f;
            pitch = (centered - 0.5f) * 0.16f;
            roll = (centered - 0.5f) * 0.08f;
            valid = 1;
        } else {
            pos_x = (float)(live.x_cm * 10.0);
            pos_y = (float)(live.y_cm * 10.0);
            pos_z = (float)(600.0 + live.z_cm * 10.0);
            yaw = (float)(live.yaw_deg * deg_to_rad);
            pitch = (float)(live.pitch_deg * deg_to_rad);
            roll = (float)(live.roll_deg * deg_to_rad);
            valid = 0;
        }
        DWORD frame_len = 0;
        unsigned char *frame = make_sesp_subscription_headpose(
            seq++,
            (uint64_t)GetTickCount64() * 1000ULL,
            pos_x,
            pos_y,
            pos_z,
            pitch,
            yaw,
            roll,
            valid,
            &frame_len);
        if (frame && frame_len) {
            DWORD wrote = 0;
            if (!WriteFile(writer->client, frame, frame_len, &wrote, NULL)) {
                free(frame);
                break;
            }
        }
        free(frame);
        phase++;
        Sleep(sleep_ms);
    }
    return 0;
}

static void normalize_display_device_id(char *s)
{
    for (; *s; ++s) {
        if (*s == '#')
            *s = '\\';
        else
            *s = (char)toupper((unsigned char)*s);
    }
}

static void choose_display_device_id_substring(char *out, size_t out_len)
{
    if (!out_len)
        return;
    out[0] = '\0';

    if (display_device_id_override[0]) {
        snprintf(out, out_len, "%s", display_device_id_override);
        normalize_display_device_id(out);
        return;
    }

    DISPLAY_DEVICEA adapter;
    DISPLAY_DEVICEA monitor;
    for (DWORD adapter_index = 0; ; ++adapter_index) {
        memset(&adapter, 0, sizeof(adapter));
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesA(NULL, adapter_index, &adapter, 0))
            break;

        for (DWORD monitor_index = 0; ; ++monitor_index) {
            memset(&monitor, 0, sizeof(monitor));
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesA(adapter.DeviceName, monitor_index, &monitor, 0))
                break;
            if (!monitor.DeviceID[0])
                continue;

            snprintf(out, out_len, "%s", monitor.DeviceID);
            normalize_display_device_id(out);
            return;
        }
    }

    snprintf(out, out_len, "DISPLAY\\DEFAULT_MONITOR");
}

static unsigned char *make_sesp_display_info_response(uint32_t seq, DWORD *out_len)
{
    char display_name[128];
    char device_id_substring[256];
    uint32_t display_width = display_width_override;
    uint32_t display_height = display_height_override;

    snprintf(display_name, sizeof(display_name), "%s", display_name_override[0] ? display_name_override : "\\\\.\\DISPLAY1");
    if (!display_width)
        display_width = (uint32_t)GetSystemMetrics(SM_CXSCREEN);
    if (!display_height)
        display_height = (uint32_t)GetSystemMetrics(SM_CYSCREEN);

    POINT pt;
    pt.x = 0;
    pt.y = 0;
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi)) {
        if (!display_name_override[0] && mi.szDevice[0])
            snprintf(display_name, sizeof(display_name), "%s", mi.szDevice);
        if (!display_width_override)
            display_width = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
        if (!display_height_override)
            display_height = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }

    choose_display_device_id_substring(device_id_substring, sizeof(device_id_substring));

    const uint32_t wrapper_obj = 4;
    const uint32_t inner_obj = 20;
    const uint32_t device_id_string = 36;
    const uint32_t device_id_len = (uint32_t)strlen(device_id_substring);
    const uint32_t inner_vt = (device_id_string + 4 + device_id_len + 1 + 3) & ~3U;
    const uint32_t wrapper_vt = inner_vt + 12;
    const uint32_t payload_len = wrapper_vt + 10;
    unsigned char *payload = (unsigned char *)calloc(payload_len, 1);
    if (!payload)
        return NULL;

    write_le32(payload + 0, wrapper_obj);

    write_le32(payload + wrapper_obj + 0, (uint32_t)(int32_t)((int32_t)wrapper_obj - (int32_t)wrapper_vt));
    write_le32(payload + wrapper_obj + 4, seq);
    write_le32(payload + wrapper_obj + 8, inner_obj - (wrapper_obj + 8));
    payload[wrapper_obj + 12] = 10;

    write_le32(payload + inner_obj + 0, (uint32_t)(int32_t)((int32_t)inner_obj - (int32_t)inner_vt));
    write_le32(payload + inner_obj + 4, 0);
    write_le32(payload + inner_obj + 8, device_id_string - (inner_obj + 8));
    write_le32(payload + inner_obj + 12, display_width);
    write_le32(payload + inner_obj + 16, display_height);

    write_le32(payload + device_id_string + 0, device_id_len);
    memcpy(payload + device_id_string + 4, device_id_substring, device_id_len);

    write_le16(payload + inner_vt + 0, 12);
    write_le16(payload + inner_vt + 2, 20);
    write_le16(payload + inner_vt + 4, 4);
    write_le16(payload + inner_vt + 6, 8);
    write_le16(payload + inner_vt + 8, 12);
    write_le16(payload + inner_vt + 10, 16);

    write_le16(payload + wrapper_vt + 0, 10);
    write_le16(payload + wrapper_vt + 2, 13);
    write_le16(payload + wrapper_vt + 4, 4);
    write_le16(payload + wrapper_vt + 6, 12);
    write_le16(payload + wrapper_vt + 8, 8);

    unsigned char *frame = make_sesp_frame(payload, payload_len, out_len);
    free(payload);
    return frame;
}

static uint32_t put_fb_string(unsigned char *payload, uint32_t pos, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    write_le32(payload + pos, len);
    memcpy(payload + pos + 4, s, len);
    return (pos + 4 + len + 1 + 3) & ~3U;
}

static unsigned char *make_sesp_list_devices_response(uint32_t seq, DWORD *out_len)
{
    const char *serial = device_serial_override[0] ? device_serial_override
                                                    : "IS5FF-100203612152";
    char url_buf[160];
    snprintf(url_buf, sizeof(url_buf), "tobii-ttp://%s", serial);
    const char *url = url_buf;
    const char *name = "Eye Tracker 5";
    const char *model = "IS5_Large_Eyetracker_5";
    const char *family = "IS5";
    const char *firmware = "02a1a6a977";

    const uint32_t wrapper_obj = 4;
    const uint32_t inner_obj = 20;
    const uint32_t devices_vector = 28;
    const uint32_t device_obj = 36;
    const uint32_t strings_start = 68;
    unsigned char *payload = (unsigned char *)calloc(512, 1);
    if (!payload)
        return NULL;

    uint32_t pos = strings_start;
    uint32_t url_obj = pos;
    pos = put_fb_string(payload, pos, url);
    uint32_t name_obj = pos;
    pos = put_fb_string(payload, pos, name);
    uint32_t model_obj = pos;
    pos = put_fb_string(payload, pos, model);
    uint32_t serial_obj = pos;
    pos = put_fb_string(payload, pos, serial);
    uint32_t family_obj = pos;
    pos = put_fb_string(payload, pos, family);
    uint32_t firmware_obj = pos;
    pos = put_fb_string(payload, pos, firmware);

    uint32_t device_vt = pos;
    pos += 18;
    uint32_t inner_vt = pos;
    pos += 6;
    uint32_t wrapper_vt = pos;
    pos += 10;
    uint32_t payload_len = pos;

    write_le32(payload + 0, wrapper_obj);

    write_le32(payload + wrapper_obj + 0, (uint32_t)(int32_t)((int32_t)wrapper_obj - (int32_t)wrapper_vt));
    write_le32(payload + wrapper_obj + 4, seq);
    write_le32(payload + wrapper_obj + 8, inner_obj - (wrapper_obj + 8));
    payload[wrapper_obj + 12] = 0x0c;

    write_le32(payload + inner_obj + 0, (uint32_t)(int32_t)((int32_t)inner_obj - (int32_t)inner_vt));
    write_le32(payload + inner_obj + 4, devices_vector - (inner_obj + 4));

    write_le32(payload + devices_vector + 0, 1);
    write_le32(payload + devices_vector + 4, device_obj - (devices_vector + 4));

    write_le32(payload + device_obj + 0, (uint32_t)(int32_t)((int32_t)device_obj - (int32_t)device_vt));
    write_le32(payload + device_obj + 4, url_obj - (device_obj + 4));
    write_le32(payload + device_obj + 8, name_obj - (device_obj + 8));
    write_le32(payload + device_obj + 12, model_obj - (device_obj + 12));
    write_le32(payload + device_obj + 16, serial_obj - (device_obj + 16));
    write_le32(payload + device_obj + 20, family_obj - (device_obj + 20));
    write_le32(payload + device_obj + 24, firmware_obj - (device_obj + 24));
    write_le32(payload + device_obj + 28, 1);

    write_le16(payload + device_vt + 0, 18);
    write_le16(payload + device_vt + 2, 32);
    write_le16(payload + device_vt + 4, 4);
    write_le16(payload + device_vt + 6, 8);
    write_le16(payload + device_vt + 8, 12);
    write_le16(payload + device_vt + 10, 16);
    write_le16(payload + device_vt + 12, 20);
    write_le16(payload + device_vt + 14, 24);
    write_le16(payload + device_vt + 16, 28);

    write_le16(payload + inner_vt + 0, 6);
    write_le16(payload + inner_vt + 2, 8);
    write_le16(payload + inner_vt + 4, 4);

    write_le16(payload + wrapper_vt + 0, 10);
    write_le16(payload + wrapper_vt + 2, 13);
    write_le16(payload + wrapper_vt + 4, 4);
    write_le16(payload + wrapper_vt + 6, 12);
    write_le16(payload + wrapper_vt + 8, 8);

    unsigned char *frame = make_sesp_frame(payload, payload_len, out_len);
    free(payload);
    return frame;
}

static unsigned char *make_sesp_reply_from_request(const unsigned char *frame, DWORD frame_len, uint8_t fallback_reply_type, DWORD *out_len, uint8_t *out_req_type)
{
    if (frame_len < 32 || memcmp(frame, "sesp", 4) != 0)
        return NULL;

    uint32_t payload_len = read_le32(frame + 4);
    if (payload_len > frame_len - 12)
        return NULL;

    const unsigned char *payload = frame + 12;
    uint32_t seq = 0;
    uint8_t req_type = 0;
    if (!read_flatbuf_root_type(payload, payload_len, &seq, &req_type))
        return NULL;
    if (out_req_type)
        *out_req_type = req_type;

    if (req_type == 2) {
        return make_sesp_response_initialize(seq, out_len);
    }
    if (req_type == 9) {
        return make_sesp_display_info_response(seq, out_len);
    }
    if (req_type == 0xb) {
        return make_sesp_list_devices_response(seq, out_len);
    }
    if (req_type == 4 || req_type == 6 || req_type == 0xd ||
        req_type == 0x10 || req_type == 0x14 || req_type == 0x16 ||
        req_type == 0x1a || req_type == 0x21 || req_type == 0x23 ||
        req_type == 0x32) {
        uint8_t reply_type = (uint8_t)(req_type + 1);
        return make_sesp_status_response(seq, reply_type, out_len);
    }

    return make_sesp_status_response(seq, fallback_reply_type, out_len);
}

static void maybe_write_sesp_reply(HANDLE client, const unsigned char *buf, DWORD got)
{
    if (!sesp_auto_reply)
        return;

    DWORD offset = 0;
    while (offset + 12 <= got) {
        if (memcmp(buf + offset, "sesp", 4) != 0)
            return;
        uint32_t payload_len = read_le32(buf + offset + 4);
        DWORD frame_len = 12 + payload_len;
        if (frame_len > got - offset)
            return;

        DWORD out_len = 0;
        uint8_t req_type = 0;
        unsigned char *out = make_sesp_reply_from_request(buf + offset, frame_len, sesp_auto_reply_type, &out_len, &req_type);
        if (out && out_len) {
            DWORD wrote = 0;
            WriteFile(client, out, out_len, &wrote, NULL);
            FlushFileBuffers(client);
        }
        free(out);
        (void)req_type;
        offset += frame_len;
    }
}

static void serve_service_followup(HANDLE service_pipe, HANDLE client)
{
    unsigned char buf[65536];

    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(service_pipe, buf, sizeof(buf), &got, NULL);
        if (!ok || !got) {
            return;
        }
        note_licence(buf, got);
        maybe_write_sesp_reply(client, buf, got);
    }
}

static void serve_etdefaultpipe_once(HANDLE pipe)
{
    unsigned char request[64];
    unsigned char response[8 + 0x200];
    DWORD got = 0;
    DWORD wrote = 0;

    memset(request, 0, sizeof(request));
    memset(response, 0, sizeof(response));

    BOOL ok = ReadFile(pipe, request, sizeof(request), &got, NULL);
    if (!ok || got == 0) {
        return;
    }

    write_le32(response + 0, 1);
    write_le32(response + 4, 1);
    snprintf((char *)(response + 8), 0x200, "%s", etdefaultpipe_entry);

    WriteFile(pipe, response, sizeof(response), &wrote, NULL);
    FlushFileBuffers(pipe);
}

typedef BOOL(WINAPI *GetNamedPipeClientProcessIdFn)(HANDLE Pipe, PULONG ClientProcessId);

static int connect_client_pipe(HANDLE service_pipe)
{
    int handled = 0;
    ULONGLONG deadline = GetTickCount64() + 1000;
    ULONG client_pid = 0;

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    union {
        FARPROC raw;
        GetNamedPipeClientProcessIdFn typed;
    } get_client_pid_proc = {0};
    if (kernel32)
        get_client_pid_proc.raw = GetProcAddress(kernel32, "GetNamedPipeClientProcessId");
    GetNamedPipeClientProcessIdFn get_client_pid = get_client_pid_proc.typed;
    if (get_client_pid && !get_client_pid(service_pipe, &client_pid))
        client_pid = 0;

    uint32_t channel = 0;
    char client_name[MAX_PATH];
    drain_service_pipe(service_pipe, &channel, client_name, sizeof(client_name));

    if (client_name[0]) {
        char name[sizeof("\\\\.\\pipe\\") + MAX_PATH];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\%.*s", (int)(sizeof(client_name) - 1), client_name);
        HANDLE client = CreateFileA(
            name,
            GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (client != INVALID_HANDLE_VALUE) {
            struct SyntheticHeadposeWriter synthetic_writer;
            HANDLE synthetic_thread = NULL;
            write_client_payload(client, channel);
            FlushFileBuffers(client);
            if ((sesp_synthetic_headpose || sesp_headpose_live) && channel == 0x2712) {
                synthetic_writer.client = client;
                synthetic_writer.running = 1;
                synthetic_thread = CreateThread(NULL, 0, synthetic_headpose_worker, &synthetic_writer, 0, NULL);
            }
            serve_service_followup(service_pipe, client);
            if (synthetic_thread) {
                InterlockedExchange(&synthetic_writer.running, 0);
                WaitForSingleObject(synthetic_thread, 1000);
                CloseHandle(synthetic_thread);
            }
            CloseHandle(client);
            return 1;
        }
    }

    if (client_pipe_suffix_scan && client_pid) {
        ULONGLONG brute_deadline = GetTickCount64() + 1200;
        while (!handled && GetTickCount64() < brute_deadline) {
            for (unsigned suffix = 0; !handled && suffix <= 0xff; ++suffix) {
                char name[MAX_PATH];
                snprintf(name, sizeof(name), "\\\\.\\pipe\\client_streamengineservices_%08lx_00006ffffee900%02x",
                         (unsigned long)client_pid, suffix);
                HANDLE client = CreateFileA(
                    name,
                    GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL);
                if (client != INVALID_HANDLE_VALUE) {
                    struct SyntheticHeadposeWriter synthetic_writer;
                    HANDLE synthetic_thread = NULL;
                    write_client_payload(client, channel);
                    FlushFileBuffers(client);
                    if ((sesp_synthetic_headpose || sesp_headpose_live) && channel == 0x2712) {
                        synthetic_writer.client = client;
                        synthetic_writer.running = 1;
                        synthetic_thread = CreateThread(NULL, 0, synthetic_headpose_worker, &synthetic_writer, 0, NULL);
                    }
                    serve_service_followup(service_pipe, client);
                    if (synthetic_thread) {
                        InterlockedExchange(&synthetic_writer.running, 0);
                        WaitForSingleObject(synthetic_thread, 1000);
                        CloseHandle(synthetic_thread);
                    }
                    CloseHandle(client);
                    handled = 1;
                    break;
                }
            }
            if (!handled)
                Sleep(25);
        }
        if (handled)
            return handled;
    }

    while (client_pipe_suffix_scan && !handled && GetTickCount64() < deadline) {
        WIN32_FIND_DATAW find_data;
        HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\*", &find_data);
        if (find == INVALID_HANDLE_VALUE) {
            Sleep(25);
            continue;
        }

        do {
            char file_name[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1, file_name, sizeof(file_name), NULL, NULL);
            if (strncmp(file_name, "client_streamengineservices_", 28) != 0)
                continue;

            char name[sizeof("\\\\.\\pipe\\") + MAX_PATH];
            snprintf(name, sizeof(name), "\\\\.\\pipe\\%.*s", (int)(sizeof(file_name) - 1), file_name);

            HANDLE client = CreateFileA(
                name,
                GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
            if (client != INVALID_HANDLE_VALUE) {
                struct SyntheticHeadposeWriter synthetic_writer;
                HANDLE synthetic_thread = NULL;
                write_client_payload(client, channel);
                FlushFileBuffers(client);
                if ((sesp_synthetic_headpose || sesp_headpose_live) && channel == 0x2712) {
                    synthetic_writer.client = client;
                    synthetic_writer.running = 1;
                    synthetic_thread = CreateThread(NULL, 0, synthetic_headpose_worker, &synthetic_writer, 0, NULL);
                }
                serve_service_followup(service_pipe, client);
                if (synthetic_thread) {
                    InterlockedExchange(&synthetic_writer.running, 0);
                    WaitForSingleObject(synthetic_thread, 1000);
                    CloseHandle(synthetic_thread);
                }
                CloseHandle(client);
                handled = 1;
                break;
            }
        } while (FindNextFileW(find, &find_data));

        FindClose(find);
        if (!handled)
            Sleep(25);
    }

    return handled;
}

static volatile LONG subscription_announced = 0;

static DWORD WINAPI service_worker(LPVOID arg)
{
    HANDLE pipe = (HANDLE)arg;
    if (InterlockedExchange(&subscription_announced, 1) == 0) {
        fputs("subscribed\n", stdout);
        fflush(stdout);
    }
    if (etdefaultpipe_mode)
        serve_etdefaultpipe_once(pipe);
    else
        connect_client_pipe(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

int main(int argc, char **argv)
{
    DWORD seconds = 35;
    const char *pipe_name = "\\\\.\\pipe\\streamengineservices";
    const char *suffix_scan_env = getenv("SC_TOBII_PIPE_SUFFIX_SCAN");
    if (suffix_scan_env && suffix_scan_env[0] && strcmp(suffix_scan_env, "0"))
        client_pipe_suffix_scan = 1;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = (DWORD)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--reply-bootstrap"))
            ;
        else if (!strcmp(argv[i], "--client-write-hex") && i + 1 < argc) {
            if (!set_client_write_hex(argv[++i])) {
                fprintf(stderr, "invalid --client-write-hex\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--client-write-text") && i + 1 < argc) {
            if (!set_client_write_text(argv[++i])) {
                fprintf(stderr, "invalid --client-write-text\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--sesp-connect-reply")) {
            sesp_connect_reply_enabled = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                sesp_connect_reply = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--sesp-auto-reply")) {
            sesp_auto_reply = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                sesp_auto_reply_type = (uint8_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--display-name") && i + 1 < argc)
            snprintf(display_name_override, sizeof(display_name_override), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--display-device-id") && i + 1 < argc)
            snprintf(display_device_id_override, sizeof(display_device_id_override), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--device-serial") && i + 1 < argc)
            snprintf(device_serial_override, sizeof(device_serial_override), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--display-width") && i + 1 < argc)
            display_width_override = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--display-height") && i + 1 < argc)
            display_height_override = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--sesp-synthetic-headpose")) {
            sesp_synthetic_headpose = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                sesp_synthetic_headpose_hz = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--sesp-headpose-udp") && i + 1 < argc) {
            sesp_headpose_live = 1;
            sesp_headpose_udp_port = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--etdefaultpipe"))
            etdefaultpipe_mode = 1;
        else if (!strcmp(argv[i], "--client-pipe-suffix-scan"))
            client_pipe_suffix_scan = 1;
        else if (!strcmp(argv[i], "--etdefault-entry") && i + 1 < argc)
            snprintf(etdefaultpipe_entry, sizeof(etdefaultpipe_entry), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--pipe") && i + 1 < argc)
            pipe_name = argv[++i];
    }

    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000ULL;

    if (sesp_headpose_udp_port) {
        InitializeCriticalSection(&live_pose.lock);
        live_pose_initialized = 1;
        InterlockedExchange(&live_pose_udp_running, 1);
        live_pose_udp_thread = CreateThread(NULL, 0, live_pose_udp_worker, &sesp_headpose_udp_port, 0, NULL);
        if (!live_pose_udp_thread) {
            InterlockedExchange(&live_pose_udp_running, 0);
        }
    }

    while (GetTickCount64() < deadline) {
        HANDLE pipe = CreateNamedPipeA(
            pipe_name,
            etdefaultpipe_mode ? PIPE_ACCESS_DUPLEX : PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            1000,
            NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            HANDLE thread = CreateThread(NULL, 0, service_worker, pipe, 0, NULL);
            if (thread != NULL) {
                CloseHandle(thread);
                pipe = INVALID_HANDLE_VALUE;
            } else {
                DisconnectNamedPipe(pipe);
            }
        }
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
    }

    if (live_pose_udp_thread) {
        InterlockedExchange(&live_pose_udp_running, 0);
        WaitForSingleObject(live_pose_udp_thread, 1000);
        CloseHandle(live_pose_udp_thread);
    }
    if (live_pose_initialized)
        DeleteCriticalSection(&live_pose.lock);
    free(client_write);
    return 0;
}
