// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/sd_update.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sd_update {
namespace fs = std::filesystem;

namespace {

// A parsed version: major.minor.patch, pre-release identifiers ("alpha", "6"),
// and the git describe commit count after the tag.
struct Version {
    bool valid = false;
    unsigned long long core[3] {};
    std::vector<std::string> pre;
    bool post = false;            // built after the tag ("-3-gabc1234" or "-dirty")
    unsigned long long commits = 0;
};

bool digits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }); }
bool hex(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); }); }

std::vector<std::string> split(const std::string& s, char by) {
    std::vector<std::string> out(1);
    for (char c : s) { if (c == by) out.emplace_back(); else out.back() += c; }
    return out;
}

Version parse(std::string s) {
    Version v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    // Strip "-dirty", then "-N-g<hash>": both mean a build after the tag.
    auto drop = [&](const std::string& end) {
        if (s.size() < end.size() || s.compare(s.size() - end.size(), end.size(), end) != 0) return false;
        s.resize(s.size() - end.size());
        return true;
    };
    if (drop("-dirty")) v.post = true;
    auto parts = split(s, '-');
    if (parts.size() >= 3 && parts.back().size() > 1 && parts.back()[0] == 'g' && hex(parts.back().substr(1))
        && digits(parts[parts.size() - 2]) && parts[parts.size() - 2].size() < 10) {
        v.post = true;
        v.commits = std::stoull(parts[parts.size() - 2]);
        s.resize(s.size() - parts.back().size() - parts[parts.size() - 2].size() - 2);
    }
    auto dash = s.find('-');
    std::string core = s.substr(0, dash);
    auto numbers = split(core, '.');
    if (numbers.size() != 3) return v;
    for (size_t n = 0; n < 3; ++n) {
        if (!digits(numbers[n]) || numbers[n].size() > 9) return v;
        v.core[n] = std::stoull(numbers[n]);
    }
    if (dash != std::string::npos) {
        v.pre = split(s.substr(dash + 1), '.');
        for (const auto& word : v.pre)
            if (word.empty() || !std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; })) return v;
    }
    v.valid = true;
    return v;
}

// Semantic versioning order of pre-release identifiers: numeric ones by value
// and before alphanumeric ones, alphanumeric ones as text, and a shorter list
// first when it is a prefix of the other.
int compare_pre(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (size_t n = 0; n < std::min(a.size(), b.size()); ++n) {
        bool an = digits(a[n]) && a[n].size() < 19, bn = digits(b[n]) && b[n].size() < 19;
        if (an && bn) {
            auto x = std::stoull(a[n]), y = std::stoull(b[n]);
            if (x != y) return x < y ? -1 : 1;
        } else if (an != bn) {
            return an ? -1 : 1;
        } else if (int c = a[n].compare(b[n])) {
            return c < 0 ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

// Runs args[0] with stdin and stderr on /dev/null and stores up to 4 KB of
// stdout in `out`. Returns the exit code, or -1 if it could not run or took
// longer than `timeout_ms` (it is then killed).
int run(const std::vector<std::string>& args, std::string& out, int timeout_ms) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipe_fds[0]); close(pipe_fds[1]); return -1; }
    if (pid == 0) { // only async-signal-safe calls from here
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 2); }
        dup2(pipe_fds[1], 1);
        close(pipe_fds[0]);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipe_fds[1]);
    out.clear();
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count());
        if (left <= 0) { timed_out = true; break; }
        pollfd p {pipe_fds[0], POLLIN, 0};
        int ready = poll(&p, 1, left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { timed_out = ready == 0; break; }
        char buffer[256];
        ssize_t got = read(pipe_fds[0], buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (out.size() < 4096) out.append(buffer, size_t(got));
    }
    close(pipe_fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timed_out || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

std::string first_line(const std::string& s) {
    auto end = s.find_first_of("\r\n");
    return s.substr(0, end);
}

} // namespace

int compare_versions(const std::string& a, const std::string& b) {
    Version x = parse(a), y = parse(b);
    if (x.valid != y.valid) return x.valid ? 1 : -1;
    if (!x.valid) return 0;
    for (size_t n = 0; n < 3; ++n)
        if (x.core[n] != y.core[n]) return x.core[n] < y.core[n] ? -1 : 1;
    if (x.pre.empty() != y.pre.empty()) return x.pre.empty() ? 1 : -1; // a pre-release comes before its release
    if (int c = compare_pre(x.pre, y.pre)) return c;
    if (x.post != y.post) return x.post ? 1 : -1;
    if (x.commits != y.commits) return x.commits < y.commits ? -1 : 1;
    return 0;
}

std::vector<std::string> zips_at(const std::string& card, uint64_t limit) {
    std::vector<std::string> out;
    std::error_code error;
    for (fs::directory_iterator it(card, error), end; !error && it != end; it.increment(error)) {
        std::string name = it->path().filename().string();
        if (name.size() < 5 || name[0] == '.') continue;
        std::string ext = name.substr(name.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".zip") continue;
        std::error_code e;
        if (!it->is_regular_file(e) || e) continue;
        auto size = it->file_size(e);
        if (e || size == 0 || size >= limit) continue;
        out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

Offer find(const Setup& setup, const std::string& card, const std::string& current, const std::vector<std::string>& skip) {
    Offer best;
    std::error_code error;
    if (card.empty() || setup.script.empty() || !fs::is_regular_file(setup.script, error)) return best;
    unsigned checked = 0;
    for (const auto& zip : zips_at(card)) {
        if (std::find(skip.begin(), skip.end(), zip) != skip.end()) continue;
        if (++checked > 16) break; // check at most 16 zips
        std::string out;
        if (run({"/bin/sh", setup.script, "check", zip, setup.package, setup.key}, out, 10000) != 0) continue;
        std::string version = first_line(out);
        if (version.empty() || compare_versions(version, current) <= 0) continue;
        if (!best.empty() && compare_versions(version, best.version) <= 0) continue;
        best.zip = zip;
        best.version = version;
        best.size = fs::file_size(zip, error);
    }
    return best;
}

bool start(const Setup& setup, const Offer& offer, const std::string& card, std::string& error) {
    if (offer.empty()) { error = "no update selected"; return false; }
    std::string out;
    int code = run({"/usr/bin/systemd-run", "--unit=sample-base-update", "--collect", "--no-block",
                    "--property=Type=oneshot", "--property=TimeoutStartSec=600",
                    "/bin/sh", setup.script, "install", offer.zip, card, setup.package, setup.app, setup.key}, out, 10000);
    if (code == 0) return true;
    error = code < 0 ? "systemd-run timed out" : "systemd-run failed (is an update already running?)";
    return false;
}

Status read_status(const std::string& path) {
    Status status;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (value.size() > 64) value.resize(64);
        if (key == "state") status.state = value;
        else if (key == "version") status.version = value;
        else if (key == "error") status.error = value;
    }
    return status;
}

} // namespace sd_update
