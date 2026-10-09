#include "tobii/vision/tracker.hpp"

#include "tobii/vision/preprocess.hpp"

namespace tobii::vision {

FaceTracker::FaceTracker(const std::string& detector_model, const std::string& landmark_model,
                         TrackerConfig cfg)
    : detector_(detector_model, cfg.detector),
      landmarker_(landmark_model, cfg.landmarks),
      cfg_(std::move(cfg)) {}

void FaceTracker::Reset() { previous_.clear(); }

std::optional<TrackedFace> FaceTracker::Track(const cv::Mat& gray) {
    if (gray.empty()) { Reset(); return std::nullopt; }

    const cv::Mat frame = cfg_.apply_clahe ? ApplyClahe(gray) : gray;

    Roi roi;
    const bool reacquiring = previous_.empty();
    if (reacquiring) {
        const auto detections = detector_.Detect(frame);
        if (detections.empty()) { Reset(); return std::nullopt; }
        roi = ComputeRoi(detections[0], gray.size(), cfg_.landmarks.roi);
    } else {
        roi = RoiFromLandmarks(previous_, cfg_.landmarks.roi);
    }

    const auto result = landmarker_.Run(frame, roi);
    if (!result) { Reset(); return std::nullopt; }

    const auto pose = EstimatePose(result->points, gray.size(), cfg_.pose);
    if (!pose) { Reset(); return std::nullopt; }

    previous_ = result->points;

    TrackedFace out;
    out.pose       = *pose;
    out.landmarks  = result->points;
    out.presence   = result->presence;
    out.reacquired = reacquiring;
    return out;
}

}
