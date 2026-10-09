#include "tobii/calibration/profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace tobii::calibration {

void DisplayArea::ScreenCorners(double top_left[3], double top_right[3],
                                double bottom_left[3]) const {
    const double half_w = width_mm / 2.0;
    const double half_h = height_mm / 2.0;

    const double x_left = offset_x_mm - half_w;
    const double x_right = offset_x_mm + half_w;
    const double y_top = offset_y_mm + half_h;
    const double y_bottom = offset_y_mm - half_h;

    const double t = static_cast<double>(tracker_pitch_deg) * 3.14159265358979 / 180.0;
    const double c = std::cos(t), s = std::sin(t);
    const auto to_device = [&](double x, double y, double out[3]) {
        out[0] = x;
        out[1] = y * c - offset_z_mm * s;
        out[2] = y * s + offset_z_mm * c;
    };
    to_device(x_left, y_top, top_left);
    to_device(x_right, y_top, top_right);
    to_device(x_left, y_bottom, bottom_left);
}

bool DisplayArea::SetFromMarkAlignment(float left_fraction, float right_fraction,
                                       int pixel_w, int pixel_h) {
    if (pixel_w <= 0 || pixel_h <= 0) return false;
    const float separation = std::fabs(right_fraction - left_fraction);
    if (separation < 0.08f) return false;

    const float mm_per_pixel =
        kTrackerMarkSpacingMm / (separation * static_cast<float>(pixel_w));
    const float derived_w = mm_per_pixel * static_cast<float>(pixel_w);
    if (derived_w < kTrackerLengthMm || derived_w > 2000.0f) return false;

    width_mm = derived_w;
    height_mm = mm_per_pixel * static_cast<float>(pixel_h);

    const float midpoint = (left_fraction + right_fraction) * 0.5f;
    offset_x_mm = -(midpoint - 0.5f) * width_mm;

    offset_y_mm = height_mm * 0.5f + tracker_drop_mm;
    offset_z_mm = kScreenBehindTrackerMm;

    pixel_width = pixel_w;
    pixel_height = pixel_h;
    measured = true;
    return true;
}

void DisplayArea::SetTrackerDrop(float drop_mm) {
    tracker_drop_mm = drop_mm;
    drop_measured = true;
    offset_y_mm = height_mm * 0.5f + drop_mm;
}

std::string Profile::DefaultPath() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config != '\0') {
        return std::string(config) + "/tobii-faithful/profile.conf";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::string(home) + "/.config/tobii-faithful/profile.conf";
    }
    return "/tmp/tobii-profile.conf";
}

bool Profile::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::size_t keys = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;

        ++keys;
        if (key == "monitor") {
            ss >> display.monitor_index;
            std::string name;
            if (ss >> name) display.monitor_name = name;
        }
        else if (key == "tracker_pitch") { ss >> display.tracker_pitch_deg; }
        else if (key == "tracker_drop") {
            int measured = 0;
            ss >> display.tracker_drop_mm >> measured;
            display.drop_measured = measured != 0;
        }
        else if (key == "display_measured") {
            int v = 0;
            ss >> v;
            display.measured = v != 0;
        }
        else if (key == "display") {
            ss >> display.width_mm >> display.height_mm >> display.offset_x_mm
               >> display.offset_y_mm >> display.offset_z_mm
               >> display.pixel_width >> display.pixel_height;
        } else if (key == "pose_solve") {
            std::string name;
            ss >> name;
            ParsePoseSolve(name, pose_solve);
        } else {
            --keys;
        }
    }
    return keys > 0;
}

bool Profile::Save(const std::string& path) const {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        std::string built;
        for (std::size_t i = 0; i <= dir.size(); ++i) {
            if (i == dir.size() || dir[i] == '/') {
                if (!built.empty()) ::mkdir(built.c_str(), 0755);
            }
            if (i < dir.size()) built += dir[i];
        }
        ::mkdir(dir.c_str(), 0755);
    }

    std::ostringstream f;

    f << "# tobii-faithful calibration profile\n"
      << "# Written by tobiid. Hand editing is fine; keys are one per line.\n\n";
    f << "monitor " << display.monitor_index;
    if (!display.monitor_name.empty()) f << ' ' << display.monitor_name;
    f << "\n\n";

    {
        f << "# width_mm height_mm offset_x_mm offset_y_mm offset_z_mm px_w px_h\n"
          << "display " << display.width_mm << ' ' << display.height_mm << ' '
          << display.offset_x_mm << ' ' << display.offset_y_mm << ' '
          << display.offset_z_mm << ' ' << display.pixel_width << ' '
          << display.pixel_height << "\n";
        f << "# how far the tracker sits below the picture, mm, and whether that "
             "was measured\n";
        f << "tracker_drop " << display.tracker_drop_mm << ' '
          << (display.drop_measured ? 1 : 0) << "\n";
        f << "# how far the tracker's own frame is rotated nose-up, degrees\n";
        f << "tracker_pitch " << display.tracker_pitch_deg << "\n";
        f << "display_measured " << (display.measured ? 1 : 0) << "\n\n";
    }

    if (pose_solve != kDefaultPoseSolve) {
        f << "# how head rotation is solved from the face mesh: mediapipe or rigid\n";
        f << "pose_solve " << PoseSolveName(pose_solve) << "\n\n";
    }

    const std::string text = f.str();
    const std::string temp = path + ".new";

    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    std::size_t at = 0;
    while (at < text.size()) {
        const ssize_t n = ::write(fd, text.data() + at, text.size() - at);
        if (n <= 0) { ::close(fd); ::unlink(temp.c_str()); return false; }
        at += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(temp.c_str()); return false; }
    if (::close(fd) != 0) { ::unlink(temp.c_str()); return false; }

    if (::rename(temp.c_str(), path.c_str()) != 0) {
        return false;
    }

    if (slash != std::string::npos) {
        const std::string dir = slash == 0 ? "/" : path.substr(0, slash);
        const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd >= 0) { ::fsync(dir_fd); ::close(dir_fd); }
    }
    return true;
}

}
