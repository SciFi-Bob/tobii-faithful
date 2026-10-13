#include "tobii/games/catalogue.hpp"

#include "tobii/eventlog.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sys/stat.h>

namespace tobii::games {
namespace {

namespace fs = std::filesystem;

constexpr int kMaxDepth = 8;

std::string Home() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? home : "";
}

bool IsTobiiDll(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("tobii_gameintegration", 0) == 0 &&
           lower.size() > 4 && lower.substr(lower.size() - 4) == ".dll";
}

std::string NameFromPath(const fs::path& dll, const fs::path& prefix) {
    static const char* kSkip[] = {
        "bin64", "bin32", "bin", "x64", "x86", "win64", "win32", "binaries",
        "live", "ptu", "eptu", "tech-preview", "hotfix", "game", "data",
    };
    for (fs::path p = dll.parent_path(); !p.empty() && p != p.root_path() && p != prefix;
         p = p.parent_path()) {
        std::string lower = p.filename().string();
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool skip = false;
        for (const char* candidate : kSkip) {
            if (lower == candidate) { skip = true; break; }
        }
        if (skip) continue;
        if (lower.rfind("program files", 0) == 0 || lower == "drive_c") break;
        return p.filename().string();
    }
    return prefix.filename().string();
}

constexpr int kProtonScanDepth = 4;

std::optional<std::string> FindTobiiDll(const fs::path& start, int max_depth) {
    std::error_code ec;
    if (!fs::is_directory(start, ec)) return std::nullopt;
    std::vector<fs::path> level{start};
    for (int depth = 0; depth <= max_depth && !level.empty(); ++depth) {
        std::vector<fs::path> next;
        for (const auto& dir : level) {
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
                 end; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); break; }
                std::error_code fec;
                if (it->is_regular_file(fec)) {
                    if (IsTobiiDll(it->path().filename().string())) return it->path().string();
                } else if (depth < max_depth && it->is_directory(fec)) {
                    next.push_back(it->path());
                }
            }
        }
        level.swap(next);
    }
    return std::nullopt;
}

std::string ReadFileText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::string> VdfValues(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + key + "\"";
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos)) {
        pos += needle.size();
        const std::size_t q1 = text.find('"', pos);
        if (q1 == std::string::npos) break;
        const std::size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        out.push_back(text.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }
    return out;
}

std::string VdfValue(const std::string& text, const std::string& key) {
    const auto all = VdfValues(text, key);
    return all.empty() ? std::string() : all.front();
}

std::vector<fs::path> SteamBases() {
    const std::string home = Home();
    if (home.empty()) return {};
    static const char* kRel[] = {
        "/.local/share/Steam", "/.steam/steam", "/.steam/root",
        "/.steam/debian-installation",
        "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
    };
    std::vector<fs::path> bases;
    std::error_code ec;
    for (const char* rel : kRel) {
        const fs::path p = home + rel;
        if (!fs::exists(p / "steamapps", ec)) continue;
        const fs::path canon = fs::weakly_canonical(p, ec);
        const fs::path key = ec ? p : canon;
        if (std::find(bases.begin(), bases.end(), key) == bases.end()) bases.push_back(key);
    }
    return bases;
}

std::vector<fs::path> SteamLibraries() {
    std::vector<fs::path> libs;
    std::error_code ec;
    const auto add = [&](const fs::path& p) {
        const fs::path canon = fs::weakly_canonical(p, ec);
        const fs::path key = ec ? p : canon;
        if (fs::exists(key / "steamapps", ec) &&
            std::find(libs.begin(), libs.end(), key) == libs.end()) {
            libs.push_back(key);
        }
    };
    for (const auto& base : SteamBases()) {
        add(base);
        const std::string vdf = ReadFileText(base / "steamapps" / "libraryfolders.vdf");
        for (const auto& path : VdfValues(vdf, "path")) add(fs::path(path));
    }
    return libs;
}

void ScanSteamLibraryInto(const fs::path& lib, std::vector<Game>& out) {
    std::error_code ec;
    const fs::path steamapps = lib / "steamapps";
    for (fs::directory_iterator it(steamapps, fs::directory_options::skip_permission_denied, ec),
         end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string fn = it->path().filename().string();
        if (fn.rfind("appmanifest_", 0) != 0 || fn.size() < 4 ||
            fn.substr(fn.size() - 4) != ".acf") continue;

        const std::string acf = ReadFileText(it->path());
        const std::string installdir = VdfValue(acf, "installdir");
        const std::string appid = VdfValue(acf, "appid");
        if (installdir.empty() || appid.empty()) continue;

        const auto dll = FindTobiiDll(steamapps / "common" / installdir, kProtonScanDepth);
        if (!dll) continue;

        Game game;
        const std::string name = VdfValue(acf, "name");
        game.name = name.empty() ? installdir : name;
        game.prefix = (steamapps / "compatdata" / appid / "pfx").string();
        game.dll_path = *dll;
        out.push_back(std::move(game));
    }
}

}

