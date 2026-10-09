#pragma once

#include "tobii/vision/anchors.hpp"

#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

namespace tobii::vision {

struct Detection {
    float score = 0.0f;
    cv::Rect2f box;
    std::array<cv::Point2f, 6> keypoints;
};

struct DetectorConfig {
    int   input_size      = 128;
    float min_score       = 0.5f;
    float nms_iou         = 0.3f;
    float score_clip      = 100.0f;
    int   num_keypoints   = 6;
    int   keypoint_offset = 4;
    bool  reverse_output_order = true;
};

class FaceDetector {
public:
    explicit FaceDetector(const std::string& model_path, DetectorConfig cfg = {});
    ~FaceDetector();
    FaceDetector(FaceDetector&&) noexcept;
    FaceDetector& operator=(FaceDetector&&) noexcept;

    std::vector<Detection> Detect(const cv::Mat& gray);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<Detection> WeightedNms(std::vector<Detection> detections, float iou_threshold);

}
