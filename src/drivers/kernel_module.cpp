// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/kernel_module.h"
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstring>

extern char** environ;

bool kernel_module::loaded(const std::string& name) { return access(("/sys/module/" + name).c_str(), F_OK) == 0; }

bool kernel_module::load(const std::string& name, const unsigned char* begin, const unsigned char* end, std::string* error) {
    if (loaded(name)) return true;
    auto fail = [&](const std::string& why) { if (error) *error = name + ": " + why; return false; };
    if (!begin || !end || end <= begin) return fail("not in this build");
    const size_t size = size_t(end - begin);
    int fd = memfd_create(name.c_str(), MFD_CLOEXEC);
    if (fd < 0) return fail(std::strerror(errno));
    bool written = true;
    for (size_t done = 0; done < size && written;) {
        ssize_t n = write(fd, begin + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        written = n > 0;
        if (written) done += size_t(n);
    }
    long result = written ? syscall(SYS_finit_module, fd, "", 0) : -1;
    int code = errno;
    close(fd);
    if (result == 0 || (written && code == EEXIST)) return true;
    return fail(written ? std::strerror(code) : "could not copy it to memory");
}

bool kernel_module::unload(const std::string& name) {
    if (!loaded(name)) return true;
    return syscall(SYS_delete_module, name.c_str(), O_NONBLOCK) == 0 || !loaded(name);
}

bool kernel_module::probe(const std::vector<std::string>& names, std::string* error) {
    std::vector<std::string> words {"modprobe", "-a"};
    words.insert(words.end(), names.begin(), names.end());
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, "modprobe", nullptr, nullptr, argv.data(), environ) != 0) {
        if (error) *error = "modprobe: could not start it";
        return false;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok && error) {
        *error = "modprobe";
        for (const auto& name : names) *error += " " + name;
        *error += ": failed";
    }
    return ok;
}