std::vector<std::string> DefaultSearchRoots() {
    const std::string home = Home();
    std::vector<std::string> roots{
        home + "/Games",
        home + "/.wine",
        home + "/.local/share/lutris/prefixes",
        home + "/.var/app/net.lutris.Lutris/data/lutris/prefixes",
        home + "/Games/Heroic/Prefixes",
        home + "/.local/share/heroic/Prefixes",
        home + "/.local/share/bottles/bottles",
        home + "/.var/app/com.usebottles.bottles/data/bottles/bottles",
    };
    roots.erase(std::remove_if(roots.begin(), roots.end(), [](const std::string& r) {
        struct stat st{};
        return ::stat(r.c_str(), &st) != 0;
    }), roots.end());
    return roots;
}

namespace {

constexpr int kRootDepth = 8;

fs::path Resolved(const std::string& path) {
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(path, ec);
    return ec || canon.empty() ? fs::path(path) : canon;
}

bool Covers(const fs::path& outer, const fs::path& inner) {
    auto o = outer.begin();
    auto i = inner.begin();
    for (; o != outer.end(); ++o, ++i) {
        if (o->empty()) continue;
        if (i == inner.end() || *o != *i) return false;
    }
    int below = 0;
    for (; i != inner.end(); ++i) {
        if (!i->empty()) ++below;
    }
    return below < kRootDepth;
}

std::string TidyRoot(const std::string& path) {
    const std::size_t first = path.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    std::string p = path.substr(first, path.find_last_not_of(" \t") - first + 1);
    if (p == "~" || p.rfind("~/", 0) == 0) p = Home() + p.substr(1);

    fs::path full(p);
    if (full.is_relative()) {
        std::error_code ec;
        full = fs::current_path(ec) / full;
    }
    fs::path kept;
    for (const auto& part : full.lexically_normal()) {
        if (part == "drive_c") break;
        kept /= part;
    }
    std::string out = kept.string();
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

void WalkRoot(const fs::path& dir, int depth, std::vector<fs::path>& prefixes,
              std::vector<fs::path>& libraries) {
    std::error_code ec;
    const auto add = [](std::vector<fs::path>& to, const fs::path& p) {
        if (std::find(to.begin(), to.end(), p) == to.end()) to.push_back(p);
    };
    if (fs::exists(dir / "drive_c", ec)) { add(prefixes, dir); return; }
    if (fs::exists(dir / "pfx" / "drive_c", ec)) { add(prefixes, dir / "pfx"); return; }
    if (fs::exists(dir / "steamapps", ec)) { add(libraries, dir); return; }
    if (depth >= kRootDepth) return;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) WalkRoot(it->path(), depth + 1, prefixes, libraries);
    }
}

}

void RecordSearch(const std::vector<std::string>& searched, std::size_t found,
                  std::size_t added) {
    std::string line = "Game search:";
    for (std::size_t i = 0; i < searched.size(); ++i) {
        line += (i == 0 ? " " : ", ") + searched[i];
    }
    if (searched.empty()) line += " (no folders to search)";
    line += ": found " + std::to_string(found) + ", " + std::to_string(added) + " new";
    eventlog::Record(line);
}

void Catalogue::AddRoot(const std::string& path) {
    const std::string root = TidyRoot(path);
    if (root.empty()) return;
    const fs::path resolved = Resolved(root);
    for (const auto& r : roots_) {
        if (Covers(Resolved(r), resolved)) return;
    }
    roots_.erase(std::remove_if(roots_.begin(), roots_.end(), [&](const std::string& r) {
        return Covers(resolved, Resolved(r));
    }), roots_.end());
    roots_.push_back(root);
}

std::vector<Game> Catalogue::Scan(std::vector<std::string>* searched) const {
    std::vector<std::string> roots = roots_;
    for (const auto& r : DefaultSearchRoots()) {
        if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
    }

    std::vector<Game> found;
    std::error_code ec;

    std::vector<fs::path> prefixes;
    std::vector<fs::path> root_libraries;
    for (const auto& root : roots) {
        const fs::path base(root);
        if (!fs::exists(base, ec)) continue;
        if (searched != nullptr) searched->push_back(root);
        WalkRoot(base, 0, prefixes, root_libraries);
    }

    for (const auto& prefix : prefixes) {
        auto game = ScanPrefix(prefix.string());
        if (!game) continue;
        if (std::none_of(found.begin(), found.end(),
                         [&](const Game& g) { return g.prefix == game->prefix; })) {
            found.push_back(std::move(*game));
        }
    }

    std::vector<fs::path> libraries = SteamLibraries();
    for (const auto& lib : root_libraries) {
        const fs::path key = Resolved(lib.string());
        if (std::find(libraries.begin(), libraries.end(), key) == libraries.end()) {
            libraries.push_back(key);
        }
    }
    for (const auto& lib : libraries) {
        if (searched != nullptr &&
            std::find(searched->begin(), searched->end(), lib.string()) == searched->end()) {
            searched->push_back(lib.string());
        }
        std::vector<Game> steam;
        ScanSteamLibraryInto(lib, steam);
        for (auto& game : steam) {
            if (std::none_of(found.begin(), found.end(),
                             [&](const Game& g) { return g.prefix == game.prefix; })) {
                found.push_back(std::move(game));
            }
        }
    }
    return found;
}

