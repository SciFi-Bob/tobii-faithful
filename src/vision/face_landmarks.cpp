#include "tobii/vision/face_landmarks.hpp"

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tobii::vision {

Roi ComputeRoi(const Detection& detection, const cv::Size& image_size, const RoiConfig& cfg) {
    const float w = static_cast<float>(image_size.width);
    const float h = static_cast<float>(image_size.height);

    const cv::Point2f left  = {detection.keypoints[static_cast<std::size_t>(cfg.left_eye_keypoint)].x * w,
                               detection.keypoints[static_cast<std::size_t>(cfg.left_eye_keypoint)].y * h};
    const cv::Point2f right = {detection.keypoints[static_cast<std::size_t>(cfg.right_eye_keypoint)].x * w,
                               detection.keypoints[static_cast<std::size_t>(cfg.right_eye_keypoint)].y * h};

    const float target = cfg.target_angle_deg * static_cast<float>(CV_PI) / 180.0f;
    const float rotation = target - std::atan2(-(right.y - left.y), right.x - left.x);

    Roi roi;
    float bw = detection.box.width * w;
    float bh = detection.box.height * h;
    if (cfg.source == RoiSource::kKeypoints ||
        cfg.source == RoiSource::kKeypointCentreBoxSize) {
        float xmin = 1e9f, ymin = 1e9f, xmax = -1e9f, ymax = -1e9f;
        for (const auto& kp : detection.keypoints) {
            xmin = std::min(xmin, kp.x); xmax = std::max(xmax, kp.x);
            ymin = std::min(ymin, kp.y); ymax = std::max(ymax, kp.y);
        }
        roi.center = {(xmin + xmax) * 0.5f * w, (ymin + ymax) * 0.5f * h};
        if (cfg.source == RoiSource::kKeypoints) {
            bw = (xmax - xmin) * w;
            bh = (ymax - ymin) * h;
        }
    } else {
        roi.center = {(detection.box.x + detection.box.width * 0.5f) * w,
                      (detection.box.y + detection.box.height * 0.5f) * h};
    }

    float side = cfg.square_long ? std::max(bw, bh) : std::min(bw, bh);
    roi.size = side * cfg.scale;
    roi.rotation_rad = rotation;
    return roi;
}

Roi RoiFromLandmarks(const std::vector<cv::Point3f>& landmarks, const RoiConfig& cfg) {
    Roi roi;
    if (landmarks.size() <= static_cast<std::size_t>(kRoiRightEyeLandmark)) return roi;

    float xmin = 1e9f, ymin = 1e9f, xmax = -1e9f, ymax = -1e9f;
    for (const auto& l : landmarks) {
        xmin = std::min(xmin, l.x); xmax = std::max(xmax, l.x);
        ymin = std::min(ymin, l.y); ymax = std::max(ymax, l.y);
    }

    const cv::Point3f& left  = landmarks[static_cast<std::size_t>(kRoiLeftEyeLandmark)];
    const cv::Point3f& right = landmarks[static_cast<std::size_t>(kRoiRightEyeLandmark)];
    const float target = cfg.target_angle_deg * static_cast<float>(CV_PI) / 180.0f;

    roi.center = {(xmin + xmax) * 0.5f, (ymin + ymax) * 0.5f};
    const float side = cfg.square_long ? std::max(xmax - xmin, ymax - ymin)
                                       : std::min(xmax - xmin, ymax - ymin);
    roi.size = side * cfg.scale;
    roi.rotation_rad = target - std::atan2(-(right.y - left.y), right.x - left.x);
    return roi;
}

struct FaceLandmarker::Impl {
    LandmarkConfig cfg;
    Ort::Env env{ORT_LOGGING_LEVEL_FATAL, "tobii-face-landmarks"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::string input_name;
    std::vector<std::string> output_names;

    Impl(const std::string& model_path, LandmarkConfig c) : cfg(c) {
        options.SetIntraOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = std::make_unique<Ort::Session>(env, model_path.c_str(), options);
        Ort::AllocatorWithDefaultOptions alloc;
        input_name = session->GetInputNameAllocated(0, alloc).get();
        for (std::size_t i = 0; i < session->GetOutputCount(); ++i) {
            output_names.emplace_back(session->GetOutputNameAllocated(i, alloc).get());
        }
    }
};

FaceLandmarker::FaceLandmarker(const std::string& model_path, LandmarkConfig cfg)
    : impl_(std::make_unique<Impl>(model_path, cfg)) {}
FaceLandmarker::~FaceLandmarker() = default;
FaceLandmarker::FaceLandmarker(FaceLandmarker&&) noexcept = default;
FaceLandmarker& FaceLandmarker::operator=(FaceLandmarker&&) noexcept = default;

std::optional<LandmarkResult> FaceLandmarker::Run(const cv::Mat& gray, const Roi& roi) {
    if (gray.empty() || gray.type() != CV_8UC1) {
        throw std::invalid_argument("FaceLandmarker::Run expects a non-empty CV_8UC1 frame");
    }
    const auto& cfg = impl_->cfg;
    const int N = cfg.input_size;

    const float c = std::cos(roi.rotation_rad);
    const float s = std::sin(roi.rotation_rad);
    const float half = roi.size * 0.5f;
    const auto corner = [&](float dx, float dy) {
        return cv::Point2f(roi.center.x + dx * half * c - dy * half * s,
                           roi.center.y + dx * half * s + dy * half * c);
    };
    const cv::Point2f src[3] = {corner(-1, -1), corner(1, -1), corner(-1, 1)};
    const cv::Point2f dst[3] = {{0, 0}, {static_cast<float>(N), 0}, {0, static_cast<float>(N)}};
    const cv::Mat affine = cv::getAffineTransform(src, dst);

    cv::Mat crop;
    cv::warpAffine(gray, crop, affine, cv::Size(N, N), cv::INTER_LINEAR,
                   cv::BORDER_REPLICATE);

    cv::Mat rgb;
    cv::cvtColor(crop, rgb, cv::COLOR_GRAY2RGB);
    cv::Mat input;
    rgb.convertTo(input, CV_32FC3, 1.0 / 255.0, 0.0);

    const std::array<int64_t, 4> shape{1, N, N, 3};
    auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(
        mem, reinterpret_cast<float*>(input.data),
        static_cast<std::size_t>(N) * static_cast<std::size_t>(N) * 3,
        shape.data(), shape.size());

    const char* in_names[] = {impl_->input_name.c_str()};
    std::vector<const char*> out_names;
    for (const auto& n : impl_->output_names) out_names.push_back(n.c_str());
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1,
                                       out_names.data(), out_names.size());

    const float* raw = outputs[0].GetTensorData<float>();
    const float presence_logit = *outputs[1].GetTensorData<float>();
    const float presence = 1.0f / (1.0f + std::exp(-presence_logit));
    if (presence < cfg.presence_threshold) {
        return std::nullopt;
    }

    cv::Mat inv;
    cv::invertAffineTransform(affine, inv);
    const auto* m = inv.ptr<double>();
    const float zs = static_cast<float>(std::hypot(m[0], m[3]));

    LandmarkResult result;
    result.presence = presence;
    result.points.reserve(kLandmarkCount);
    for (std::size_t i = 0; i < kLandmarkCount; ++i) {
        const float x = raw[i * 3 + 0];
        const float y = raw[i * 3 + 1];
        const float z = raw[i * 3 + 2];
        result.points.emplace_back(
            static_cast<float>(m[0] * x + m[1] * y + m[2]),
            static_cast<float>(m[3] * x + m[4] * y + m[5]),
            z * zs);
    }
    return result;
}

}
