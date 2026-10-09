#pragma once

#include <string>

namespace tobii::manager {

class SingleInstance {
  public:
    explicit SingleInstance(std::string name);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool owner() const { return listen_fd_ >= 0; }

    void NudgeOwner(const std::string& request = {}) const;

    bool TakeNudge(std::string* request = nullptr);

  private:
    int listen_fd_ = -1;
    std::string name_;
};

}
