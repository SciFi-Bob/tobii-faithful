#include "tobii/daemon/commands.hpp"

#include "tobii/ipc/protocol.hpp"

#include <opencv2/imgcodecs.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace tobii::daemon {
namespace {

bool JsonNumber(const std::string& line, const char* key, float& out) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = line.find(needle);
    if (at == std::string::npos) return false;
    out = std::strtof(line.c_str() + at + needle.size(), nullptr);
    return true;
}

bool JsonHasKey(const std::string& line, const char* key) {
    return line.find(std::string("\"") + key + "\":") != std::string::npos;
}

std::string Base64(const std::vector<unsigned char>& data) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const unsigned a = data[i];
        const unsigned b = i + 1 < data.size() ? data[i + 1] : 0;
        const unsigned c = i + 2 < data.size() ? data[i + 2] : 0;
        const unsigned triple = (a << 16) | (b << 8) | c;
        out += kAlphabet[(triple >> 18) & 0x3f];
        out += kAlphabet[(triple >> 12) & 0x3f];
        out += i + 1 < data.size() ? kAlphabet[(triple >> 6) & 0x3f] : '=';
        out += i + 2 < data.size() ? kAlphabet[triple & 0x3f] : '=';
    }
    return out;
}

constexpr std::size_t kMaxErrorChars = 200;

void Reply(const CommandContext& ctx, const std::string& line) {
    if (ctx.reply) ctx.reply(line);
}

void ReplyError(const CommandContext& ctx, const char* message) {
    Reply(ctx, std::string("{\"type\":\"error\",\"message\":\"") +
                   ipc::EscapeJson(message) + "\"}\n");
}

void ReplyOk(const CommandContext& ctx, const char* cmd) {
    Reply(ctx, std::string("{\"type\":\"ok\",\"cmd\":\"") + cmd + "\"}\n");
}

}

bool MutateProfile(CommandContext& ctx,
                   const std::function<void(calibration::Profile&)>& change) {
    calibration::Profile updated;
    if (!updated.Load(ctx.profile_path)) updated = ctx.pipeline.profile();

    change(updated);

    ctx.pipeline.set_profile(updated);
    return updated.Save(ctx.profile_path);
}

std::string StatusLine(const CommandContext& ctx) {
    const auto& p = ctx.pipeline.profile();
    const auto& c = ctx.counters;

    std::string error = c.error;
    if (error.size() > kMaxErrorChars) {
        error.resize(kMaxErrorChars);
        error += "...";
    }

    char buf[1024];
    const int written = std::snprintf(buf, sizeof(buf),
        "{\"type\":\"status\",\"version\":%d,\"device\":\"%s\",\"present\":%s,"
        "\"clients\":%zu,\"holders\":%zu,\"fps\":%.1f,\"frames\":%llu,\"poses\":%llu,"
        "\"face\":%s,\"display_mm\":[%.1f,%.1f],\"display_px\":[%d,%d],"
        "\"error\":\"%s\"}\n",
        ipc::kProtocolVersion, c.device_open ? "open" : "closed",
        c.device_present ? "true" : "false",
        c.clients, c.holders, c.fps,
        static_cast<unsigned long long>(c.frames),
        static_cast<unsigned long long>(c.poses),
        ctx.pipeline.face_tracked() ? "true" : "false",
        p.display.width_mm, p.display.height_mm,
        p.display.pixel_width, p.display.pixel_height,
        ipc::EscapeJson(error).c_str());

    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buf)) {
        char fallback[256];
        std::snprintf(fallback, sizeof(fallback),
                      "{\"type\":\"status\",\"version\":%d,\"device\":\"%s\","
                      "\"clients\":%zu,\"holders\":%zu,"
                      "\"error\":\"status line did not fit\"}\n",
                      ipc::kProtocolVersion, c.device_open ? "open" : "closed",
                      c.clients, c.holders);
        return fallback;
    }
    return buf;
}

