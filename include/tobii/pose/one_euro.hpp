#pragma once

#include <cmath>

namespace tobii::pose {

class OneEuro {
public:
    OneEuro(double fc_min = 2.3, double beta = 0.09, double dcutoff = 1.0)
        : fc_min_(fc_min), beta_(beta), dcutoff_(dcutoff) {}

    double Filter(double x, double dt) {
        if (!have_prev_ || dt <= 0.0) {
            x_prev_ = x;
            dx_prev_ = 0.0;
            have_prev_ = true;
            return x;
        }
        const double dx = (x - x_prev_) / dt;
        const double edx = LowPass(dx, dx_prev_, Alpha(dcutoff_, dt));
        dx_prev_ = edx;
        const double cutoff = fc_min_ + beta_ * std::fabs(edx);
        const double x_hat = LowPass(x, x_prev_, Alpha(cutoff, dt));
        x_prev_ = x_hat;
        return x_hat;
    }

    void Reset() { have_prev_ = false; }

private:
    static double Alpha(double cutoff, double dt) {
        const double tau = 1.0 / (2.0 * M_PI * cutoff);
        return 1.0 / (1.0 + tau / dt);
    }
    static double LowPass(double x, double prev, double a) { return a * x + (1.0 - a) * prev; }

    double fc_min_;
    double beta_;
    double dcutoff_;
    double x_prev_ = 0.0;
    double dx_prev_ = 0.0;
    bool have_prev_ = false;
};

}
