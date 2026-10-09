#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tobii::ipc {

struct Client {
    int  fd = -1;
    bool wants_pose = false;
    bool holds_device = false;
    bool wants_diagnostics = false;
};

class Server {
public:
    Server();
    ~Server();

    bool Acquire(const std::string& path);

    bool Listen(const std::string& path);
    void Close();

    bool Poll(int timeout_ms,
              const std::function<void(Client&, const std::string&)>& on_command);

    void BroadcastPose(const std::string& line);
    void BroadcastDiagnostics(const std::string& line);
    bool any_diagnostics() const;

    void Send(Client& client, const std::string& line);

    static constexpr std::size_t kMaxBacklogBytes = 256 * 1024;

    std::size_t client_count() const { return clients_.size(); }
    std::size_t device_holders() const;

    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }

private:
    void Drop(std::size_t index);
    void CloseListener();

    int listen_fd_ = -1;
    int lock_fd_ = -1;
    std::string lock_path_;
    std::string path_;
    std::string error_;
    std::vector<Client> clients_;
    std::vector<std::string> pending_;
    std::vector<std::string> outbox_;

    void Flush(std::size_t index);
};

}