std::string PoseLine(const Pipeline& pipeline, double now_s) {
    float gaze_x = 0.5f, gaze_y = 0.5f;
    bool gaze_valid = false;
    pipeline.raw_gaze(gaze_x, gaze_y, gaze_valid);
    const auto& hp = pipeline.official_pose();

    const auto& eyes = pipeline.eyes();
    const bool left_eye_valid = eyes.has_left && eyes.validity_left == 0;
    const bool right_eye_valid = eyes.has_right && eyes.validity_right == 0;

    char buf[768];
    std::snprintf(buf, sizeof(buf),
        "{\"type\":\"pose\",\"x\":%.5f,\"y\":%.5f,\"z\":%.5f,"
        "\"yaw\":%.6f,\"pitch\":%.6f,\"roll\":%.6f,"
        "\"gaze_x\":%.5f,\"gaze_y\":%.5f,\"gaze_valid\":%s,"
        "\"pos_valid\":%s,"
        "\"face\":%s,\"present\":%s,\"presence\":%.4f,\"distance_mm\":%.1f,"
        "\"raw_gaze\":[%.5f,%.5f],\"raw_gaze_valid\":%s,"
        "\"left_eye_valid\":%s,\"right_eye_valid\":%s,"
        "\"eye_distance_mm\":%.0f,"
        "\"counter\":%llu}\n",
        hp.pos[0], hp.pos[1], hp.pos[2], hp.rot[1], hp.rot[0], hp.rot[2],
        gaze_x, gaze_y, gaze_valid ? "true" : "false",
        hp.pos_valid ? "true" : "false",
        pipeline.face_tracked() ? "true" : "false",
        pipeline.face_present(now_s) ? "true" : "false",
        pipeline.presence(), pipeline.distance_mm(),
        gaze_x, gaze_y, gaze_valid ? "true" : "false",
        left_eye_valid ? "true" : "false", right_eye_valid ? "true" : "false",
        pipeline.eye_distance_mm(),
        static_cast<unsigned long long>(hp.ts_us));
    return buf;
}

std::string HeadPoseLine(const pose::HeadPoseSample& s) {
    char buf[384];
    std::snprintf(buf, sizeof(buf),
        "{\"type\":\"head_pose\",\"ts_us\":%llu,"
        "\"pos\":[%.3f,%.3f,%.3f],\"rot\":[%.6f,%.6f,%.6f],"
        "\"pos_valid\":%s,\"rot_valid\":[%s,%s,%s]}\n",
        static_cast<unsigned long long>(s.ts_us),
        s.pos[0], s.pos[1], s.pos[2], s.rot[0], s.rot[1], s.rot[2],
        s.pos_valid ? "true" : "false",
        s.rot_valid[0] ? "true" : "false",
        s.rot_valid[1] ? "true" : "false",
        s.rot_valid[2] ? "true" : "false");
    return buf;
}

std::string DiagnosticLine(const Pipeline& pipeline, double now_s) {
    const auto& eyes = pipeline.eyes();
    char buf[640];
    std::snprintf(buf, sizeof(buf),
        "{\"type\":\"diag\",\"t\":%.4f,\"face\":%s,"
        "\"eye_left\":[%.3f,%.3f,%.3f],\"eye_right\":[%.3f,%.3f,%.3f],"
        "\"eye_validity\":[%u,%u],\"eye_present\":[%u,%u],"
        "\"eye_has\":[%s,%s],\"eye_t_us\":%lld}\n",
        now_s, pipeline.face_tracked() ? "true" : "false",
        eyes.left[0], eyes.left[1], eyes.left[2],
        eyes.right[0], eyes.right[1], eyes.right[2],
        eyes.validity_left, eyes.validity_right,
        eyes.present_left, eyes.present_right,
        eyes.has_left ? "true" : "false", eyes.has_right ? "true" : "false",
        static_cast<long long>(eyes.timestamp_us));
    return buf;
}

std::string FrameLine(const Pipeline& pipeline) {
    const cv::Mat& frame = pipeline.last_frame();
    if (frame.empty()) {
        return "{\"type\":\"frame\",\"available\":false}\n";
    }

    std::vector<unsigned char> jpeg;
    cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 80});

    std::string out = "{\"type\":\"frame\",\"available\":true,\"width\":";
    out += std::to_string(frame.cols);
    out += ",\"height\":" + std::to_string(frame.rows);
    out += ",\"jpeg\":\"" + Base64(jpeg) + "\"";

    out += ",\"landmarks\":[";
    const auto& points = pipeline.landmarks();
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i > 0) out += ',';
        char xy[48];
        std::snprintf(xy, sizeof(xy), "%.1f,%.1f", points[i].x, points[i].y);
        out += xy;
    }
    out += "]}\n";
    return out;
}

