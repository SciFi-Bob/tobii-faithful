#include "tobii/vision/face_detector.hpp"

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace tobii::vision {
namespace {

float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

float IoU(const cv::Rect2f& a, const cv::Rect2f& b) {
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width,  b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float inter = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float uni = a.area() + b.area() - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

}

std::vector<Detection> WeightedNms(std::vector<Detection> detections, float iou_threshold) {
    std::sort(detections.begin(), detections.end(),
              [](const Detection& l, const Detection& r) { return l.score > r.score; });

    std::vector<Detection> kept;
    std::vector<bool> used(detections.size(), false);

    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (used[i]) continue;

        std::vector<std::size_t> cluster{i};
        used[i] = true;
        for (std::size_t j = i + 1; j < detections.size(); ++j) {
            if (used[j]) continue;
            if (IoU(detections[i].box, detections[j].box) >= iou_threshold) {
                cluster.push_back(j);
                used[j] = true;
            }
        }

        Detection merged = detections[i];
        if (cluster.size() > 1) {
            float wsum = 0.0f;
            cv::Rect2f acc(0, 0, 0, 0);
            std::array<cv::Point2f, 6> kacc{};
            for (const std::size_t idx : cluster) {
                const float w = detections[idx].score;
                wsum += w;
                acc.x      += detections[idx].box.x * w;
                acc.y      += detections[idx].box.y * w;
                acc.width  += detections[idx].box.width * w;
                acc.height += detections[idx].box.height * w;
                for (std::size_t k = 0; k < kacc.size(); ++k) {
                    kacc[k].x += detections[idx].keypoints[k].x * w;
                    kacc[k].y += detections[idx].keypoints[k].y * w;
                }
            }
            if (wsum > 0.0f) {
                merged.box = cv::Rect2f(acc.x / wsum, acc.y / wsum,
                                        acc.width / wsum, acc.height / wsum);
                for (std::size_t k = 0; k < kacc.size(); ++k) {
                    merged.keypoints[k] = cv::Point2f(kacc[k].x / wsum, kacc[k].y / wsum);
                }
            }
        }
        kept.push_back(merged);
    }
    return kept;
}

struct FaceDetector::Impl {
    DetectorConfig cfg;
    std::vector<Anchor> anchors;
    Ort::Env env{ORT_LOGGING_LEVEL_FATAL, "tobii-face-detector"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::string input_name;
    std::vector<std::string> output_names;

    Impl(const std::string& model_path, DetectorConfig c) : cfg(c), anchors(GenerateAnchors()) {
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

FaceDetector::FaceDetector(const std::string& model_path, DetectorConfig cfg)
    : impl_(std::make_unique<Impl>(model_path, cfg)) {}
FaceDetector::~FaceDetector() = default;
FaceDetector::FaceDetector(FaceDetector&&) noexcept = default;
FaceDetector& FaceDetector::operator=(FaceDetector&&) noexcept = default;

std::vector<Detection> FaceDetector::Detect(const cv::Mat& gray) {
    if (gray.empty() || gray.type() != CV_8UC1) {
        throw std::invalid_argument("FaceDetector::Detect expects a non-empty CV_8UC1 frame");
    }
    const auto& cfg = impl_->cfg;
    const int N = cfg.input_size;

    const float scale = std::min(static_cast<float>(N) / static_cast<float>(gray.cols),
                                 static_cast<float>(N) / static_cast<float>(gray.rows));
    const int sw = static_cast<int>(std::round(static_cast<float>(gray.cols) * scale));
    const int sh = static_cast<int>(std::round(static_cast<float>(gray.rows) * scale));
    const int ox = (N - sw) / 2;
    const int oy = (N - sh) / 2;

    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(sw, sh), 0, 0, cv::INTER_LINEAR);
    cv::Mat canvas(N, N, CV_8UC1, cv::Scalar(0));
    resized.copyTo(canvas(cv::Rect(ox, oy, sw, sh)));

    cv::Mat rgb;
    cv::cvtColor(canvas, rgb, cv::COLOR_GRAY2RGB);
    cv::Mat input;
    rgb.convertTo(input, CV_32FC3, 1.0 / 127.5, -1.0);

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

    const float* regressors = outputs[0].GetTensorData<float>();
    const float* scores     = outputs[1].GetTensorData<float>();
    const std::size_t num_anchors = impl_->anchors.size();

    std::vector<Detection> raw;
    for (std::size_t i = 0; i < num_anchors; ++i) {
        const float clipped = std::clamp(scores[i], -cfg.score_clip, cfg.score_clip);
        const float score = Sigmoid(clipped);
        if (score < cfg.min_score) continue;

        const Anchor& a = impl_->anchors[i];
        const float* r = regressors + i * 16;

        const float sx = cfg.reverse_output_order ? r[0] : r[1];
        const float sy = cfg.reverse_output_order ? r[1] : r[0];
        const float sw_ = cfg.reverse_output_order ? r[2] : r[3];
        const float sh_ = cfg.reverse_output_order ? r[3] : r[2];

        const float fn = static_cast<float>(N);
        const float cx = sx / fn * a.width  + a.x_center;
        const float cy = sy / fn * a.height + a.y_center;
        const float w  = sw_ / fn * a.width;
        const float h  = sh_ / fn * a.height;

        Detection d;
        d.score = score;
        d.box = cv::Rect2f(cx - w * 0.5f, cy - h * 0.5f, w, h);
        for (int k = 0; k < cfg.num_keypoints; ++k) {
            const float kx = r[cfg.keypoint_offset + k * 2];
            const float ky = r[cfg.keypoint_offset + k * 2 + 1];
            d.keypoints[static_cast<std::size_t>(k)] =
                cv::Point2f(kx / fn * a.width + a.x_center, ky / fn * a.height + a.y_center);
        }
        raw.push_back(d);
    }

    auto kept = WeightedNms(std::move(raw), cfg.nms_iou);

    const float fN   = static_cast<float>(N);
    const float fcol = static_cast<float>(gray.cols);
    const float frow = static_cast<float>(gray.rows);
    const auto to_src_x = [&](float nx) { return (nx * fN - static_cast<float>(ox)) / scale / fcol; };
    const auto to_src_y = [&](float ny) { return (ny * fN - static_cast<float>(oy)) / scale / frow; };

    for (auto& d : kept) {
        const float x1 = to_src_x(d.box.x);
        const float y1 = to_src_y(d.box.y);
        const float x2 = to_src_x(d.box.x + d.box.width);
        const float y2 = to_src_y(d.box.y + d.box.height);
        d.box = cv::Rect2f(x1, y1, x2 - x1, y2 - y1);
        for (auto& kp : d.keypoints) {
            kp.x = to_src_x(kp.x);
            kp.y = to_src_y(kp.y);
        }
    }
    return kept;
}

}
