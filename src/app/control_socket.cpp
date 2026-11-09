// SPDX-License-Identifier: AGPL-3.0-only
#include "app/control_socket.h"
#include "app/util.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace {
constexpr size_t request_limit = 4096; // Longer requests get "ERR command too long".
constexpr size_t reply_limit = 32768;  // Longer replies become "ERR response too large".
const char* const socket_name = "/control.sock";
} // namespace

ControlServer::ControlServer(const std::string& runtime) : path(runtime + socket_name) {
    require(path.size() < sizeof(sockaddr_un::sun_path), "Runtime path is too long");
    std::filesystem::create_directories(runtime);
    struct stat directory {};
    require(stat(runtime.c_str(), &directory) == 0 && directory.st_uid == geteuid() && (directory.st_mode & 077) == 0,
            "Runtime directory must be private and owned by this user");
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    require(fd >= 0, "Create control socket");
    sockaddr_un address {}; address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path.c_str());
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd); fd = -1; throw std::runtime_error("Bind control socket");
    }
    chmod(path.c_str(), 0600);
}

ControlServer::~ControlServer() { if (fd >= 0) close(fd); unlink(path.c_str()); }

void ControlServer::serve(int timeout_ms, const std::function<std::string(const std::string&)>& handle) {
    pollfd ready {fd, POLLIN, 0};
    int result = poll(&ready, 1, timeout_ms);
    if (result < 0 && errno != EINTR) throw std::runtime_error("Control poll failed");
    if (!(ready.revents & POLLIN)) return;
    sockaddr_un sender {}; socklen_t size = sizeof(sender);
    char request[request_limit] {};
    auto length = recvfrom(fd, request, sizeof(request) - 1, MSG_TRUNC, reinterpret_cast<sockaddr*>(&sender), &size);
    if (length <= 0) return;
    std::string reply = length >= ssize_t(sizeof(request)) ? "ERR command too long" : handle(request);
    if (reply.size() >= reply_limit) reply = "ERR response too large";
    sendto(fd, reply.data(), reply.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sender), size);
}

ControlClient::ControlClient(const std::string& runtime) {
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "Create client socket");
    local.sun_family = remote.sun_family = AF_UNIX;
    std::string local_path = runtime + "/client-" + std::to_string(getpid()) + ".sock", remote_path = runtime + socket_name;
    require(local_path.size() < sizeof(local.sun_path) && remote_path.size() < sizeof(remote.sun_path), "Runtime path is too long");
    strcpy(local.sun_path, local_path.c_str()); strcpy(remote.sun_path, remote_path.c_str());
    unlink(local.sun_path);
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) { close(fd); throw std::runtime_error("Bind client socket"); }
}

ControlClient::~ControlClient() { unlink(local.sun_path); close(fd); }

std::string ControlClient::request(const std::string& command) {
    char reply[reply_limit];
    while (recv(fd, reply, sizeof(reply), MSG_DONTWAIT) > 0) {}
    if (sendto(fd, command.data(), command.size(), 0, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) return {};
    pollfd ready {fd, POLLIN, 0};
    if (poll(&ready, 1, 3500) <= 0) return {};
    auto size = recv(fd, reply, sizeof(reply) - 1, 0);
    return size > 0 ? std::string(reply, size_t(size)) : std::string();
}

int send_control_command(const std::string& runtime, const std::string& command) {
    auto reply = ControlClient(runtime).request(command);
    if (reply.empty()) { std::cerr << "No reply from sample-base\n"; return 1; }
    std::cout << reply << '\n';
    return reply.compare(0, 2, "OK") != 0;
}

int stream_control_commands(const std::string& runtime) {
    ControlClient client(runtime);
    for (std::string command; std::getline(std::cin, command);) {
        auto reply = client.request(command);
        if (reply.empty()) { std::cout << "ERR no reply from sample-base" << std::endl; return 1; }
        std::replace(reply.begin(), reply.end(), '\n', ' ');
        std::cout << reply << std::endl;
    }
    return 0;
}
