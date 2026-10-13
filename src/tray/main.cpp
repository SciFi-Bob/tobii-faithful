#include "tobii/about.hpp"
#include "tobii/eventlog.hpp"
#include "tobii/games/catalogue.hpp"
#include "tobii/ipc/protocol.hpp"
#include "tobii/manager/single_instance.hpp"
#include "tobii/paths.hpp"
#include "tobii/tray/eye_icon.hpp"

#include <systemd/sd-bus.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

struct State {
    bool connected = false;
    bool device_open = false;
    float fps = 0.0f;
    std::string socket_path;
    std::string manager_path;
    std::string daemon_path;
};

tobii::tray::EyeState IconState(const State& state) {
    if (!state.connected) return tobii::tray::EyeState::kNoDaemon;
    return state.device_open ? tobii::tray::EyeState::kWatching
                             : tobii::tray::EyeState::kSleeping;
}

State g_state;
int g_icon_size = 48;

void PollDaemon(State& state) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, state.socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        state.connected = false;
        state.device_open = false;
        return;
    }
    state.connected = true;

    const std::string query = "{\"cmd\":\"status\"}\n";
    ::send(fd, query.data(), query.size(), MSG_NOSIGNAL);

    timeval tv{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    const std::string line(buf);

    state.device_open = line.find("\"device\":\"open\"") != std::string::npos;
    const std::size_t fps = line.find("\"fps\":");
    if (fps != std::string::npos) state.fps = std::strtof(line.c_str() + fps + 6, nullptr);
}

pid_t FindDaemon() {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return 0;
    pid_t found = 0;
    while (const dirent* entry = ::readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        const std::string comm_path = std::string("/proc/") + entry->d_name + "/comm";
        std::ifstream f(comm_path);
        std::string comm;
        if (!(f >> comm) || comm != "tobiid") continue;
        const pid_t pid = static_cast<pid_t>(std::atoi(entry->d_name));
        if (::kill(pid, 0) != 0 && errno == EPERM) continue;
        found = pid;
        break;
    }
    ::closedir(proc);
    return found;
}

void Systemctl(const char* verb) {
    if (::fork() != 0) return;
    ::setsid();
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        ::dup2(null_fd, STDIN_FILENO);
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) ::close(null_fd);
    }
    ::execlp("systemctl", "systemctl", "--user", verb, "tobiid", nullptr);
    std::_Exit(127);
}

void StartDaemon() {
    if (FindDaemon() != 0) return;
    Systemctl("start");
}

void StopDaemon() { Systemctl("stop"); }

void OpenManager() {
    if (::fork() != 0) return;
    ::setsid();
    ::execlp(g_state.manager_path.c_str(), g_state.manager_path.c_str(), nullptr);
    std::_Exit(127);
}

void OpenAbout() {
    if (::fork() != 0) return;
    ::setsid();
    ::execlp(g_state.manager_path.c_str(), g_state.manager_path.c_str(), "--about", nullptr);
    std::_Exit(127);
}

int PropertyIcon(sd_bus*  , const char*  , const char*  ,
                 const char*  , sd_bus_message* reply, void*  ,
                 sd_bus_error*  ) {
    const auto pixels = tobii::tray::RenderEye(g_icon_size, IconState(g_state));
    int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
    if (r < 0) return r;
    r = sd_bus_message_open_container(reply, 'r', "iiay");
    if (r < 0) return r;
    r = sd_bus_message_append(reply, "ii", g_icon_size, g_icon_size);
    if (r < 0) return r;
    r = sd_bus_message_append_array(reply, 'y', pixels.data(), pixels.size());
    if (r < 0) return r;
    r = sd_bus_message_close_container(reply);
    if (r < 0) return r;
    return sd_bus_message_close_container(reply);
}

