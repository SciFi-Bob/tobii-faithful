#pragma once

#include <string>
#include <vector>

namespace tobii::paths {

std::string FirstExisting(const std::vector<std::string>& candidates);

std::string ExecutableDir();

std::string SiblingDir(const std::string& relative);

std::string Describe(const std::vector<std::string>& candidates);

}
