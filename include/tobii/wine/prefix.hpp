#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

namespace tobii::wine {

struct Prefix {
    std::string path;
    std::string server_dir;
    pid_t pid = 0;
    bool has_game_process = false;
    std::string wine_binary;
};

std::vector<Prefix> FindPrefixes();

std::string ServerDirFor(const std::string& prefix_path);

pid_t LaunchInPrefix(const std::string& prefix_path, const std::string& exe_path,
                     const std::vector<std::string>& args,
                     const std::string& wine_binary = {},
                     int* stdout_read = nullptr);

}
