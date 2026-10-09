#include "tobii/calibration/profile.hpp"
#include "tobii/daemon/commands.hpp"
#include "tobii/device/calibration_store.hpp"
#include "tobii/device/monitor_id.hpp"
#include "tobii/eventlog.hpp"
#include "tobii/ipc/protocol.hpp"
#include "tobii/ipc/server.hpp"
#include "tobii/paths.hpp"
#include "tobii/pipeline.hpp"
#include "tobii/usb/device.hpp"

#include <opencv2/core.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop = true; }

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Options {
    std::string socket_path = tobii::ipc::DefaultSocketPath();
    std::string model_dir;
    double idle_hold_s = 3.0;
    double stall_timeout_s = 3.0;
    double output_hz = 120.0;
    std::string profile_path = tobii::calibration::Profile::DefaultPath();
    tobii::usb::HandshakeOrder handshake = tobii::usb::HandshakeOrder::kOfficial;
    bool read_device_calibration = false;
    bool reconcile_calibration = false;
};

class PresenceCache {
public:
    bool Get(double now_s, bool device_open) {
        if (device_open) { present_ = true; asked_s_ = now_s; return true; }
        if (asked_s_ == 0.0 || now_s - asked_s_ > kMinIntervalS) {
            asked_s_ = now_s;
            present_ = tobii::usb::Device::IsPresent();
        }
        return present_;
    }

private:
    static constexpr double kMinIntervalS = 2.0;
    double asked_s_ = 0.0;
    bool present_ = false;
};

std::vector<std::string> ModelCandidates(const std::string& asked) {
    if (!asked.empty()) return {asked};
    return {
        tobii::paths::SiblingDir("share/tobii-faithful/models"),
        TOBII_MODEL_DIR_INSTALL,
        TOBII_MODEL_DIR_BUILD,
    };
}

std::string ResolveModels(const std::vector<std::string>& candidates) {
    for (const auto& dir : candidates) {
        if (dir.empty()) continue;
        if (!tobii::paths::FirstExisting({dir + "/face_detector.onnx"}).empty() &&
            !tobii::paths::FirstExisting({dir + "/face_landmarks_detector.onnx"}).empty()) {
            return dir;
        }
    }
    return {};
}

}