int PropertyToolTip(sd_bus*  , const char*  , const char*  ,
                    const char*  , sd_bus_message* reply, void*  ,
                    sd_bus_error*  ) {
    std::string detail;
    if (!g_state.connected) {
        detail = "Service not running";
    } else if (g_state.device_open) {
        detail = "Awake";
    } else {
        detail = "Asleep";
    }

    int r = sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
    if (r < 0) return r;
    r = sd_bus_message_append(reply, "s", "");
    if (r < 0) return r;
    r = sd_bus_message_open_container(reply, 'a', "(iiay)");
    if (r < 0) return r;
    r = sd_bus_message_close_container(reply);
    if (r < 0) return r;
    r = sd_bus_message_append(reply, "ss", tobii::kProductName, detail.c_str());
    if (r < 0) return r;
    return sd_bus_message_close_container(reply);
}

int MethodActivate(sd_bus_message* m, void*  , sd_bus_error*  ) {
    OpenManager();
    return sd_bus_reply_method_return(m, "");
}

int PropertyString(sd_bus*  , const char*  , const char*  ,
                   const char* property, sd_bus_message* reply, void*  ,
                   sd_bus_error*  ) {
    const std::string name = property;
    if (name == "Category") return sd_bus_message_append(reply, "s", "ApplicationStatus");
    if (name == "Id") return sd_bus_message_append(reply, "s", "tobii-faithful");
    if (name == "Title") return sd_bus_message_append(reply, "s", tobii::kProductName);
    if (name == "Status") return sd_bus_message_append(reply, "s", "Active");
    if (name == "IconName") return sd_bus_message_append(reply, "s", "");
    if (name == "Menu") return sd_bus_message_append(reply, "o", "/MenuBar");
    return sd_bus_message_append(reply, "s", "");
}

int MethodIgnore(sd_bus_message* m, void*  , sd_bus_error*  ) {
    return sd_bus_reply_method_return(m, "");
}

int PropertyBool(sd_bus*  , const char*  , const char*  ,
                 const char*  , sd_bus_message* reply, void*  ,
                 sd_bus_error*  ) {
    return sd_bus_message_append(reply, "b", 0);
}

constexpr int kMenuShow = 1;
constexpr int kMenuDaemon = 2;
constexpr int kMenuAbout = 3;
constexpr int kMenuQuit = 4;

std::uint32_t g_menu_revision = 1;

struct MenuItem {
    int id;
    const char* label;
    bool separator;
};

std::vector<MenuItem> MenuItems() {
    return {
        {kMenuShow, "Open Faithful Tobii", false},
        {0, nullptr, true},
        {kMenuDaemon, FindDaemon() != 0 ? "Stop service" : "Start service", false},
        {0, nullptr, true},
        {kMenuAbout, "About", false},
        {kMenuQuit, "Quit", false},
    };
}

int AppendMenuProperties(sd_bus_message* m, const MenuItem& item) {
    int r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (r < 0) return r;
    if (item.separator) {
        r = sd_bus_message_append(m, "{sv}", "type", "s", "separator");
        if (r < 0) return r;
    } else {
        r = sd_bus_message_append(m, "{sv}", "label", "s", item.label);
        if (r < 0) return r;
        r = sd_bus_message_append(m, "{sv}", "enabled", "b", 1);
        if (r < 0) return r;
        r = sd_bus_message_append(m, "{sv}", "visible", "b", 1);
        if (r < 0) return r;
    }
    return sd_bus_message_close_container(m);
}

