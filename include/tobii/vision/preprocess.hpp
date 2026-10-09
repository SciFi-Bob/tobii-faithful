#pragma once

#include <opencv2/core.hpp>

namespace tobii::vision {

struct PreprocessConfig {
    double clahe_clip_limit = 2.0;
    int    clahe_tile_grid  = 8;
};

cv::Mat ApplyClahe(const cv::Mat& gray, const PreprocessConfig& cfg = {});

cv::Mat GrayToRgb(const cv::Mat& gray);

}