int main(int argc, char** argv) {
    Options opt;
    double pipeline_presence_hold = -1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) opt.socket_path = argv[++i];
        else if (a == "--models" && i + 1 < argc) opt.model_dir = argv[++i];
        else if (a == "--idle-hold" && i + 1 < argc) opt.idle_hold_s = std::atof(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) opt.output_hz = std::atof(argv[++i]);
        else if (a == "--profile" && i + 1 < argc) opt.profile_path = argv[++i];
        else if (a == "--stall-timeout" && i + 1 < argc) opt.stall_timeout_s = std::atof(argv[++i]);
        else if (a == "--read-device-calibration") opt.read_device_calibration = true;
        else if (a == "--reconcile-calibration") opt.reconcile_calibration = true;
        else if (a == "--presence-hold" && i + 1 < argc)
            pipeline_presence_hold = std::atof(argv[++i]);
        else if (a == "--handshake" && i + 1 < argc) {
            const std::string order = argv[++i];
            if (order == "official") opt.handshake = tobii::usb::HandshakeOrder::kOfficial;
            else if (order == "legacy") opt.handshake = tobii::usb::HandshakeOrder::kLegacy;
            else {
                std::fprintf(stderr, "--handshake wants legacy or official\n");
                return 2;
            }
        }
        else if (a == "--help") {
            std::printf("usage: tobiid [--socket PATH] [--models DIR] [--idle-hold S]"
                        " [--rate HZ] [--profile PATH]"
                        " [--presence-hold S] [--stall-timeout S]\n"
                        "              [--handshake legacy|official]"
                        " [--read-device-calibration] [--reconcile-calibration]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    cv::setNumThreads(1);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    tobii::ipc::Server server;
    if (!server.Acquire(opt.socket_path)) {
        return 0;
    }
    if (!server.Listen(opt.socket_path)) {
        tobii::eventlog::Record("Daemon failed to start: " + server.error());
        return 1;
    }

    const auto model_candidates = ModelCandidates(opt.model_dir);
    const std::string model_dir = ResolveModels(model_candidates);
    if (model_dir.empty()) {
        tobii::eventlog::Record("Daemon failed to start: cannot find the face models (looked in " +
                                tobii::paths::Describe(model_candidates) + ")");
        return 1;
    }

    tobii::Pipeline pipeline(model_dir + "/face_detector.onnx",
                             model_dir + "/face_landmarks_detector.onnx");
    tobii::usb::Device device;

    tobii::calibration::Profile profile;
    profile.Load(opt.profile_path);
    pipeline.set_profile(profile);
    if (pipeline_presence_hold >= 0.0) pipeline.set_presence_hold_s(pipeline_presence_hold);

    bool device_open = false;
    double last_holder_s = 0.0;
    std::uint64_t frames = 0, poses = 0;
    std::uint64_t last_official_ts = 0;
    double next_output_s = 0.0;
    double last_frame_s = 0.0;
    PresenceCache presence;

    double last_rate_s = NowSeconds();
    double last_heartbeat_s = 0.0;
    double frame_rate = 0.0;
    std::uint64_t frames_at_mark = 0;
    std::string device_error;

    const auto release_device = [&]() {
        if (!device_open) return;
        device.Close();
        device_open = false;
        pipeline.Reset();
    };

    const auto device_info_json = [&]() -> std::string {
        if (!device_open) return {};
        const auto& di = device.device_info();
        if (!di.valid) return {};
        const auto hex = [](const std::vector<std::uint8_t>& b) {
            static const char* d = "0123456789abcdef";
            std::string s;
            s.reserve(b.size() * 2);
            for (std::uint8_t c : b) { s.push_back(d[c >> 4]); s.push_back(d[c & 0xf]); }
            return s;
        };
        std::string out = "{\"type\":\"deviceinfo\"";
        out += ",\"serial\":\"" + tobii::ipc::EscapeJson(di.serial) + "\"";
        out += ",\"model\":\"" + tobii::ipc::EscapeJson(di.model) + "\"";
        out += ",\"platform\":\"" + tobii::ipc::EscapeJson(di.platform) + "\"";
        out += ",\"firmware\":\"" + tobii::ipc::EscapeJson(di.firmware) + "\"";
        out += ",\"device_info\":\"" + hex(di.device_info_payload) + "\"";
        out += ",\"device_config\":\"" + hex(di.device_config_payload) + "\"";
        out += ",\"capabilities\":\"" + hex(di.capabilities_payload) + "\"";
        out += ",\"stream_catalog\":\"" + hex(di.stream_catalog_payload) + "\"";
        out += "}\n";
        return out;
    };

    const auto calibrate = [&](const tobii::daemon::CalibrationAction& a,
                               std::string& msg) -> bool {
        using Verb = tobii::daemon::CalibrationAction::Verb;
        if (!device_open) { msg = "device not open"; return false; }
        const auto fail = [&]() { msg = device.error(); return false; };
        const auto run = [&]() -> bool {
        switch (a.verb) {
            case Verb::kStart:   return device.CalibrationStart() || fail();
            case Verb::kStop:    return device.CalibrationStop() || fail();
            case Verb::kClear:   return device.CalibrationClear() || fail();
            case Verb::kCompute: return device.CalibrationCompute() || fail();
            case Verb::kCollect: return device.CalibrationCollect(a.x, a.y) || fail();
            case Verb::kDiscard: return device.CalibrationDiscard(a.x, a.y) || fail();
            case Verb::kEnabledEye:
                return device.SetEnabledEye(static_cast<tobii::usb::EnabledEye>(a.enabled_eye))
                       || fail();
            case Verb::kSave: {
                std::vector<std::uint8_t> blob;
                if (!device.CalibrationRetrieve(blob)) return fail();
                const auto& info = device.device_info();
                if (info.serial.empty()) { msg = "device serial unknown"; return false; }
                tobii::device::CalibrationRecord rec;
                rec.serial = info.serial;
                rec.monitor_id = tobii::device::MonitorIdForConnector(
                    pipeline.profile().display.monitor_name);
                rec.blob = std::move(blob);
                tobii::device::CalibrationStore store;
                if (!store.Save(rec)) { msg = "could not store calibration"; return false; }
                tobii::eventlog::Record("Calibration saved (eye tracking)");
                return true;
            }
            case Verb::kApply: {
                const auto& info = device.device_info();
                if (info.serial.empty()) { msg = "device serial unknown"; return false; }
                const std::string monitor_id = tobii::device::MonitorIdForConnector(
                    pipeline.profile().display.monitor_name);
                tobii::device::CalibrationStore store;
                std::optional<tobii::device::CalibrationRecord> rec =
                    store.Load(info.serial, monitor_id);
                if (!rec) {
                    msg = "no stored calibration for " + info.serial + "&" + monitor_id;
                    return false;
                }
                return device.CalibrationApply(rec->blob) || fail();
            }
        }
        return false;
        };
        const bool ok = run();
        last_frame_s = NowSeconds();
        return ok;
    };

    const auto context_for = [&](tobii::ipc::Client* client,
                                 const std::function<void(const std::string&)>& reply) {
        tobii::daemon::CommandContext ctx{pipeline, opt.profile_path, client,
                                          release_device,
                                          [&]() { return device_open; },
                                          {}, device_info_json, calibrate, reply};
        ctx.counters.device_open = device_open;
        ctx.counters.device_present = presence.Get(NowSeconds(), device_open);
        ctx.counters.clients = server.client_count();
        ctx.counters.holders = server.device_holders();
        ctx.counters.games = server.game_clients();
        ctx.counters.fps = frame_rate;
        ctx.counters.frames = frames;
        ctx.counters.poses = poses;
        ctx.counters.error = device_error;
        return ctx;
    };

    const auto on_command = [&](tobii::ipc::Client& client, const std::string& line) {
        auto ctx = context_for(&client, [&](const std::string& out) {
            server.Send(client, out);
        });
        tobii::daemon::HandleCommand(ctx, line);
    };

    tobii::eventlog::Record("Daemon started");

    std::string recorded_device_error;
    const auto record_device_error = [&](const std::string& what) {
        if (what == recorded_device_error) return;
        recorded_device_error = what;
        tobii::eventlog::Record(what);
    };

    while (!g_stop) {
        if (!server.Poll(device_open ? 0 : 200, on_command)) {
            tobii::eventlog::Record("Daemon error: " + server.error());
            break;
        }

        const std::size_t holders = server.device_holders();
        const double now = NowSeconds();

        if (holders > 0 && !device_open) {
            tobii::usb::DeviceConfig cfg;
            cfg.should_stop = []() { return g_stop.load(); };
            cfg.handshake = opt.handshake;
            cfg.read_device_calibration = opt.read_device_calibration;
            cfg.enabled_eye_both = true;
            cfg.calibration_blob =
                [monitor = tobii::device::MonitorIdForConnector(
                     pipeline.profile().display.monitor_name)](const std::string& serial)
                -> std::vector<std::uint8_t> {
                    if (serial.empty()) return {};
                    tobii::device::CalibrationStore store;
                    const auto rec = store.Load(serial, monitor);
                    return rec ? rec->blob : std::vector<std::uint8_t>{};
                };
            if (pipeline.profile().display.measured) {
                pipeline.profile().display.ScreenCorners(
                    cfg.display.top_left, cfg.display.top_right, cfg.display.bottom_left);
            }
            if (device.Open(cfg)) {
                device_open = true;
                device_error.clear();
                recorded_device_error.clear();
                pipeline.Reset();
                last_frame_s = NowSeconds();
                if (opt.read_device_calibration) {
                    const auto& cal = device.device_calibration();
                    const auto& info = device.device_info();
                    if (cal.present && !info.serial.empty()) {
                        const std::string monitor = tobii::device::MonitorIdForConnector(
                            pipeline.profile().display.monitor_name);
                        tobii::device::CalibrationRecord rec;
                        rec.serial = info.serial;
                        rec.monitor_id = monitor;
                        rec.blob = cal.blob;
                        rec.stimulus = cal.stimulus;
                        tobii::device::CalibrationStore store;
                        if (store.Save(rec)) {
                            tobii::eventlog::Record("Calibration saved (read from the device)");
                        }
                    }
                }

                last_frame_s = NowSeconds();
            } else {
                device_error = device.error();
                record_device_error("Tracker could not be opened: " + device_error);
                const timespec ts{0, 500 * 1000 * 1000};
                nanosleep(&ts, nullptr);
            }
        }

        if (holders > 0) last_holder_s = now;
        if (device_open && holders == 0 && now - last_holder_s > opt.idle_hold_s) {
            release_device();
        }

        if (!device_open) continue;

        const auto result = device.Poll(
            [&](const tobii::usb::ImageFrame& image) {
                ++frames;
                last_frame_s = NowSeconds();
                pipeline.OnImage(image, NowSeconds());
            },
            [&](const tobii::usb::GazeSample& gaze) { pipeline.OnGaze(gaze); });

        if (device_open && last_frame_s > 0.0 &&
            now - last_frame_s > opt.stall_timeout_s) {
            if (device.opened_realm() == tobii::usb::kRealmObject2712) {
                tobii::usb::DeclineRealm2712(device.device_info().serial);
            }
            device_error = "stream stalled, device reopened";
            record_device_error("Tracker error: " + device_error);
            release_device();
            last_frame_s = 0.0;
            continue;
        }

        if (result == tobii::usb::PollResult::kError) {
            device_error = device.error();
            record_device_error("Tracker error: " + device_error);
            release_device();
            continue;
        }

        const bool frames_are_fresh = last_frame_s > 0.0 && (now - last_frame_s) < 1.0;

        if (device_open && now - last_heartbeat_s >= 1.0 / 33.0) {
            last_heartbeat_s = now;
            device.Heartbeat();
        }

        if (pipeline.face_tracked() && frames_are_fresh && now >= next_output_s) {
            const double period = opt.output_hz > 1.0 ? 1.0 / opt.output_hz : 1.0 / 120.0;
            next_output_s = std::max(next_output_s + period, now);
            ++poses;
            server.BroadcastPose(tobii::daemon::PoseLine(pipeline, now));

            if (server.any_diagnostics()) {
                server.BroadcastDiagnostics(tobii::daemon::DiagnosticLine(pipeline, now));
            }
        }

        if (frames_are_fresh) {
            const auto& hp = pipeline.official_pose();
            if (hp.ts_us != last_official_ts && (hp.pos_valid || hp.rot_valid[1])) {
                last_official_ts = hp.ts_us;
                ++poses;
                server.BroadcastPose(tobii::daemon::HeadPoseLine(hp));
            }
        }

        if (now - last_rate_s >= 1.0) {
            frame_rate = static_cast<double>(frames - frames_at_mark) / (now - last_rate_s);
            frames_at_mark = frames;
            last_rate_s = now;
        }
    }

    device.Close();
    server.Close();
    tobii::eventlog::Record("Daemon stopped");
    return 0;
}
