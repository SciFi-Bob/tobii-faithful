#pragma once

#include "tobii/pose/one_euro.hpp"

#include <cstdint>

namespace tobii::manager {

class PoseEasing {
public:
    struct Config {
        double mincut = 1.0;
        double beta = 0.007;
        double dcut = 1.0;
    };

    PoseEasing() : PoseEasing(Config{}) {}
    explicit PoseEasing(const Config& c) : cfg_(c) {
        for (int i = 0; i < 6; ++i) f_[i] = tobii::pose::OneEuro(c.mincut, c.beta, c.dcut);
    }

    void Update(std::uint64_t sample_ts_us, const float pose[6],
                bool pos_valid, bool rot_valid, float out[6]) {
        if (!have_ts_ || sample_ts_us != last_ts_) {
            const double dt = have_ts_ && sample_ts_us > last_ts_
                                  ? static_cast<double>(sample_ts_us - last_ts_) / 1e6
                                  : 0.0;
            for (int i = 0; i < 6; ++i) {
                const bool valid = (i < 3) ? pos_valid : rot_valid;
                if (valid) held_[i] = static_cast<float>(f_[i].Filter(pose[i], dt));
            }
            last_ts_ = sample_ts_us;
            have_ts_ = true;
        }

        for (int i = 0; i < 6; ++i) out[i] = held_[i];
    }

private:
    Config cfg_;
    tobii::pose::OneEuro f_[6];
    float held_[6] = {0, 0, 800, 0, 0, 0};
    std::uint64_t last_ts_ = 0;
    bool have_ts_ = false;
};

}