int MethodGetLayout(sd_bus_message* m, void*  , sd_bus_error*  ) {
    int parent = 0, depth = 0;
    int r = sd_bus_message_read(m, "ii", &parent, &depth);
    if (r < 0) return r;
    r = sd_bus_message_skip(m, "as");
    if (r < 0) return r;

    sd_bus_message* reply = nullptr;
    r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    r = sd_bus_message_append(reply, "u", g_menu_revision);
    if (r < 0) return r;

    r = sd_bus_message_open_container(reply, 'r', "ia{sv}av");
    if (r < 0) return r;
    r = sd_bus_message_append(reply, "i", 0);
    if (r < 0) return r;
    r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0) return r;
    r = sd_bus_message_append(reply, "{sv}", "children-display", "s", "submenu");
    if (r < 0) return r;
    r = sd_bus_message_close_container(reply);
    if (r < 0) return r;

    r = sd_bus_message_open_container(reply, 'a', "v");
    if (r < 0) return r;
    int next_id = 10;
    for (const auto& item : MenuItems()) {
        MenuItem entry = item;
        if (entry.separator) entry.id = next_id++;
        r = sd_bus_message_open_container(reply, 'v', "(ia{sv}av)");
        if (r < 0) return r;
        r = sd_bus_message_open_container(reply, 'r', "ia{sv}av");
        if (r < 0) return r;
        r = sd_bus_message_append(reply, "i", entry.id);
        if (r < 0) return r;
        r = AppendMenuProperties(reply, entry);
        if (r < 0) return r;
        r = sd_bus_message_open_container(reply, 'a', "v");
        if (r < 0) return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0) return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0) return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0) return r;
    }
    r = sd_bus_message_close_container(reply);
    if (r < 0) return r;
    r = sd_bus_message_close_container(reply);
    if (r < 0) return r;

    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

int MethodGetGroupProperties(sd_bus_message* m, void*  , sd_bus_error*  ) {
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    r = sd_bus_message_open_container(reply, 'a', "(ia{sv})");
    if (r < 0) return r;
    int next_id = 10;
    for (const auto& item : MenuItems()) {
        MenuItem entry = item;
        if (entry.separator) entry.id = next_id++;
        r = sd_bus_message_open_container(reply, 'r', "ia{sv}");
        if (r < 0) return r;
        r = sd_bus_message_append(reply, "i", entry.id);
        if (r < 0) return r;
        r = AppendMenuProperties(reply, entry);
        if (r < 0) return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0) return r;
    }
    r = sd_bus_message_close_container(reply);
    if (r < 0) return r;
    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

int MethodMenuEvent(sd_bus_message* m, void*  , sd_bus_error*  ) {
    int id = 0;
    const char* event = nullptr;
    int r = sd_bus_message_read(m, "is", &id, &event);
    if (r < 0) return r;
    if (event != nullptr && std::string(event) == "clicked") {
        switch (id) {
            case kMenuShow:   OpenManager(); break;
            case kMenuDaemon: FindDaemon() != 0 ? StopDaemon() : StartDaemon(); break;
            case kMenuAbout:  OpenAbout(); break;
            case kMenuQuit:   g_stop = true; break;
            default: break;
        }
    }
    return sd_bus_reply_method_return(m, "");
}

int MethodAboutToShow(sd_bus_message* m, void*  , sd_bus_error*  ) {
    ++g_menu_revision;
    return sd_bus_reply_method_return(m, "b", 1);
}

int MenuProperty(sd_bus*  , const char*  , const char*  ,
                 const char* property, sd_bus_message* reply, void*  ,
                 sd_bus_error*  ) {
    const std::string name = property;
    if (name == "Version") return sd_bus_message_append(reply, "u", 3);
    if (name == "Status") return sd_bus_message_append(reply, "s", "normal");
    if (name == "TextDirection") return sd_bus_message_append(reply, "s", "ltr");
    return sd_bus_message_append(reply, "s", "");
}

int MenuIconThemePath(sd_bus*  , const char*  , const char*  ,
                      const char*  , sd_bus_message* reply, void*  ,
                      sd_bus_error*  ) {
    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0) return r;
    return sd_bus_message_close_container(reply);
}

const sd_bus_vtable kMenuVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TextDirection", "s", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconThemePath", "as", MenuIconThemePath, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", MethodGetLayout,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", MethodGetGroupProperties,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", MethodMenuEvent, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShow", "i", "b", MethodAboutToShow, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ias)", 0),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kItemVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Category", "s", PropertyString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", PropertyString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Title", "s", PropertyString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", PropertyString, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IconName", "s", PropertyString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", PropertyIcon, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", PropertyToolTip, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ItemIsMenu", "b", PropertyBool, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Menu", "o", PropertyString, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Activate", "ii", "", MethodActivate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", MethodActivate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Scroll", "is", "", MethodIgnore, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NewIcon", "", 0),
    SD_BUS_SIGNAL("NewToolTip", "", 0),
    SD_BUS_SIGNAL("NewStatus", "s", 0),
    SD_BUS_VTABLE_END,
};

}

