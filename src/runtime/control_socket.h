// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <sys/un.h>
#include <functional>
#include <string>

// The control protocol's transport: one text command per datagram on the
// private Unix socket `<runtime>/control.sock`, answered by one reply that
// starts with "OK" or "ERR". The commands are in Controller::command().

// The app's end. Creates the runtime directory if needed and refuses one
// that other users can reach.
class ControlServer {
public:
    explicit ControlServer(const std::string& runtime);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    // Waits up to `timeout_ms` for one request and sends `handle`'s reply.
    void serve(int timeout_ms, const std::function<std::string(const std::string&)>& handle);
private:
    std::string path;
    int fd = -1;
};

// A tool's end, bound to its own socket in the same runtime directory.
class ControlClient {
public:
    explicit ControlClient(const std::string& runtime);
    ~ControlClient();
    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;
    // Empty when the app does not answer. A late reply to an earlier request
    // is discarded by draining before each send.
    std::string request(const std::string& command);
private:
    int fd = -1;
    sockaddr_un local {}, remote {};
};

// --ctl: sends one command and prints the reply. Exits 0 only for "OK".
int send_control_command(const std::string& runtime, const std::string& command);
// --ctl-stream: one command per stdin line and one reply per stdout line, so
// a single SSH session can drive interactive tools such as control calibration.
int stream_control_commands(const std::string& runtime);
