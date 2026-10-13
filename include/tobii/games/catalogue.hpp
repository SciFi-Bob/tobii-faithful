#pragma once

#include <optional>
#include <string>
#include <vector>

namespace tobii::games {

struct Game {
    std::string name;
    std::string prefix;
    std::string dll_path;
    bool enabled = true;
    bool user_added = false;
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
