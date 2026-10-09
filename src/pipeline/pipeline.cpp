#include "tobii/pipeline.hpp"

#include <cmath>

namespace tobii {

Pipeline::Pipeline(const std::string& detector_model, const std::string& landmark_model)
    : tracker_(detector_model, landmark_model) {
    set_profile(profile_);
}

void Pipeline::set_profile(const calibration::Profile& profile) {
    profile_ = profile;
    vision::PoseOptions options;
    options.solve = profile_.pose_solve;
    tracker_.set_pose_options(options);
    double tl[3], tr[3], bl[3];
    profile_.display.ScreenCorners(tl, tr, bl);
    official_.set_screen_frame(pose::ScreenFrame::FromCorners(
        Eigen::Vector3d(tl[0], tl[1], tl[2]), Eigen::Vector3d(tr[0], tr[1], tr[2]),
        Eigen::Vector3d(bl[0], bl[1], bl[2])));
}

void Pipeline::Reset() {
    tracker_.Reset();
    face_tracked_ = false;
    presence_ = 0.0f;
    distance_mm_ = 0.0f;
    last_tracked_s_ = 0.0;
    landmarks_.clear();
    last_frame_.release();
    eyes_ = {};
    raw_gaze_x_ = 0.5f;
    raw_gaze_y_ = 0.5f;
    raw_gaze_valid_ = false;
    official_.Reset();
    official_sample_ = {};
}

EyeMidpoint HeadMidpointFromEyes(const double left[3], bool left_ok,
                                 const double right[3], bool right_ok,
                                 double tracker_pitch_deg) {
    const double t = tracker_pitch_deg * 3.14159265358979 / 180.0;
    const double c = std::cos(t), sn = std::sin(t);
    const auto to_world = [&](const double p[3], double out[3]) {
        out[0] = p[0];
        out[1] = p[1] * c + p[2] * sn;
        out[2] = -p[1] * sn + p[2] * c;
    };

    double l[3], r[3];
    if (left_ok) to_world(left, l);
    if (right_ok) to_world(right, r);

    EyeMidpoint out;
    if (left_ok && right_ok) {
        out = {(l[0] + r[0]) * 0.5, (l[1] + r[1]) * 0.5, (l[2] + r[2]) * 0.5, true};
    } else if (left_ok) {
        out = {l[0], l[1], l[2], true};
    } else if (right_ok) {
        out = {r[0], r[1], r[2], true};
    }
    return out;
}

void Pipeline::OnGaze(const usb::GazeSample& sample) {
    raw_gaze_x_ = static_cast<float>(sample.gaze_x);
    raw_gaze_y_ = static_cast<float>(sample.gaze_y);
    raw_gaze_valid_ = sample.GazeIsValid();

    if (sample.has_left_origin) {
        eyes_.left[0] = sample.eye_left_x;
        eyes_.left[1] = sample.eye_left_y;
        eyes_.left[2] = sample.eye_left_z;
    }
    if (sample.has_right_origin) {
        eyes_.right[0] = sample.eye_right_x;
        eyes_.right[1] = sample.eye_right_y;
        eyes_.right[2] = sample.eye_right_z;
    }
    eyes_.has_left = sample.has_left_origin;
    eyes_.has_right = sample.has_right_origin;
    eyes_.validity_left = sample.validity_left;
    eyes_.validity_right = sample.validity_right;
    eyes_.present_left = sample.eye_present_left;
    eyes_.present_right = sample.eye_present_right;
    eyes_.timestamp_us = sample.timestamp_us;
}

bool Pipeline::OnImage(const usb::ImageFrame& frame, double now_s) {
    face_tracked_ = false;
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0) return false;

    const cv::Mat gray(static_cast<int>(frame.height), static_cast<int>(frame.width),
                       CV_8UC1, const_cast<std::uint8_t*>(frame.pixels.data()),
                       frame.stride != 0 ? frame.stride : frame.width);

    gray.copyTo(last_frame_);

    const auto tracked = tracker_.Track(gray);
    if (!tracked) { landmarks_.clear(); return false; }
    landmarks_ = tracked->landmarks;

    pose::HeadPoseInput in;
    const std::int64_t ts = eyes_.timestamp_us != 0
                                ? eyes_.timestamp_us
                                : static_cast<std::int64_t>(now_s * 1e6);
    in.ts_us = static_cast<std::uint64_t>(ts);
    in.has_mesh = true;
    const cv::Matx33f& m = tracked->pose.rotation;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) in.mesh_rotation(r, c) = static_cast<double>(m(r, c));
    const bool left_ok = eyes_.validity_left == 0 && eyes_.has_left;
    const bool right_ok = eyes_.validity_right == 0 && eyes_.has_right;
    in.has_left = left_ok;
    in.has_right = right_ok;
    if (left_ok)
        in.left_eye = Eigen::Vector3d(-eyes_.left[0], eyes_.left[1], eyes_.left[2]);
    if (right_ok)
        in.right_eye = Eigen::Vector3d(-eyes_.right[0], eyes_.right[1], eyes_.right[2]);
    official_sample_ = official_.Update(in);

    face_tracked_ = true;
    presence_ = tracked->presence;
    distance_mm_ = tracked->pose.distance_mm;
    last_tracked_s_ = now_s;
    return true;
}

}
