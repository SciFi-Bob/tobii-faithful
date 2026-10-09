#pragma once

#include "tobii/pose_solve.hpp"

#include <opencv2/core.hpp>
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace tobii::vision {

inline constexpr std::size_t kLandmarkPlaneMinPoints = 478;

inline constexpr float kCameraVerticalFovDeg = 63.0f;

struct HeadPose {
    float yaw   = 0.0f;
    float pitch = 0.0f;
    float roll  = 0.0f;
    std::array<float, 3> translation{};
    float distance_mm = 0.0f;
    cv::Matx33f rotation = cv::Matx33f::eye();
};

using tobii::PoseSolve;

struct PoseOptions {
    PoseSolve solve = PoseSolve::kMediaPipe;
    bool hybrid_pitch = true;
};

struct Similarity {
    cv::Matx33f rotation;
    cv::Vec3f   translation;
    float       scale = 1.0f;
};

std::optional<Similarity> SolveWeightedProcrustes(const std::vector<cv::Point3f>& source,
                                                  const std::vector<cv::Point3f>& target,
                                                  const std::vector<float>& weights);

HeadPose PoseFromRotation(const cv::Matx33f& rotation);

std::optional<HeadPose> PoseFromLandmarkPlane(const std::vector<cv::Point3f>& landmarks);

float HybridPitch(float matrix_pitch, float landmark_pitch);

std::vector<cv::Point3f> ToMetricLandmarks(const std::vector<cv::Point3f>& landmarks,
                                           cv::Size frame);

std::optional<HeadPose> EstimatePose(const std::vector<cv::Point3f>& landmarks,
                                     cv::Size frame, const PoseOptions& options = {});

}
