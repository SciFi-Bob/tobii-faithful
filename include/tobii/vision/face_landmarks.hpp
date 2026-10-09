#pragma once

#include "tobii/vision/face_detector.hpp"

#include <opencv2/core.hpp>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tobii::vision {

inline constexpr std::size_t kLandmarkCount = 478;

struct LandmarkResult {
    std::vector<cv::Point3f> points;
    float presence = 0.0f;
};

enum class RoiSource { kBoundingBox, kKeypoints, kKeypointCentreBoxSize };

inline constexpr int kRoiLeftEyeLandmark  = 33;
inline constexpr int kRoiRightEyeLandmark = 263;

struct RoiConfig {
    RoiSource source = RoiSource::kKeypointCentreBoxSize;
    int   left_eye_keypoint  = 0;
    int   right_eye_keypoint = 1;
    float target_angle_deg   = 0.0f;
    float scale              = 1.5f;
    bool  square_long        = true;
};

struct LandmarkConfig {
    int   input_size        = 256;
    float presence_threshold = 0.5f;
    RoiConfig roi;
};

struct Roi {
    cv::Point2f center;
    float size        = 0.0f;
    float rotation_rad = 0.0f;
};

Roi ComputeRoi(const Detection& detection, const cv::Size& image_size,
               const RoiConfig& cfg = {});

Roi RoiFromLandmarks(const std::vector<cv::Point3f>& landmarks, const RoiConfig& cfg = {});

class FaceLandmarker {
public:
    explicit FaceLandmarker(const std::string& model_path, LandmarkConfig cfg = {});
    ~FaceLandmarker();
    FaceLandmarker(FaceLandmarker&&) noexcept;
    FaceLandmarker& operator=(FaceLandmarker&&) noexcept;

    std::optional<LandmarkResult> Run(const cv::Mat& gray, const Roi& roi);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