std::optional<Game> Catalogue::ScanPrefix(const std::string& prefix) {
    const fs::path root(prefix);

    if (const auto dll = FindTobiiDll(root / "drive_c", kMaxDepth)) {
        Game game;
        game.dll_path = *dll;
        game.prefix = root.string();
        game.name = NameFromPath(fs::path(*dll), root);
        return game;
    }

    if (root.filename() == "pfx") {
        const fs::path appid_dir = root.parent_path();
        const fs::path compatdata = appid_dir.parent_path();
        if (compatdata.filename() == "compatdata") {
            const fs::path steamapps = compatdata.parent_path();
            const std::string appid = appid_dir.filename().string();
            const std::string acf =
                ReadFileText(steamapps / ("appmanifest_" + appid + ".acf"));
            const std::string installdir = VdfValue(acf, "installdir");
            if (!installdir.empty()) {
                if (const auto dll =
                        FindTobiiDll(steamapps / "common" / installdir, kProtonScanDepth)) {
                    Game game;
                    game.dll_path = *dll;
                    game.prefix = root.string();
                    const std::string name = VdfValue(acf, "name");
                    game.name = name.empty() ? installdir : name;
                    return game;
                }
            }
        }
    }
    return std::nullopt;
}

std::size_t Catalogue::Merge(const std::vector<Game>& found) {
    std::size_t added = 0;
    for (const auto& game : found) {
        const auto existing = std::find_if(games_.begin(), games_.end(),
            [&](const Game& g) { return g.prefix == game.prefix; });
        if (existing != games_.end()) {
            existing->dll_path = game.dll_path;
            existing->name = game.name;
            continue;
        }
        games_.push_back(game);
        ++added;
    }
    return added;
}

const Game* Catalogue::ForPrefix(const std::string& prefix) const {
    for (const auto& game : games_) {
        if (!game.enabled) continue;
        if (game.prefix == prefix) return &game;
        std::error_code ec;
        const auto a = fs::weakly_canonical(game.prefix, ec);
        const auto b = fs::weakly_canonical(prefix, ec);
        if (!ec && !a.empty() && a == b) return &game;
    }
    return nullptr;
}

std::string Catalogue::DefaultPath() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config) {
        return std::string(config) + "/tobii-faithful/games.conf";
    }
    return Home() + "/.config/tobii-faithful/games.conf";
}

bool Catalogue::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    games_.clear();
    roots_.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t first = line.find('\t');
        const std::string key = line.substr(0, first);
        if (key == "root" && first != std::string::npos) {
            AddRoot(line.substr(first + 1));
            continue;
        }
        if (key != "game" || first == std::string::npos) continue;
        std::vector<std::string> fields;
        std::size_t pos = first + 1;
        for (;;) {
            const std::size_t next = line.find('\t', pos);
            fields.push_back(line.substr(pos, next == std::string::npos ? std::string::npos
                                                                        : next - pos));
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        if (fields.size() < 4) continue;
        Game game;
        game.enabled = fields[0] != "0";
        game.name = fields[1];
        game.prefix = fields[2];
        game.dll_path = fields[3];
        game.user_added = fields.size() > 4 && fields[4] == "user";
        games_.push_back(std::move(game));
    }
    return true;
}

bool Catalogue::Save(const std::string& path) const {
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string built;
        const std::string dir = path.substr(0, slash);
        for (std::size_t i = 0; i <= dir.size(); ++i) {
            if (i == dir.size() || dir[i] == '/') {
                if (!built.empty()) ::mkdir(built.c_str(), 0755);
            }
            if (i < dir.size()) built += dir[i];
        }
        ::mkdir(dir.c_str(), 0755);
    }

    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Games that ship Tobii's integration DLL, found by scanning.\n"
      << "# Fields are tab separated: game <enabled> <name> <prefix> <dll>\n"
      << "# Add 'root <tab> /path/to/search' to look somewhere unusual.\n\n";
    for (const auto& root : roots_) f << "root\t" << root << "\n";
    if (!roots_.empty()) f << "\n";
    for (const auto& game : games_) {
        f << "game\t" << (game.enabled ? 1 : 0) << '\t' << game.name << '\t'
          << game.prefix << '\t' << game.dll_path
          << (game.user_added ? "\tuser" : "") << "\n";
    }
    return static_cast<bool>(f);
}

}
