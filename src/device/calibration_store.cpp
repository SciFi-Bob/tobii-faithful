#include "tobii/device/calibration_store.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace tobii::device {
namespace {

constexpr char kMagic[] = "TBCAL1";

std::string FileName(const std::string& key) {
    std::string out;
    bool last_dash = false;
    for (char c : key) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            last_dash = false;
        } else if (!last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out + ".cal";
}

void MkdirParents(const std::string& dir) {
    std::string built;
    std::size_t pos = 0;
    while (pos <= dir.size()) {
        const std::size_t slash = dir.find('/', pos);
        const std::string part = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
        if (!part.empty()) ::mkdir(part.c_str(), 0755);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
}

}

std::string ContentHash(const std::vector<std::uint8_t>& blob) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : blob) {
        h ^= b;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

ReconcileAction ReconcileDecision(bool have_record, std::uint64_t record_size,
                                  const std::string& record_hash,
                                  const DeviceCalibrationState& device) {
    if (!have_record) {
        return (device.present && device.size > 0) ? ReconcileAction::kClear
                                                   : ReconcileAction::kNothing;
    }
    if (device.present && device.size == record_size && device.hash == record_hash) {
        return ReconcileAction::kNothing;
    }
    return ReconcileAction::kApply;
}

std::string CalibrationRecord::Key() const {
    return "tobii-prp://" + serial + "&" + monitor_id;
}

std::string CalibrationStore::DefaultDir() {
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data != '\0') {
        return std::string(data) + "/tobii-faithful/calibrations";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::string(home) + "/.local/share/tobii-faithful/calibrations";
    }
    return "/tmp/tobii-calibrations";
}

CalibrationStore::CalibrationStore() : dir_(DefaultDir()) {}
CalibrationStore::CalibrationStore(std::string dir) : dir_(std::move(dir)) {}

std::string CalibrationStore::PathFor(const std::string& serial,
                                      const std::string& monitor_id) const {
    CalibrationRecord k;
    k.serial = serial;
    k.monitor_id = monitor_id;
    return dir_ + "/" + FileName(k.Key());
}

bool CalibrationStore::Save(CalibrationRecord rec) {
    if (rec.serial.empty() || rec.monitor_id.empty()) return false;
    if (rec.timestamp_unix == 0) rec.timestamp_unix = static_cast<std::int64_t>(std::time(nullptr));

    rec.data_size = rec.blob.size();
    rec.content_hash = ContentHash(rec.blob);

    MkdirParents(dir_);

    std::string header;
    {
        std::ostringstream h;
        h << kMagic << '\n'
          << "key " << rec.Key() << '\n'
          << "serial " << rec.serial << '\n'
          << "monitor " << rec.monitor_id << '\n'
          << "data_size " << rec.data_size << '\n'
          << "content_hash " << rec.content_hash << '\n'
          << "num_points " << rec.num_points << '\n'
          << "timestamp " << rec.timestamp_unix << '\n'
          << "blob_size " << rec.blob.size() << '\n'
          << "stimulus_size " << rec.stimulus.size() << '\n'
          << '\n';
        header = h.str();
    }

    const std::string path = PathFor(rec.serial, rec.monitor_id);
    const std::string temp = path + ".tmp";

    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    const auto write_all = [&](const void* data, std::size_t len) {
        const char* p = static_cast<const char*>(data);
        std::size_t off = 0;
        while (off < len) {
            const ssize_t n = ::write(fd, p + off, len - off);
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    };

    bool ok = write_all(header.data(), header.size());
    if (ok && !rec.blob.empty()) ok = write_all(rec.blob.data(), rec.blob.size());
    if (ok && !rec.stimulus.empty()) ok = write_all(rec.stimulus.data(), rec.stimulus.size());
    if (ok && ::fsync(fd) != 0) ok = false;
    ::close(fd);
    if (!ok) { ::unlink(temp.c_str()); return false; }

    if (::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        return false;
    }
    if (const int dir_fd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
    return true;
}

std::optional<CalibrationRecord> CalibrationStore::Load(const std::string& serial,
                                                        const std::string& monitor_id) const {
    const std::string path = PathFor(serial, monitor_id);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;

    std::string all;
    {
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) { ::close(fd); return std::nullopt; }
            if (n == 0) break;
            all.append(buf, static_cast<std::size_t>(n));
        }
    }
    ::close(fd);

    const std::size_t sep = all.find("\n\n");
    if (sep == std::string::npos) return std::nullopt;
    const std::string header = all.substr(0, sep + 1);
    const std::size_t body = sep + 2;

    std::istringstream h(header);
    std::string line;
    if (!std::getline(h, line) || line != kMagic) return std::nullopt;

    CalibrationRecord rec;
    std::size_t blob_size = 0;
    std::size_t stimulus_size = 0;
    bool have_blob_size = false;
    bool have_stimulus_size = false;
    while (std::getline(h, line)) {
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "serial") ss >> rec.serial;
        else if (key == "monitor") ss >> rec.monitor_id;
        else if (key == "data_size") ss >> rec.data_size;
        else if (key == "content_hash") ss >> rec.content_hash;
        else if (key == "num_points") ss >> rec.num_points;
        else if (key == "timestamp") ss >> rec.timestamp_unix;
        else if (key == "blob_size") { ss >> blob_size; have_blob_size = true; }
        else if (key == "stimulus_size") { ss >> stimulus_size; have_stimulus_size = true; }
    }
    if (!have_blob_size || !have_stimulus_size) return std::nullopt;
    if (body + blob_size + stimulus_size != all.size()) return std::nullopt;

    const auto* p = reinterpret_cast<const std::uint8_t*>(all.data()) + body;
    rec.blob.assign(p, p + blob_size);
    rec.stimulus.assign(p + blob_size, p + blob_size + stimulus_size);
    return rec;
}

}