void HandleCommand(CommandContext& ctx, const std::string& line) {
    const std::string cmd = ipc::JsonField(line, "cmd");

    if (cmd == "subscribe") {
        if (ctx.client == nullptr) { ReplyError(ctx, "no client to subscribe"); return; }
        ctx.client->wants_pose = true;
        ctx.client->holds_device = ipc::JsonBool(line, "hold", true);
        ctx.client->wants_diagnostics = ipc::JsonBool(line, "diagnostics", false);
        return;
    }

    if (cmd == "unsubscribe") {
        if (ctx.client == nullptr) return;
        ctx.client->wants_pose = false;
        ctx.client->holds_device = false;
        ctx.client->wants_diagnostics = false;
        return;
    }

    if (cmd == "hold") {
        if (ctx.client == nullptr) { ReplyError(ctx, "no client to hold"); return; }
        ctx.client->holds_device = ipc::JsonBool(line, "device", true);
        return;
    }

    if (cmd == "status") {
        Reply(ctx, StatusLine(ctx));
        return;
    }

    if (cmd == "deviceinfo") {
        const std::string body = ctx.device_info_json ? ctx.device_info_json() : std::string();
        if (body.empty()) { ReplyError(ctx, "device not open"); return; }
        Reply(ctx, body);
        return;
    }

    if (cmd == "frame") {
        Reply(ctx, FrameLine(ctx.pipeline));
        return;
    }

    if (cmd == "display-area") {
        calibration::DisplayArea after{};
        MutateProfile(ctx, [&](calibration::Profile& p) {
            auto& d = p.display;
            d.measured = ipc::JsonBool(line, "measured", d.measured);
            JsonNumber(line, "width_mm", d.width_mm);
            JsonNumber(line, "height_mm", d.height_mm);
            JsonNumber(line, "offset_x_mm", d.offset_x_mm);
            JsonNumber(line, "offset_y_mm", d.offset_y_mm);
            JsonNumber(line, "offset_z_mm", d.offset_z_mm);
            if (JsonHasKey(line, "tracker_drop_mm")) {
                float drop = d.tracker_drop_mm;
                JsonNumber(line, "tracker_drop_mm", drop);
                d.SetTrackerDrop(drop);
            }
            after = d;
        });

        if (ctx.device_open && ctx.device_open() && ctx.release_device) {
            ctx.release_device();
        }

        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"ok\",\"cmd\":\"display-area\",\"measured\":%s,"
                      "\"width_mm\":%.1f,\"height_mm\":%.1f,\"offset\":[%.1f,%.1f,%.1f],"
                      "\"tracker_drop_mm\":%.1f,\"drop_measured\":%s}\n",
                      after.measured ? "true" : "false", after.width_mm, after.height_mm,
                      after.offset_x_mm, after.offset_y_mm, after.offset_z_mm,
                      after.tracker_drop_mm, after.drop_measured ? "true" : "false");
        Reply(ctx, buf);
        return;
    }

    if (cmd == "reload-profile") {
        calibration::Profile reloaded;
        if (reloaded.Load(ctx.profile_path)) {
            ctx.pipeline.set_profile(reloaded);
            ReplyOk(ctx, "reload-profile");
        } else {
            ReplyError(ctx, "no profile to load");
        }
        return;
    }

    {
        using Verb = CalibrationAction::Verb;
        bool is_cal = true;
        CalibrationAction action;
        if (cmd == "cal-start") action.verb = Verb::kStart;
        else if (cmd == "cal-stop") action.verb = Verb::kStop;
        else if (cmd == "cal-clear") action.verb = Verb::kClear;
        else if (cmd == "cal-compute") action.verb = Verb::kCompute;
        else if (cmd == "cal-save") action.verb = Verb::kSave;
        else if (cmd == "cal-apply") action.verb = Verb::kApply;
        else if (cmd == "cal-collect" || cmd == "cal-discard") {
            action.verb = cmd == "cal-collect" ? Verb::kCollect : Verb::kDiscard;
            float v = 0.0f;
            if (!JsonNumber(line, "x", v)) { ReplyError(ctx, "collect needs x"); return; }
            action.x = v;
            if (!JsonNumber(line, "y", v)) { ReplyError(ctx, "collect needs y"); return; }
            action.y = v;
        } else if (cmd == "enabled-eye") {
            action.verb = Verb::kEnabledEye;
            float v = 0.0f;
            if (!JsonNumber(line, "eye", v)) { ReplyError(ctx, "enabled-eye needs eye 1/2/3"); return; }
            action.enabled_eye = static_cast<std::uint32_t>(v);
            if (action.enabled_eye < 1 || action.enabled_eye > 3) {
                ReplyError(ctx, "eye must be 1 (left), 2 (right) or 3 (both)");
                return;
            }
        } else {
            is_cal = false;
        }
        if (is_cal) {
            if (!ctx.calibrate) { ReplyError(ctx, "device calibration not available"); return; }
            std::string message;
            if (ctx.calibrate(action, message)) {
                char reply[160];
                std::snprintf(reply, sizeof(reply), "{\"type\":\"ok\",\"cmd\":\"%s\"}\n",
                              cmd.c_str());
                Reply(ctx, reply);
            } else {
                ReplyError(ctx, message.empty() ? "calibration step failed" : message.c_str());
            }
            return;
        }
    }

    ReplyError(ctx, "unknown command");
}

}