int main(int argc, char** argv) {
    g_state.socket_path = tobii::ipc::DefaultSocketPath();
    const std::string here = tobii::paths::ExecutableDir();
    g_state.manager_path = tobii::paths::FirstExisting(
        {here.empty() ? "" : here + "/tobii-manager", TOBII_MANAGER_INSTALL,
         TOBII_MANAGER_BUILD});
    g_state.daemon_path = tobii::paths::FirstExisting(
        {here.empty() ? "" : here + "/tobiid", TOBII_DAEMON_INSTALL, TOBII_DAEMON_BUILD});
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) g_state.socket_path = argv[++i];
        else if (a == "--manager" && i + 1 < argc) g_state.manager_path = argv[++i];
        else if (a == "--daemon" && i + 1 < argc) g_state.daemon_path = argv[++i];
        else if (a == "--help") {
            std::printf("usage: tobii-tray [--socket PATH] [--manager PATH]"
                        " [--daemon PATH]\n");
            return 0;
        }
    }

    if (g_state.manager_path.empty()) {
        tobii::eventlog::Record("Tray failed to start: cannot find tobii-manager");
        return 1;
    }

    tobii::manager::SingleInstance instance("tray");
    if (!instance.owner()) {
        OpenManager();
        return 0;
    }

    {
        const std::string games_path = tobii::games::Catalogue::DefaultPath();
        tobii::games::Catalogue catalogue;
        catalogue.Load(games_path);
        std::vector<std::string> searched;
        const auto found = catalogue.Scan(&searched);
        const std::size_t added = catalogue.Merge(found);
        tobii::games::RecordSearch(searched, found.size(), added);
        if (added > 0) catalogue.Save(games_path);
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGCHLD, SIG_IGN);

    sd_bus* bus = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        tobii::eventlog::Record(std::string("Tray failed to start: no session bus: ") +
                                std::strerror(-r));
        return 1;
    }

    r = sd_bus_add_object_vtable(bus, nullptr, "/StatusNotifierItem",
                                 "org.kde.StatusNotifierItem", kItemVtable, nullptr);
    if (r >= 0) {
        r = sd_bus_add_object_vtable(bus, nullptr, "/MenuBar",
                                     "com.canonical.dbusmenu", kMenuVtable, nullptr);
    }
    if (r < 0) {
        tobii::eventlog::Record(std::string("Tray failed to start: could not export the item: ") +
                                std::strerror(-r));
        return 1;
    }

    const std::string name = "org.freedesktop.StatusNotifierItem-" +
                             std::to_string(::getpid()) + "-1";
    r = sd_bus_request_name(bus, name.c_str(), 0);
    if (r < 0) {
        tobii::eventlog::Record(std::string("Tray failed to start: could not take the bus name: ") +
                                std::strerror(-r));
        return 1;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           &error, nullptr, "s", name.c_str());
    if (r < 0) {
        tobii::eventlog::Record(std::string("Tray failed to start: no status notifier host is "
                                            "listening (") +
                                (error.message != nullptr ? error.message : std::strerror(-r)) +
                                "); run tobii-manager directly");
        sd_bus_error_free(&error);
        return 1;
    }
    sd_bus_error_free(&error);

    bool was_open = false;
    auto last_poll = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    while (!g_stop) {
        sd_bus_process(bus, nullptr);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_poll > std::chrono::seconds(2)) {
            last_poll = now;
            PollDaemon(g_state);
            if (g_state.device_open != was_open) {
                was_open = g_state.device_open;
                sd_bus_emit_signal(bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
                                   "NewIcon", "");
            }
            sd_bus_emit_signal(bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
                               "NewToolTip", "");
        }

        sd_bus_wait(bus, 200 * 1000);
    }

    sd_bus_unref(bus);
    return 0;
}
