#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tobii::games {

inline constexpr std::uint8_t kAxisLeanLR = 1u << 0;
inline constexpr std::uint8_t kAxisLeanUD = 1u << 1;
inline constexpr std::uint8_t kAxisLeanFB = 1u << 2;
inline constexpr std::uint8_t kAxisYaw    = 1u << 3;
inline constexpr std::uint8_t kAxisPitch  = 1u << 4;
inline constexpr std::uint8_t kAxisRoll   = 1u << 5;
inline constexpr std::uint8_t kAllAxes    = 0x3f;

struct Game {
    std::string name;
    std::string prefix;
    std::string dll_path;
    bool enabled = true;
    bool user_added = false;
    std::uint8_t axes = kAllAxes;
};

class Catalogue {
public:
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    static std::string DefaultPath();

    std::vector<Game> Scan(std::vector<std::string>* searched = nullptr) const;

    static std::optional<Game> ScanPrefix(const std::string& prefix);

    std::size_t Merge(const std::vector<Game>& found);

    const Game* ForPrefix(const std::string& prefix) const;

    const Game* Find(const std::string& prefix) const;

    const std::vector<Game>& games() const { return games_; }
    std::vector<Game>& games() { return games_; }

    const std::vector<std::string>& roots() const { return roots_; }
    void AddRoot(const std::string& path);

private:
    std::vector<Game> games_;
    std::vector<std::string> roots_;
};

void RecordSearch(const std::vector<std::string>& searched, std::size_t found,
                  std::size_t added);

std::vector<std::string> DefaultSearchRoots();

}
