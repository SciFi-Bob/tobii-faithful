#include "tobii/adapter/translate.hpp"
#include "tobii/ipc/protocol.hpp"
#include "tobii/paths.hpp"
#include "tobii/calibration/profile.hpp"
#include "tobii/eventlog.hpp"
#include "tobii/games/catalogue.hpp"
#include "tobii/middleware/server.hpp"
#include "tobii/wine/prefix.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <map>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <vector>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

constexpr double kSerialWaitS = 2.0;

struct Options {
    std::string daemon_socket = tobii::ipc::DefaultSocketPath();
    std::string middleware_host = "127.0.0.1";
    std::uint16_t middleware_port = tobii::middleware::kDefaultMiddlewarePort;
    std::uint16_t headpose_port = 4243;
    double presence_hz = 10.0;
    std::string bridge_exe;
    std::string device_serial = "IS5FF-100203612152";
    bool serial_from_flag = false;
    std::string profile_path = tobii::calibration::Profile::DefaultPath();
    std::string games_path = tobii::games::Catalogue::DefaultPath();
    int display_override_w = 0, display_override_h = 0;
    bool manage_bridge = true;
};

tobii::adapter::DisplayGeometry DetectPrimaryDisplay() {
    FILE* pipe = ::popen("xrandr 2>/dev/null", "r");
    if (pipe == nullptr) return {};
    std::string output;
    char line[1024];
    while (std::fgets(line, sizeof(line), pipe) != nullptr) output += line;
    ::pclose(pipe);
    return tobii::adapter::ParseXrandr(output);
}

class PrefixJudge {
public:
    bool Serves(const tobii::games::Catalogue& catalogue, const std::string& prefix) {
        if (const auto* known = catalogue.ForPrefix(prefix); known != nullptr) return true;
        const auto seen = judged_.find(prefix);
        if (seen != judged_.end()) return seen->second.has_value();

        auto found = tobii::games::Catalogue::ScanPrefix(prefix);
        return judged_.emplace(prefix, std::move(found)).first->second.has_value();
    }

    const std::optional<tobii::games::Game>& Discovered(const std::string& prefix) const {
        static const std::optional<tobii::games::Game> kNone;
        const auto it = judged_.find(prefix);
        return it == judged_.end() ? kNone : it->second;
    }

    std::string NameFor(const tobii::games::Catalogue& catalogue,
                        const std::string& prefix) const {
        if (const auto* known = catalogue.ForPrefix(prefix); known != nullptr) return known->name;
        const auto& discovered = Discovered(prefix);
        return discovered ? discovered->name : std::string();
    }

private:
    std::map<std::string, std::optional<tobii::games::Game>> judged_;
};

bool GameStillRunning(const tobii::games::Catalogue& catalogue, PrefixJudge& judge) {
    for (const auto& prefix : tobii::wine::FindPrefixes()) {
        if (!prefix.has_game_process) continue;
        if (judge.Serves(catalogue, prefix.path)) return true;
    }
    return false;
}

struct Bridge {
    pid_t pid = -1;
    int out_fd = -1;
    std::string pipe;
};

void StopBridge(Bridge& bridge) {
    ::kill(bridge.pid, SIGTERM);
    int status = 0;
    ::waitpid(bridge.pid, &status, 0);
    if (bridge.out_fd >= 0) ::close(bridge.out_fd);
    bridge.out_fd = -1;
}

std::string GameLabel(const std::string& name, const std::string& prefix) {
    if (!name.empty()) return name;
    const std::size_t slash = prefix.find_last_of('/');
    return slash == std::string::npos ? prefix : prefix.substr(slash + 1);
}

