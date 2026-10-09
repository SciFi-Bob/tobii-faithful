#include "tobii/vision/pose.hpp"

#include "tobii/face_geometry_data.hpp"
#include "tobii/vision/rigid_basis.hpp"

#include <Eigen/Dense>
#include <Eigen/SVD>
#include <cmath>

namespace tobii::vision {
namespace {

constexpr float kRad2Deg = 57.29577951308232f;
constexpr float kDeg2Rad = 0.017453292519943295f;

cv::Point3f Average(const std::vector<cv::Point3f>& pts, std::initializer_list<int> idx) {
    cv::Point3f sum(0, 0, 0);
    int n = 0;
    for (const int i : idx) {
        if (i >= 0 && static_cast<std::size_t>(i) < pts.size()) { sum += pts[static_cast<std::size_t>(i)]; ++n; }
    }
    return n ? cv::Point3f(sum.x / static_cast<float>(n), sum.y / static_cast<float>(n),
                           sum.z / static_cast<float>(n))
             : cv::Point3f(0, 0, 0);
}

}

std::optional<Similarity> SolveWeightedProcrustes(const std::vector<cv::Point3f>& source,
                                                  const std::vector<cv::Point3f>& target,
                                                  const std::vector<float>& weights) {
    const std::size_t n = source.size();
    if (n < 3 || target.size() != n || weights.size() != n) return std::nullopt;

    Eigen::Matrix3Xd P(3, static_cast<Eigen::Index>(n));
    Eigen::Matrix3Xd Q(3, static_cast<Eigen::Index>(n));
    Eigen::VectorXd  w(static_cast<Eigen::Index>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const auto j = static_cast<Eigen::Index>(i);
        P(0, j) = source[i].x; P(1, j) = source[i].y; P(2, j) = source[i].z;
        Q(0, j) = target[i].x; Q(1, j) = target[i].y; Q(2, j) = target[i].z;
        w(j) = weights[i];
    }
    const double wsum = w.sum();
    if (wsum <= 0.0) return std::nullopt;

    const Eigen::Vector3d pc = (P * w) / wsum;
    const Eigen::Vector3d qc = (Q * w) / wsum;
    Eigen::Matrix3Xd Pc = P.colwise() - pc;
    Eigen::Matrix3Xd Qc = Q.colwise() - qc;

    const Eigen::Matrix3d M = (Qc * w.asDiagonal() * Pc.transpose());
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(M, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
    if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0.0) D(2, 2) = -1.0;
    const Eigen::Matrix3d R = svd.matrixU() * D * svd.matrixV().transpose();

    const double denom = (Pc.array().square().colwise().sum().matrix() * w).value();
    const double scale = denom > 1e-12 ? (svd.singularValues().dot(D.diagonal()) / denom) : 1.0;
    const Eigen::Vector3d t = qc - scale * R * pc;

    Similarity out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out.rotation(r, c) = static_cast<float>(R(r, c));
        out.translation[r] = static_cast<float>(t(r));
    }
    out.scale = static_cast<float>(scale);
    return out;
}

HeadPose PoseFromRotation(const cv::Matx33f& m) {
    const float fx = m(0, 2), fy = m(1, 2), fz = m(2, 2);
    HeadPose p;
    p.yaw   = std::atan2(fx, fz) * kRad2Deg;
    p.pitch = std::atan2(-fy, std::sqrt(fx * fx + fz * fz)) * kRad2Deg;
    p.roll  = std::atan2(m(1, 0), m(0, 0)) * kRad2Deg;
    p.rotation = m;
    return p;
}

std::optional<HeadPose> PoseFromLandmarkPlane(const std::vector<cv::Point3f>& lm) {
    if (lm.size() < kLandmarkPlaneMinPoints) return std::nullopt;
    const cv::Point3f left  = Average(lm, {33, 133, 159, 145, 468, 469, 470, 471, 472});
    const cv::Point3f right = Average(lm, {263, 362, 386, 374, 473, 474, 475, 476, 477});
    const cv::Point3f brow  = Average(lm, {10, 151, 109, 338});
    const cv::Point3f chin  = Average(lm, {152, 175, 199});

    const cv::Point3f across = right - left;
    const cv::Point3f down   = chin - brow;
    cv::Point3f normal = across.cross(down);
    const float len = std::sqrt(normal.dot(normal));
    if (len < 1e-6f) return std::nullopt;
    normal *= 1.0f / len;
    if (normal.z > 0.0f) normal = -normal;

    HeadPose p;
    p.yaw   = std::atan2(normal.x, -normal.z) * kRad2Deg;
    p.pitch = std::atan2(normal.y, std::sqrt(normal.x * normal.x + normal.z * normal.z)) * kRad2Deg;
    p.roll  = std::atan2(across.y, across.x) * kRad2Deg;
    return p;
}

