#include "tobii/ipc/protocol.hpp"
#include "tobii/manager/client.hpp"
#include "tobii/about.hpp"
#include "tobii/eventlog.hpp"
#include "tobii/games/catalogue.hpp"
#include "tobii/manager/axis_convention.hpp"
#include "tobii/manager/calibration_ui.hpp"
#include "tobii/manager/head_view.hpp"
#include "tobii/manager/pose_easing.hpp"
#include "tobii/manager/single_instance.hpp"
#include "tobii/tray/eye_icon.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <string>
#include <vector>
#include <vector>

namespace {

constexpr float kAxesPaneWidth = 480.0f;
constexpr float kPaneHeight = 512.0f;

std::string DefaultProfilePath() {
    return tobii::calibration::Profile::DefaultPath();
}

void PresentWindow(GLFWwindow* window) {
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE) {
        glfwRestoreWindow(window);
    }
    glfwShowWindow(window);
    glfwFocusWindow(window);
    glfwRequestWindowAttention(window);
}

bool FitWindowToContent(GLFWwindow* window, float content_w, float content_h) {
    int want_w = static_cast<int>(content_w + 0.5f);
    int want_h = static_cast<int>(content_h + 0.5f);

    int room_w = 0, room_h = 0;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        room_w = std::max(room_w, mw);
        room_h = std::max(room_h, mh);
    }
    const int asked_w = want_w, asked_h = want_h;
    if (room_w > 0) want_w = std::min(want_w, room_w);
    if (room_h > 0) want_h = std::min(want_h, room_h);
    const bool clamped = want_w < asked_w || want_h < asked_h;

    int have_w = 0, have_h = 0;
    glfwGetWindowSize(window, &have_w, &have_h);
    if (std::abs(have_w - want_w) <= 1 && std::abs(have_h - want_h) <= 1) return clamped;

    glfwSetWindowSizeLimits(window, GLFW_DONT_CARE, GLFW_DONT_CARE,
                            GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwSetWindowSize(window, want_w, want_h);
    glfwSetWindowSizeLimits(window, want_w, want_h, want_w, want_h);
    return clamped;
}

template <typename OnClick>
void DoneButton(const char* label, ImVec2 size, bool done, OnClick&& on_click) {
    if (done) {
        ImGui::PushStyleColor(ImGuiCol_Button, {0.16f, 0.46f, 0.27f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.21f, 0.58f, 0.34f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.13f, 0.38f, 0.22f, 1.0f});
    }
    if (ImGui::Button(label, size)) on_click();
    if (done) ImGui::PopStyleColor(3);
}

}