int ConnectDaemon(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::vector<std::uint8_t> HexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) return {};
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

}

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) opt.daemon_socket = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            opt.middleware_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--headpose-port" && i + 1 < argc)
            opt.headpose_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--bridge" && i + 1 < argc) opt.bridge_exe = argv[++i];
        else if (a == "--no-bridge") opt.manage_bridge = false;
        else if (a == "--profile" && i + 1 < argc) opt.profile_path = argv[++i];
        else if (a == "--serial" && i + 1 < argc) {
            opt.device_serial = argv[++i];
            opt.serial_from_flag = true;
        }
        else if (a == "--games" && i + 1 < argc) opt.games_path = argv[++i];
        else if (a == "--display" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d",
                            &opt.display_override_w, &opt.display_override_h) != 2 ||
                opt.display_override_w <= 0 || opt.display_override_h <= 0) {
                std::fprintf(stderr, "--display wants WxH, e.g. 2560x1440\n");
                return 2;
            }
        }
        else if (a == "--help") {
            std::printf("usage: tobii-adapter [--socket PATH] [--port N]"
                        " [--headpose-port N] [--bridge EXE] [--no-bridge]"
                        " [--display WxH]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    if (opt.manage_bridge) {
        const std::vector<std::string> candidates = !opt.bridge_exe.empty()
            ? std::vector<std::string>{opt.bridge_exe}
            : std::vector<std::string>{
                  tobii::paths::SiblingDir("lib/tobii-faithful/tobii-sesp-bridge.exe"),
                  TOBII_BRIDGE_INSTALL,
                  TOBII_BRIDGE_BUILD,
              };
        opt.bridge_exe = tobii::paths::FirstExisting(candidates);
        if (opt.bridge_exe.empty()) {
            opt.manage_bridge = false;
            tobii::eventlog::Record("Adapter: no Wine bridge, so head pose will not reach games "
                                    "(looked in " + tobii::paths::Describe(candidates) + ")");
        }
    }

    tobii::middleware::Server middleware;
    if (!middleware.Listen(opt.middleware_host, opt.middleware_port)) {
        tobii::eventlog::Record("Adapter failed to start: " + middleware.error());
        return 1;
    }

    tobii::games::Catalogue catalogue;
    catalogue.Load(opt.games_path);

    PrefixJudge judge;

    int display_w = 0, display_h = 0;
    {
        tobii::calibration::Profile profile;
        const bool have_profile = profile.Load(opt.profile_path);
        if (opt.display_override_w > 0) {
            display_w = opt.display_override_w;
            display_h = opt.display_override_h;
        } else if (have_profile && profile.display.measured) {
            display_w = profile.display.pixel_width;
            display_h = profile.display.pixel_height;
        } else if (const auto detected = DetectPrimaryDisplay(); detected.found) {
            display_w = detected.width;
            display_h = detected.height;
        } else if (have_profile && profile.display.pixel_width > 0) {
            display_w = profile.display.pixel_width;
            display_h = profile.display.pixel_height;
        }
        if (display_w > 0 && display_h > 0) {
            middleware.OverrideReply(tobii::middleware::kObjectDisplayArea,
                                     tobii::middleware::BuildDisplayAreaPayload(display_w, display_h));
        } else {
            tobii::eventlog::Record("Adapter: could not determine the display, so games are "
                                    "likely to refuse to start tracking - run screen setup in "
                                    "the manager, or pass --display WxH");
        }
    }

    const int pose_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in pose_addr{};
    pose_addr.sin_family = AF_INET;
    pose_addr.sin_port = htons(opt.headpose_port);
    ::inet_pton(AF_INET, "127.0.0.1", &pose_addr.sin_addr);

    int daemon_fd = -1;
    bool daemon_miss_recorded = false;
    std::vector<Bridge> bridges;
    int bridge_failures = 0;
    bool bridge_death_recorded = false;
    std::string bridge_prefix;
    double last_prefix_scan_s = 0.0;
    double last_liveness_s = 0.0;
    double bridge_up_since_s = 0.0;
    bool game_running = false;
    std::string game_label;
    bool gaze_recorded = false;
    bool licence_recorded = false;
    const auto record_licence = [&] {
        if (licence_recorded) return;
        licence_recorded = true;
        tobii::eventlog::Record("Game " + (game_label.empty() ? std::string("(unknown)")
                                                              : game_label) +
                                " provided a licence, but licences are not implemented"
                                " - serving consumer level only");
    };
    bool subscribed = false;
    std::string buffer;
    double last_presence_s = 0.0;
    double last_connect_try_s = 0.0;
    double last_devinfo_try_s = 0.0;
    bool device_info_known = false;
    double serial_wait_until_s = 0.0;
    std::uint64_t counter = 0;
    bool last_face = false;
    bool official_mode = false;

    while (!g_stop) {
        const double now = NowSeconds();

        const bool wanted = game_running || middleware.subscriber_count() > 0;

        if (wanted && daemon_fd < 0 && now - last_connect_try_s > 1.0) {
            last_connect_try_s = now;
            daemon_fd = ConnectDaemon(opt.daemon_socket);
            if (daemon_fd >= 0) {
                const std::string sub =
                    "{\"cmd\":\"subscribe\",\"stream\":\"pose\",\"hold\":true}\n";
                ::send(daemon_fd, sub.data(), sub.size(), MSG_NOSIGNAL);
                subscribed = true;
                device_info_known = false;
                last_devinfo_try_s = 0.0;
                buffer.clear();
                daemon_miss_recorded = false;
            } else if (!daemon_miss_recorded) {
                daemon_miss_recorded = true;
                tobii::eventlog::Record("Adapter: the tracking service (tobiid) is not running, "
                                        "so the game gets no tracking");
            }
        }

        if (!wanted && daemon_fd >= 0) {
            ::close(daemon_fd);
            daemon_fd = -1;
            subscribed = false;
        }

        const double devinfo_retry_s = serial_wait_until_s > 0.0 ? 0.25 : 1.0;
        if (daemon_fd >= 0 && subscribed && !device_info_known &&
            now - last_devinfo_try_s > devinfo_retry_s) {
            last_devinfo_try_s = now;
            static const std::string req = "{\"cmd\":\"deviceinfo\"}\n";
            ::send(daemon_fd, req.data(), req.size(), MSG_NOSIGNAL);
        }

        if (opt.manage_bridge && bridges.empty() && now - last_prefix_scan_s > 0.4) {
            last_prefix_scan_s = now;
            game_running = false;
            for (const auto& prefix : tobii::wine::FindPrefixes()) {
                if (!judge.Serves(catalogue, prefix.path)) continue;
                if (!prefix.has_game_process) continue;
                game_running = true;

                if (catalogue.ForPrefix(prefix.path) == nullptr) {
                    if (const auto& discovered = judge.Discovered(prefix.path); discovered) {
                        if (catalogue.Merge({*discovered}) > 0) {
                            catalogue.Save(opt.games_path);
                        }
                    }
                }

                if (!device_info_known && !opt.serial_from_flag) {
                    if (serial_wait_until_s == 0.0) serial_wait_until_s = now + kSerialWaitS;
                    if (now < serial_wait_until_s) break;
                }
                serial_wait_until_s = 0.0;

                struct Instance {
                    std::string pipe;
                    std::vector<std::string> args;
                };
                std::vector<Instance> instances;

                std::vector<std::string> sesp_args{
                    "--seconds", "86400",
                    "--sesp-connect-reply", "1",
                    "--sesp-auto-reply", "3",
                    "--sesp-headpose-udp", std::to_string(opt.headpose_port),
                    "--device-serial", opt.device_serial,
                };
                if (display_w > 0 && display_h > 0) {
                    sesp_args.insert(sesp_args.end(), {
                        "--display-name", "\\\\.\\DISPLAY1",
                        "--display-device-id", "DISPLAY\\DEFAULT_MONITOR\\0000&0000",
                        "--display-width", std::to_string(display_w),
                        "--display-height", std::to_string(display_h),
                    });
                }
                instances.push_back({"\\\\.\\pipe\\streamengineservices", std::move(sesp_args)});
                instances.push_back({"\\\\.\\pipe\\ETDefaultPIPE", {
                    "--seconds", "86400",
                    "--pipe", "\\\\.\\pipe\\ETDefaultPIPE",
                    "--etdefaultpipe",
                    "--etdefault-entry", opt.middleware_host,
                }});
                instances.push_back({"\\\\.\\pipe\\TOBII-" + opt.middleware_host, {
                    "--seconds", "86400",
                    "--pipe", "\\\\.\\pipe\\TOBII-" + opt.middleware_host,
                }});
                instances.push_back({"\\\\.\\pipe\\TOBIIPRP-" + opt.device_serial, {
                    "--seconds", "86400",
                    "--pipe", "\\\\.\\pipe\\TOBIIPRP-" + opt.device_serial,
                }});

                for (auto& instance : instances) {
                    Bridge bridge;
                    bridge.pid = tobii::wine::LaunchInPrefix(prefix.path, opt.bridge_exe,
                                                             instance.args, prefix.wine_binary,
                                                             &bridge.out_fd);
                    if (bridge.pid > 0) {
                        bridge.pipe = std::move(instance.pipe);
                        bridges.push_back(std::move(bridge));
                    }
                }
                if (!bridges.empty()) {
                    bridge_prefix = prefix.path;
                    bridge_up_since_s = now;
                    game_label = GameLabel(judge.NameFor(catalogue, prefix.path), prefix.path);
                    gaze_recorded = false;
                    licence_recorded = false;
                    tobii::eventlog::Record("Game " + game_label + " started");
                }
                break;
            }
        }

        if (!bridges.empty()) {
            for (auto& bridge : bridges) {
                if (bridge.out_fd < 0) continue;
                char said[256];
                const ssize_t n = ::read(bridge.out_fd, said, sizeof(said));
                if (n > 0) {
                    const std::string text(said, static_cast<std::size_t>(n));
                    if (text.find("subscribed") != std::string::npos)
                        tobii::eventlog::Record("Game " + game_label + " subscribed to " +
                                                bridge.pipe);
                    if (text.find("licence") != std::string::npos) record_licence();
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    ::close(bridge.out_fd);
                    bridge.out_fd = -1;
                }
            }

            for (std::size_t i = bridges.size(); i-- > 0;) {
                int status = 0;
                if (::waitpid(bridges[i].pid, &status, WNOHANG) == bridges[i].pid) {
                    if (bridges[i].out_fd >= 0) ::close(bridges[i].out_fd);
                    bridges.erase(bridges.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
            if (bridges.empty()) {
                const bool worked = bridge_up_since_s > 0.0 && now - bridge_up_since_s > 30.0;
                if (worked) {
                    bridge_failures = 0;
                    bridge_death_recorded = false;
                } else if (!bridge_death_recorded) {
                    bridge_death_recorded = true;
                    tobii::eventlog::Record("Game " + game_label + ": the pipe servers exited "
                                            "straight away, so the game gets no head pose");
                }
                ++bridge_failures;
                last_prefix_scan_s = now + std::min(30.0, 2.0 * bridge_failures);
                bridge_prefix.clear();
                bridge_up_since_s = 0.0;
            } else if (now - last_liveness_s > 0.4) {
                last_liveness_s = now;
                if (!GameStillRunning(catalogue, judge)) {
                    for (auto& bridge : bridges) StopBridge(bridge);
                    bridges.clear();
                    bridge_prefix.clear();
                    bridge_up_since_s = 0.0;
                    game_running = false;
                }
            }
        }

        pollfd fds[1];
        int nfds = 0;
        if (daemon_fd >= 0) fds[nfds++] = {daemon_fd, POLLIN, 0};
        if (nfds > 0) ::poll(fds, static_cast<nfds_t>(nfds), 0);

        if (daemon_fd >= 0 && (fds[0].revents & (POLLHUP | POLLERR)) != 0) {
            ::close(daemon_fd);
            daemon_fd = -1;
            subscribed = false;
        } else if (daemon_fd >= 0 && (fds[0].revents & POLLIN) != 0) {
            char buf[8192];
            const ssize_t n = ::read(daemon_fd, buf, sizeof(buf));
            if (n <= 0) {
                ::close(daemon_fd);
                daemon_fd = -1;
                subscribed = false;
            } else {
                buffer.append(buf, static_cast<std::size_t>(n));
                std::size_t start = 0;
                for (;;) {
                    const std::size_t nl = buffer.find('\n', start);
                    if (nl == std::string::npos) break;
                    const std::string line = buffer.substr(start, nl - start);
                    start = nl + 1;

                    if (!device_info_known &&
                        tobii::ipc::JsonField(line, "type") == "deviceinfo") {
                        const std::string serial = tobii::ipc::JsonField(line, "serial");
                        if (!serial.empty() && !opt.serial_from_flag) {
                            opt.device_serial = serial;
                        }
                        const auto apply = [&](const char* field, std::uint32_t object) {
                            const auto bytes = HexToBytes(tobii::ipc::JsonField(line, field));
                            if (!bytes.empty()) middleware.OverrideReply(object, bytes);
                        };
                        apply("device_info", 0x058cu);
                        apply("device_config", 0x0532u);
                        apply("capabilities", 0x0546u);
                        apply("stream_catalog", 0x04b0u);
                        device_info_known = true;
                        continue;
                    }

                    tobii::adapter::PosePacket packet{};
                    if (tobii::adapter::HeadPosePacketFromLine(line, counter, packet)) {
                        official_mode = true;
                        ++counter;
                        ::sendto(pose_fd, &packet, sizeof(packet), 0,
                                 reinterpret_cast<sockaddr*>(&pose_addr), sizeof(pose_addr));
                        continue;
                    }
                    if (!tobii::adapter::PosePacketFromLine(line, counter, packet)) continue;
                    last_face = tobii::adapter::PresentFromLine(line);
                    middleware.PublishGaze(packet.gaze_x, packet.gaze_y,
                                           packet.gaze_valid != 0.0);
                    if (official_mode) continue;
                    ++counter;
                    ::sendto(pose_fd, &packet, sizeof(packet), 0,
                             reinterpret_cast<sockaddr*>(&pose_addr), sizeof(pose_addr));
                }
                buffer.erase(0, start);
            }
        }

        if (now - last_presence_s > 1.0 / opt.presence_hz) {
            last_presence_s = now;
            middleware.PublishPresence(subscribed && last_face);
        }

        if (middleware.subscriber_count() > 0 && !gaze_recorded) {
            gaze_recorded = true;
            tobii::eventlog::Record("Game " + (game_label.empty() ? std::string("(unknown)")
                                                                  : game_label) +
                                    " subscribed to the gaze stream on " +
                                    opt.middleware_host + ":" +
                                    std::to_string(opt.middleware_port));
        } else if (middleware.subscriber_count() == 0 && bridges.empty()) {
            gaze_recorded = false;
        }
        if (middleware.TakeLicenceOffered()) record_licence();

        if (!middleware.Poll(daemon_fd >= 0 ? 2 : 50)) {
            tobii::eventlog::Record("Adapter stopped: " + middleware.error());
            break;
        }
    }

    for (auto& bridge : bridges) StopBridge(bridge);
    if (daemon_fd >= 0) ::close(daemon_fd);
    ::close(pose_fd);
    middleware.Close();
    return 0;
}