float HybridPitch(float matrix_pitch, float landmark_pitch) {
    if (!std::isfinite(matrix_pitch) || !std::isfinite(landmark_pitch)) return landmark_pitch;
    if (matrix_pitch == 0.0f || landmark_pitch == 0.0f) return landmark_pitch;
    if (std::signbit(matrix_pitch) == std::signbit(landmark_pitch)) {
        return std::fabs(matrix_pitch) > std::fabs(landmark_pitch) ? matrix_pitch : landmark_pitch;
    }
    return landmark_pitch;
}

std::vector<cv::Point3f> ToMetricLandmarks(const std::vector<cv::Point3f>& landmarks,
                                           cv::Size frame) {
    std::vector<cv::Point3f> out;
    if (landmarks.empty() || frame.width <= 0 || frame.height <= 0) return out;

    const float t = std::tan(kCameraVerticalFovDeg * 0.5f * kDeg2Rad);
    const float aspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
    const float xs = 2.0f * t * aspect;
    const float ys = 2.0f * t;
    const float w = static_cast<float>(frame.width);
    const float h = static_cast<float>(frame.height);

    out.reserve(landmarks.size());
    float depth_sum = 0.0f;
    for (const auto& l : landmarks) {
        const float z = (l.z / w) * xs;
        out.emplace_back((l.x / w - 0.5f) * xs, (0.5f - l.y / h) * ys, z);
        depth_sum += z;
    }

    const float depth_offset = depth_sum / static_cast<float>(landmarks.size());
    for (auto& p : out) {
        const float depth = 1.0f + (p.z - depth_offset);
        p.x *= depth;
        p.y *= depth;
        p.z = -depth;
    }
    return out;
}

std::optional<HeadPose> EstimatePose(const std::vector<cv::Point3f>& landmarks,
                                     cv::Size frame, const PoseOptions& options) {
    if (landmarks.size() < kLandmarkPlaneMinPoints) return std::nullopt;

    const std::vector<cv::Point3f> metric = ToMetricLandmarks(landmarks, frame);
    if (metric.empty()) return std::nullopt;

    std::vector<cv::Point3f> src, dst;
    std::vector<float> w;
    src.reserve(kProcrustesLandmarkCount);
    dst.reserve(kProcrustesLandmarkCount);
    w.reserve(kProcrustesLandmarkCount);
    const auto add = [&](const WeightedLandmark& b) {
        if (b.index >= metric.size()) return;
        src.emplace_back(kCanonicalVertices[b.index * 3 + 0],
                         kCanonicalVertices[b.index * 3 + 1],
                         kCanonicalVertices[b.index * 3 + 2]);
        dst.push_back(metric[b.index]);
        w.push_back(b.weight);
    };
    if (options.solve == PoseSolve::kRigid) {
        for (const auto& b : kRigidProcrustesBasis) add(b);
    } else {
        for (const auto& b : kProcrustesBasis) add(b);
    }

    const auto fit = SolveWeightedProcrustes(src, dst, w);
    if (!fit) return std::nullopt;

    HeadPose pose = PoseFromRotation(fit->rotation);
    if (options.solve == PoseSolve::kMediaPipe && options.hybrid_pitch) {
        if (const auto plane = PoseFromLandmarkPlane(landmarks)) {
            pose.pitch = HybridPitch(pose.pitch, plane->pitch);
        }
    }
    if (fit->scale > 1e-9f) {
        const float to_mm = 10.0f / fit->scale;
        pose.translation = {fit->translation[0] * to_mm,
                            fit->translation[1] * to_mm,
                            fit->translation[2] * to_mm};
        pose.distance_mm = -pose.translation[2];
    }
    return pose;
}

}