int main(int argc, char** argv) {
    std::string socket_path = tobii::ipc::DefaultSocketPath();
    std::string start_routine;
    bool about_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) socket_path = argv[++i];
        else if (a == "--calibrate" && i + 1 < argc) start_routine = argv[++i];
        else if (a == "--screen-setup") start_routine = "screen";
        else if (a == "--setup") start_routine = "setup";
        else if (a == "--about") about_only = true;
        else if (a == "--help") {
            std::printf("usage: tobii-manager [--socket PATH] [--calibrate gaze|movement]"
                        " [--about]\n");
            return 0;
        }
    }

    tobii::manager::SingleInstance instance(about_only ? "about" : "manager");
    if (!instance.owner()) {
        std::string request;
        if (!start_routine.empty()) request = "calibrate " + start_routine;
        instance.NudgeOwner(request);
        return 0;
    }

    if (glfwInit() == GLFW_FALSE) {
        tobii::eventlog::Record("Manager failed to start: could not initialise GLFW");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "tobii-manager");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "tobii-manager");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "tobii-manager");
    if (about_only) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    const std::string about_title = std::string("About ") + tobii::kProductName;
    GLFWwindow* window = about_only
        ? glfwCreateWindow(760, 700, about_title.c_str(), nullptr, nullptr)
        : glfwCreateWindow(1180, 760, tobii::kProductName, nullptr, nullptr);
    if (window == nullptr) {
        tobii::eventlog::Record("Manager failed to start: could not create a window");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    {
        constexpr int kIconSize = 64;
        const std::vector<std::uint8_t> argb =
            tobii::tray::RenderEye(kIconSize, tobii::tray::EyeState::kWatching);
        std::vector<unsigned char> rgba(argb.size());
        for (std::size_t i = 0; i + 3 < argb.size(); i += 4) {
            rgba[i + 0] = argb[i + 1];
            rgba[i + 1] = argb[i + 2];
            rgba[i + 2] = argb[i + 3];
            rgba[i + 3] = argb[i + 0];
        }
        if (!rgba.empty()) {
            GLFWimage icon{kIconSize, kIconSize, rgba.data()};
            glfwSetWindowIcon(window, 1, &icon);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui::GetStyle().WindowPadding = {16.0f, 14.0f};
    ImGui::GetStyle().ItemSpacing = {10.0f, 8.0f};
    ImGui::GetStyle().FramePadding = {6.0f, 4.0f};
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    tobii::manager::Client client(socket_path);
    if (!about_only) client.Start();
    tobii::manager::CalibrationUi calibration(client, DefaultProfilePath());

    const std::string games_path = tobii::games::Catalogue::DefaultPath();
    tobii::games::Catalogue catalogue;
    catalogue.Load(games_path);
    std::vector<std::string> searched;
    const auto scanned = catalogue.Scan(&searched);
    const std::size_t onboarded = catalogue.Merge(scanned);
    tobii::games::RecordSearch(searched, scanned.size(), onboarded);
    if (onboarded > 0) catalogue.Save(games_path);
    std::string games_status =
        catalogue.games().empty() ? "no games with the Tobii integration found yet"
      : onboarded > 0 ? (std::to_string(onboarded) + " new game(s) found and registered")
      : "";
    char new_root[512] = {0};
    if (start_routine == "setup") calibration.StartSetup();
    else if (start_routine == "screen") calibration.StartScreenSetup();
    else if (start_routine == "gaze") calibration.StartGaze();

    bool monitor_resolved = false;
    bool content_clipped = false;
    int frames_drawn = 0;
    int resolved_count = 0;

    bool holding_device = true;
    tobii::manager::PoseEasing easing;
    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        glfwPollEvents();

        if (!about_only) {
            const bool want =
                glfwGetWindowAttrib(window, GLFW_ICONIFIED) != GLFW_TRUE;
            if (want != holding_device) {
                holding_device = want;
                client.SetHold(want);
            }
        }

        if (std::string request; instance.TakeNudge(&request)) {
            PresentWindow(window);
            if (request.rfind("calibrate ", 0) == 0) {
                const std::string which = request.substr(10);
                if (which == "setup") calibration.StartSetup();
                else if (which == "screen") calibration.StartScreenSetup();
                else if (which == "gaze") calibration.StartGaze();
            }
        }

        if (about_only) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            static bool sized = false;
            if (!sized) {
                sized = true;
                const ImGuiStyle& style = ImGui::GetStyle();
                const ImVec2 body = ImGui::CalcTextSize(tobii::kAboutText);
                const ImVec2 heading = ImGui::CalcTextSize(tobii::kProductName);
                const int w = static_cast<int>(
                    std::max(body.x, heading.x) + style.WindowPadding.x * 2.0f + 8.0f);
                const int h = static_cast<int>(
                    heading.y + body.y + 32.0f + style.WindowPadding.y * 2.0f +
                    style.ItemSpacing.y * 5.0f + 6.0f);
                glfwSetWindowSize(window, w, h);
                glfwSetWindowSizeLimits(window, w, h, w, h);
            }
            const ImGuiViewport* about_viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(about_viewport->WorkPos);
            ImGui::SetNextWindowSize(about_viewport->WorkSize);
            ImGui::Begin("about", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar);
            ImGui::TextUnformatted(tobii::kProductName);
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted(tobii::kAboutText);
            ImGui::Spacing();
            if (ImGui::Button("Close", {120, 32})) glfwSetWindowShouldClose(window, GLFW_TRUE);
            ImGui::End();
            ImGui::Render();
            int aw = 0, ah = 0;
            glfwGetFramebufferSize(window, &aw, &ah);
            glViewport(0, 0, aw, ah);
            glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto snapshot = client.GetSnapshot();

        const float raw_pose[6] = {snapshot.pose[0], snapshot.pose[1], snapshot.pose[2],
                                   snapshot.pose[3], snapshot.pose[4], snapshot.pose[5]};
        const bool pos_valid = snapshot.pos_valid;
        const bool rot_valid = snapshot.face;
        float ep[6];
        easing.Update(snapshot.counter, raw_pose, pos_valid, rot_valid, ep);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const double now_s =
            std::chrono::duration<double>(now.time_since_epoch()).count();
        if (calibration.Draw(window, snapshot, now_s)) {
            ImGui::Render();
            int fw = 0, fh = 0;
            glfwGetFramebufferSize(window, &fw, &fh);
            glViewport(0, 0, fw, fh);
            glClearColor(0.05f, 0.055f, 0.07f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            continue;
        }

        if (monitor_resolved && calibration.monitor() < 0) {
            int now = 0;
            glfwGetMonitors(&now);
            if (now != resolved_count) {
                resolved_count = now;
                calibration.ResolveInitialMonitor(window);
            }
        }
        if (!monitor_resolved && ++frames_drawn > 3) {
            monitor_resolved = true;
            glfwGetMonitors(&resolved_count);
            calibration.ResolveInitialMonitor(window);
            const int mon = calibration.monitor();
            int mcount = 0;
            GLFWmonitor** mons = glfwGetMonitors(&mcount);
            if (mon >= 0 && mon < mcount) {
                int mx = 0, my = 0, mw = 0, mh = 0;
                glfwGetMonitorWorkarea(mons[mon], &mx, &my, &mw, &mh);
                int ww = 0, wh = 0;
                glfwGetWindowSize(window, &ww, &wh);
                glfwSetWindowPos(window, mx + (mw - ww) / 2, my + (mh - wh) / 2);
            }
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("tobii", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     (content_clipped ? 0 : ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoScrollWithMouse) |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (!snapshot.connected) {
            ImGui::TextColored({1.0f, 0.45f, 0.45f, 1.0f},
                               "Not connected to tobiid  (%s)", socket_path.c_str());
            ImGui::Separator();
        } else if (!snapshot.error.empty()) {
            ImGui::TextColored({1.0f, 0.5f, 0.5f, 1.0f}, "Error: %s", snapshot.error.c_str());
            ImGui::Separator();
        }

        tobii::manager::HeadViewState head;
        head.tracked = snapshot.face;
        constexpr float kRadToDeg = 57.29577951308232f;
        head.yaw   = ep[3] * kRadToDeg * tobii::manager::DisplaySign(3);
        head.pitch = ep[4] * kRadToDeg * tobii::manager::DisplaySign(4);
        head.roll  = ep[5] * kRadToDeg * tobii::manager::DisplaySign(5);
        constexpr float kNominalDistanceMm = 800.0f;
        head.translation_mm[0] = ep[0] * tobii::manager::DisplaySign(0);
        head.translation_mm[1] = ep[1] * tobii::manager::DisplaySign(1);
        head.translation_mm[2] = (ep[2] - kNominalDistanceMm) *
                                 tobii::manager::DisplaySign(2);
        head.gaze_valid = snapshot.gaze_valid;
        head.gaze_x = snapshot.gaze_x;
        head.gaze_y = snapshot.gaze_y;
        head.left_eye_valid = snapshot.left_eye_valid;
        head.right_eye_valid = snapshot.right_eye_valid;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::BeginChild("head", {kPaneHeight, kPaneHeight}, true,
                          ImGuiWindowFlags_NoScrollbar);
        {
            const ImVec2 room = ImGui::GetContentRegionAvail();
            tobii::manager::DrawHead(head, std::min(room.x, room.y));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine();
        ImGui::BeginChild("axes", {kAxesPaneWidth, kPaneHeight}, true);
        const ImGuiStyle& pane_style = ImGui::GetStyle();
        const float content_right = 190.0f * 2.0f + pane_style.ItemSpacing.x;
        const float axes_side_pad =
            std::max(0.0f, (ImGui::GetContentRegionAvail().x - content_right) * 0.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + axes_side_pad);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::BeginChild("axes_center", {content_right, ImGui::GetContentRegionAvail().y},
                          false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        static float axes_content_h = 0.0f;
        const float axes_avail_h = ImGui::GetContentRegionAvail().y;
        const float axes_top_pad = axes_content_h > 0.0f
            ? std::max(0.0f, (axes_avail_h - axes_content_h) * 0.5f) : 0.0f;
        if (axes_top_pad > 0.0f) ImGui::Dummy({0.0f, axes_top_pad});
        const float axes_content_start = ImGui::GetCursorPosY();
        ImGui::SeparatorText("Output");
        ImGui::Spacing();
        {
            constexpr float kRadToDeg = 57.29577951308232f;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float label_col = 100.0f;
            const auto bar = [&](const char* label, float value, float centre, float span) {
                const float h = 18.0f;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine(label_col);
                const ImVec2 o = ImGui::GetCursorScreenPos();
                const float bw = content_right - label_col;
                const float mid = o.x + bw * 0.5f;
                draw->AddRectFilled({o.x, o.y}, {o.x + bw, o.y + h},
                                    IM_COL32(26, 31, 38, 255), 3.0f);
                draw->AddLine({mid, o.y}, {mid, o.y + h}, IM_COL32(90, 100, 115, 255));
                const float frac = std::clamp((value - centre) / span, -1.0f, 1.0f);
                const float end = mid + frac * bw * 0.5f;
                draw->AddRectFilled({std::min(mid, end), o.y + 2.0f},
                                    {std::max(mid, end), o.y + h - 2.0f},
                                    IM_COL32(110, 180, 255, 255), 2.0f);
                ImGui::Dummy({bw, h});
                ImGui::Spacing();
            };
            bar("Yaw",    ep[3] * kRadToDeg, 0.0f, 45.0f);
            bar("Pitch",  ep[4] * kRadToDeg, 0.0f, 30.0f);
            bar("Roll",   ep[5] * kRadToDeg, 0.0f, 30.0f);
            bar("Left/Right", ep[0], 0.0f,   200.0f);
            bar("Up/Down",    ep[1], 0.0f,   200.0f);
            bar("Fwd/Back",   ep[2], 800.0f, 200.0f);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Calibration");
        ImGui::Spacing();

        const int chosen_monitor = calibration.monitor();
        const bool screen_selected = chosen_monitor >= 0;
        {
            const std::vector<std::string> monitors =
                tobii::manager::CalibrationUi::MonitorNames();
            const bool valid = screen_selected &&
                               chosen_monitor < static_cast<int>(monitors.size());
            const char* preview = monitors.empty()
                                      ? "no displays"
                                      : valid ? monitors[static_cast<std::size_t>(chosen_monitor)].c_str()
                                              : "Choose a screen...";
            ImGui::TextUnformatted("Screen the tracker sits under");
            ImGui::SetNextItemWidth(content_right);
            if (ImGui::BeginCombo("##screen", preview)) {
                for (int i = 0; i < static_cast<int>(monitors.size()); ++i) {
                    const bool is_this = i == chosen_monitor;
                    if (ImGui::Selectable(monitors[static_cast<std::size_t>(i)].c_str(), is_this)) {
                        calibration.SetMonitor(i);
                    }
                    if (is_this) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Spacing();

        {
            const auto& profile = calibration.profile();
            const bool set_up = profile.display.measured;

            const char* eye_labels[3] = {"Both eyes (default)", "Left eye", "Right eye"};
            const int eye_values[3] = {3, 1, 2};
            const int eye_index = calibration.enabled_eye() == 1 ? 1
                                : calibration.enabled_eye() == 2 ? 2 : 0;
            ImGui::TextUnformatted("Eyes to calibrate");
            ImGui::SetNextItemWidth(content_right);
            if (ImGui::BeginCombo("##eyes", eye_labels[eye_index])) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(eye_labels[i], i == eye_index)) {
                        calibration.set_enabled_eye(eye_values[i]);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Spacing();

            ImGui::BeginDisabled(!screen_selected || snapshot.game);
            DoneButton("Calibrate", {190, 32}, set_up,
                       [&] { calibration.StartSetup(); });
            ImGui::SameLine();
            ImGui::BeginDisabled(!set_up);
            DoneButton("Improve", {190, 32}, false,
                       [&] { calibration.StartGaze(true); });
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (snapshot.game) {
                ImGui::TextDisabled("A game is using the tracker. Close it to calibrate.");
            } else if (!screen_selected) {
                ImGui::TextDisabled("Choose the screen the tracker sits under to calibrate.");
            }

            if (calibration.mode() != tobii::manager::CalibrationUi::Mode::kIdle &&
                !calibration.status().empty()) {
                ImGui::TextDisabled("%s", calibration.status().c_str());
            }
            if (!snapshot.last_reply.empty()) {
                ImGui::TextColored(snapshot.last_reply_ok ? ImVec4{0.45f, 0.95f, 0.6f, 1.0f}
                                                          : ImVec4{1.0f, 0.55f, 0.5f, 1.0f},
                                   "%s", snapshot.last_reply.c_str());
            }
        }
        axes_content_h = ImGui::GetCursorPosY() - axes_content_start;
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::SeparatorText("Games");
        ImGui::Spacing();

        if (ImGui::Button("Detect Games", {160, 32})) {
            std::vector<std::string> searched;
            const auto found = catalogue.Scan(&searched);
            const std::size_t added = catalogue.Merge(found);
            tobii::games::RecordSearch(searched, found.size(), added);
            catalogue.Save(games_path);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "found %zu, %zu new", found.size(), added);
            games_status = buf;
        }
        ImGui::SameLine();
        if (ImGui::Button("View Detected Games", {220, 32})) ImGui::OpenPopup("Enabled Games");
        ImGui::SameLine();
        const std::size_t game_count = catalogue.games().size();
        ImGui::Text("%zu detected", game_count);
        if (!games_status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", games_status.c_str());
        }

        ImGui::SetNextWindowSize({720, 420}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Enabled Games", nullptr,
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
            ImGui::TextWrapped("List of detected Tobii enabled games, un-tick a title to "
                               "disable native Tobii support");
            ImGui::Spacing();
            ImGui::Separator();

            const ImGuiStyle& s = ImGui::GetStyle();
            const float footer_h = ImGui::GetFrameHeightWithSpacing()
                                 + 32.0f + s.ItemSpacing.y * 3.0f
                                 + s.ItemSpacing.y + 2.0f;
            ImGui::BeginChild("gameslist", {0.0f, -footer_h}, true);
            if (catalogue.games().empty()) {
                ImGui::TextDisabled("nothing detected yet");
            } else {
                for (std::size_t i = 0; i < catalogue.games().size(); ++i) {
                    auto& game = catalogue.games()[i];
                    ImGui::PushID(static_cast<int>(300 + i));
                    bool on = game.enabled;
                    if (ImGui::Checkbox(game.name.c_str(), &on)) {
                        game.enabled = on;
                        catalogue.Save(games_path);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", game.prefix.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("##root", "Add new path to include in search",
                                     new_root, sizeof(new_root));
            ImGui::SameLine();
            if (ImGui::Button("Add Folder", {110, 0}) && new_root[0] != '\0') {
                catalogue.AddRoot(new_root);
                std::vector<std::string> searched;
                const auto found = catalogue.Scan(&searched);
                const std::size_t added = catalogue.Merge(found);
                tobii::games::RecordSearch(searched, found.size(), added);
                catalogue.Save(games_path);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "found %zu, %zu new", found.size(), added);
                games_status = buf;
                new_root[0] = '\0';
            }

            const float close_w = 140.0f;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - close_w);
            if (ImGui::Button("Close", {close_w, 32})) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        const float content_height =
            ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
        const float content_width =
            ImGui::GetCurrentWindow()->DC.CursorMaxPos.x -
            ImGui::GetCurrentWindow()->Pos.x + ImGui::GetStyle().WindowPadding.x;
        ImGui::End();
        ImGui::Render();

        content_clipped = FitWindowToContent(window, content_width, content_height);

        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    client.Stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
