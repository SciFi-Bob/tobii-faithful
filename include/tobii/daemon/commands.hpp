#pragma once

#include "tobii/calibration/profile.hpp"
#include "tobii/ipc/server.hpp"
#include "tobii/pipeline.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace tobii::daemon {

struct StatusCounters {
    bool device_open = false;
    bool device_present = false;
    std::size_t clients = 0;
    std::size_t holders = 0;
    double fps = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t poses = 0;
    std::string error;
};

struct CalibrationAction {
    enum class Verb { kStart, kStop, kClear, kCompute, kCollect, kDiscard, kSave, kApply, kEnabledEye };
    Verb verb = Verb::kStart;
    double x = 0.0, y = 0.0;
    std::uint32_t enabled_eye = 0;
};

struct CommandContext {
    Pipeline& pipeline;
    std::string profile_path;

    ipc::Client* client = nullptr;

    std::function<void()> release_device;
    std::function<bool()> device_open;

    StatusCounters counters;

    std::function<std::string()> device_info_json;

    std::function<bool(const CalibrationAction&, std::string& message)> calibrate;

    std::function<void(const std::string&)> reply;
};

void HandleCommand(CommandContext& ctx, const std::string& line);

bool MutateProfile(CommandContext& ctx,
                   const std::function<void(calibration::Profile&)>& change);

std::string StatusLine(const CommandContext& ctx);

std::string PoseLine(const Pipeline& pipeline, double now_s);

std::string HeadPoseLine(const pose::HeadPoseSample& sample);

std::string DiagnosticLine(const Pipeline& pipeline, double now_s);

std::string FrameLine(const Pipeline& pipeline);

}
