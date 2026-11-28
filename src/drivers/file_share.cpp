// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "drivers/file_share.h"
#include "drivers/kernel_module.h"
#include "drivers/smb_pipe.h"
#include "drivers/usb_gadget.h"
#include <glob.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

// kernel/ksmbd's module for each firmware's kernel (src/drivers/smb_module.S).
extern "C" const unsigned char sample_base_smb_module[] __attribute__((weak));
extern "C" const unsigned char sample_base_smb_module_end[] __attribute__((weak));
extern "C" const unsigned char sample_base_smb_6_18_module[] __attribute__((weak));
extern "C" const unsigned char sample_base_smb_6_18_module_end[] __attribute__((weak));

namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* module_name = "sample_base_smb";
constexpr const char* kill_server = "/sys/class/ksmbd-control/kill_server";
constexpr const char* server_stats = "/sys/class/ksmbd-control/stats"; // "2 running 445 ...": version, state, port

// The server's messages (kernel/ksmbd/fs/smb/server/ksmbd_netlink.h): the
// generic netlink command is the event, and its one attribute, of the same
// type, carries the struct. Each answer is the request's event plus one.
namespace event {
enum : uint8_t {
    Heartbeat = 1, StartingUp, ShuttingDown, LoginRequest, LoginResponse, ShareConfigRequest, ShareConfigResponse,
    TreeConnectRequest, TreeConnectResponse, TreeDisconnectRequest, LogoutRequest, RpcRequest, RpcResponse,
};
}
constexpr uint8_t genl_version = 1;
#pragma pack(push, 1)
struct StartupRequest {
    uint32_t flags;
    int32_t signing;
    char min_prot[16], max_prot[16], netbios_name[16], work_group[64], server_string[64];
    uint16_t tcp_port, ipc_timeout;
    uint32_t deadtime, file_max, smb2_max_write, smb2_max_read, smb2_max_trans, share_fake_fscaps, sub_auth[3];
    uint32_t smb2_max_credits, smbd_max_io_size, max_connections;
    int8_t bind_interfaces_only;
    uint32_t max_ip_connections;
    int8_t reserved[499];
    uint32_t ifc_list_sz; // Then the interfaces' names, each ending in NUL
};
#pragma pack(pop)
struct LoginRequest { uint32_t handle; char account[48]; uint32_t reserved[16]; };
struct LoginResponse { uint32_t handle, gid, uid; char account[48]; uint16_t status, hash_sz; char hash[18]; uint32_t reserved[16]; };
struct ShareConfigRequest { uint32_t handle; char share_name[64]; uint32_t reserved[16]; };
struct ShareConfigResponse {
    uint32_t handle, flags;
    uint16_t create_mask, directory_mask, force_create_mode, force_directory_mode, force_uid, force_gid;
    char share_name[64];
    uint32_t reserved[111], payload_sz, veto_list_sz; // Then the veto list and the path
};
struct TreeConnectRequest {
    uint32_t handle; uint16_t account_flags, flags; uint64_t session_id, connect_id;
    char account[48], share[64], peer_addr[64]; uint32_t reserved[16];
};
struct TreeConnectResponse { uint32_t handle; uint16_t status, connection_flags; uint32_t reserved[16]; };
struct TreeDisconnectRequest { uint64_t session_id, connect_id; uint32_t reserved[16]; };
struct RpcCommand { uint32_t handle, flags, payload_sz; }; // Then the payload
static_assert(sizeof(StartupRequest) == 744 && sizeof(LoginRequest) == 116 && sizeof(LoginResponse) == 148);
static_assert(sizeof(ShareConfigRequest) == 132 && sizeof(ShareConfigResponse) == 536);
static_assert(sizeof(TreeConnectRequest) == 264 && sizeof(TreeConnectResponse) == 72 && sizeof(TreeDisconnectRequest) == 80);

enum : uint16_t { UserOk = 1 << 0, UserGuest = 1 << 4 };
enum : uint32_t { ShareAvailable = 1 << 0, ShareBrowseable = 1 << 1, ShareWriteable = 1 << 2, ShareGuestOk = 1 << 4,
                  SharePipe = 1 << 8, ShareHideDotFiles = 1 << 9, ShareCrossMount = 1 << 15 };
enum : uint16_t { TreeOk = 0, TreeNoShare = 2 };
enum : uint16_t { TreeGuest = 1 << 0, TreeWritable = 1 << 2, TreeUpdate = 1 << 4 };
enum : uint32_t { RpcSrvsvc = 1 << 1, RpcWkssvc = 1 << 2, RpcIoctl = 1 << 3, RpcOpen = 1 << 4, RpcWrite = 1 << 5,
                  RpcRead = 1 << 6, RpcClose = 1 << 7, RpcRap = 1 << 8, RpcSamr = 1 << 10, RpcLsarpc = 1 << 11 };
enum : uint32_t { RpcStatusOk = 0, RpcBadHandle = 6, RpcNotImplemented = 0x40 };
// The NT hash (MD4 of UTF-16) of an empty password.
constexpr uint8_t empty_password[16] = {0x31, 0xd6, 0xcf, 0xe0, 0xd1, 0x6a, 0xe9, 0x31, 0xb7, 0x3c, 0x59, 0xd7, 0xe0, 0xc0, 0x89, 0xc0};
constexpr uint16_t no_owner = 0xffff; // KSMBD_SHARE_INVALID_UID: files are made as the login's user

template <size_t N> void copy(char (&to)[N], const std::string& text) { std::strncpy(to, text.c_str(), N - 1); to[N - 1] = 0; }
template <size_t N> std::string text(const char (&from)[N]) { return std::string(from, strnlen(from, N)); }
bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

// The USB-C network's interface: the gadget's ncm function names it.
std::string network_interface() {
    glob_t found {};
    std::string name;
    if (glob((std::string(gadget_root) + "/*/functions/ncm.*/ifname").c_str(), 0, nullptr, &found) == 0 && found.gl_pathc)
        std::getline(std::ifstream(found.gl_pathv[0]), name);
    globfree(&found);
    return name.empty() || name.find('%') != std::string::npos ? "usb0" : name;
}

// One generic netlink socket to the server.
class Netlink {
public:
    ~Netlink() { if (fd >= 0) ::close(fd); }
    bool open(std::string& error) {
        if (fd >= 0) return true;
        fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
        if (fd < 0) { error = std::string("netlink: ") + std::strerror(errno); return false; }
        sockaddr_nl local {}; local.nl_family = AF_NETLINK;
        int size = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) { error = std::string("netlink: ") + std::strerror(errno); close(); return false; }
        // The family's number, from generic netlink's controller.
        static const char name[] = "SMBD_GENL";
        if (!send(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME, name, sizeof(name), 0)) { error = "netlink: no controller"; close(); return false; }
        std::vector<uint8_t> buffer(8192);
        pollfd wait {fd, POLLIN, 0};
        ssize_t received = poll(&wait, 1, 1000) > 0 ? recv(fd, buffer.data(), buffer.size(), 0) : -1;
        unsigned got = received > 0 ? unsigned(received) : 0;
        for (auto* n = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(n, got); n = NLMSG_NEXT(n, got)) {
            if (n->nlmsg_type != GENL_ID_CTRL) continue;
            each_attribute(n, [&](uint16_t type, const uint8_t* data, size_t length) {
                if (type == CTRL_ATTR_FAMILY_ID && length >= 2) std::memcpy(&family, data, 2);
            });
        }
        if (!family) { error = "the SMB server is not in the kernel"; close(); return false; }
        return true;
    }
    void close() { if (fd >= 0) ::close(fd); fd = -1; family = 0; }
    // One message to the kernel: `event` as the command and as the attribute. Returns its sequence number, 0 when not sent.
    uint32_t send(uint8_t event, const void* payload, size_t size, bool ack = false) {
        return send(family, event, event, payload, size, ack ? NLM_F_ACK : 0);
    }
    template <typename Each> static void each_attribute(const nlmsghdr* n, Each each) {
        auto* at = reinterpret_cast<const uint8_t*>(NLMSG_DATA(n)) + GENL_HDRLEN;
        auto* end = reinterpret_cast<const uint8_t*>(n) + n->nlmsg_len;
        while (at + NLA_HDRLEN <= end) {
            auto* attribute = reinterpret_cast<const nlattr*>(at);
            if (attribute->nla_len < NLA_HDRLEN || at + attribute->nla_len > end) break;
            each(uint16_t(attribute->nla_type & NLA_TYPE_MASK), at + NLA_HDRLEN, size_t(attribute->nla_len - NLA_HDRLEN));
            at += NLA_ALIGN(attribute->nla_len);
        }
    }
    int fd = -1;
    uint16_t family = 0;

private:
    uint32_t send(uint16_t to, uint8_t command, uint16_t attribute, const void* payload, size_t size, uint16_t flags) {
        if (fd < 0 || NLA_HDRLEN + size > 0xffff) return 0;
        std::vector<uint8_t> message(NLMSG_HDRLEN + GENL_HDRLEN + NLA_HDRLEN + NLA_ALIGN(size));
        auto* n = reinterpret_cast<nlmsghdr*>(message.data());
        n->nlmsg_len = uint32_t(message.size()); n->nlmsg_type = to; n->nlmsg_flags = uint16_t(NLM_F_REQUEST | flags);
        n->nlmsg_seq = ++sequence;
        auto* g = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(n));
        g->cmd = command; g->version = to == GENL_ID_CTRL ? 1 : genl_version;
        auto* a = reinterpret_cast<nlattr*>(message.data() + NLMSG_HDRLEN + GENL_HDRLEN);
        a->nla_len = uint16_t(NLA_HDRLEN + size); a->nla_type = attribute;
        std::memcpy(message.data() + NLMSG_HDRLEN + GENL_HDRLEN + NLA_HDRLEN, payload, size);
        sockaddr_nl kernel {}; kernel.nl_family = AF_NETLINK;
        ssize_t sent = sendto(fd, message.data(), message.size(), 0, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel));
        return sent == ssize_t(message.size()) ? sequence : 0;
    }
    uint32_t sequence = 0;
};

// The module and the crypto it asks the kernel for by name (MD5 for NTLM,
// CMAC, CCM and GCM for SMB 3) and the UTF-8 tables; libmd5 is 6.18's MD5,
// called directly. Each is tried alone: one a kernel lacks is not an error
// until the server needs it.
bool load_module(std::string& error) {
    if (kernel_module::loaded(module_name)) return true;
    for (const char* name : {"md5", "libmd5", "cmac", "ccm", "gcm", "nls_utf8"}) kernel_module::probe({name});
    return kernel_module::load(module_name, {{sample_base_smb_module, sample_base_smb_module_end},
                                             {sample_base_smb_6_18_module, sample_base_smb_6_18_module_end}}, &error);
}

// The server's state: "startup" (waiting for a process to start it),
// "running", "reset" or "shutdown"; "" without the module.
std::string server_state() {
    std::string version, state;
    std::ifstream(server_stats) >> version >> state;
    return state;
}

// Stops the server at once, closing every connection; it waits for a start.
void stop_server() {
    if (!kernel_module::loaded(module_name)) return;
    std::ofstream(kill_server) << "hard";
}
} // namespace

FileShare::FileShare() : thread([this] { run(); }) {}

FileShare::~FileShare() {
    stopping.store(true);
    changed.notify_all();
    if (thread.joinable()) thread.join();
}

void FileShare::follow(const FileSharing& want, const std::string& card, bool plugged) {
    std::lock_guard<std::mutex> hold(lock);
    if (followed && want.on == wanted.on && want.name == wanted.name && card == folder && plugged == connected) return;
    wanted = want; folder = card; connected = plugged; followed = true;
    changed.notify_all();
}

FileShareStatus FileShare::status() const {
    std::lock_guard<std::mutex> hold(lock);
    return current;
}

void FileShare::let_go() {
    {
        std::lock_guard<std::mutex> hold(lock);
        folder.clear();
        ++generation;
    }
    stop_server();
    users.store(0);
    changed.notify_all();
}

void FileShare::run() {
    Netlink link;
    bool started = false, stopped = false, serving = false;
    unsigned seen = 0;
    uint32_t startup = 0;            // The start message's sequence number, until the kernel answers it
    std::string answered;            // The card's folder as the server was last told it
    std::string last_folder;
    std::map<std::pair<uint64_t, uint64_t>, bool> trees; // (session, connection) -> on "SD Card"
    std::map<uint32_t, SmbPipe> pipes;
    Clock::time_point retry {}, unplugged {};
    bool was_connected = false;
    std::vector<uint8_t> buffer(1 << 18);
    auto forget = [&] { trees.clear(); pipes.clear(); answered.clear(); users.store(0); started = serving = false; startup = 0; };
    Clock::time_point next_look {}; // When to check next that the server still runs
    auto report = [&](FileShareState state, const std::string& error) {
        unsigned count = unsigned(std::count_if(trees.begin(), trees.end(), [](const auto& tree) { return tree.second; }));
        users.store(count);
        std::lock_guard<std::mutex> hold(lock);
        current.state = state; current.error = error; current.computers = count;
    };
    auto wait = [&](int ms) {
        std::unique_lock<std::mutex> hold(lock);
        changed.wait_for(hold, std::chrono::milliseconds(ms));
    };

    while (!stopping.load()) {
        FileSharing want; std::string card; bool plugged, asked; unsigned generation_now;
        {
            std::lock_guard<std::mutex> hold(lock);
            want = wanted; card = folder; plugged = connected; asked = followed; generation_now = generation;
        }
        if (!asked) { wait(250); continue; }
        if (generation_now != seen) { seen = generation_now; forget(); } // let_go() stopped it
        if (!want.on) {
            if (started || !stopped) { stop_server(); forget(); stopped = true; } // An earlier run's server too
            report(FileShareState::Off, {});
            wait(1000);
            continue;
        }
        stopped = false;
        // Nothing a connection named is there any more: another card, none,
        // or the computer unplugged for 5 s. Start again without them.
        auto now = Clock::now();
        if (was_connected && !plugged) unplugged = now;
        was_connected = plugged;
        bool card_users = std::any_of(trees.begin(), trees.end(), [](const auto& tree) { return tree.second; });
        if (started && ((card != last_folder && card_users) || (!plugged && !trees.empty() && now - unplugged > std::chrono::seconds(5)))) {
            stop_server(); forget();
        }
        last_folder = card;
        if (!started && now >= retry) {
            std::string error;
            if (load_module(error) && link.open(error)) {
                std::string interface = network_interface();
                std::vector<uint8_t> message(sizeof(StartupRequest) + interface.size() + 1);
                auto* s = reinterpret_cast<StartupRequest*>(message.data());
                s->signing = 2;                        // Auto: signed when the computer asks
                copy(s->netbios_name, want.name.substr(0, 15));
                copy(s->work_group, "WORKGROUP");
                copy(s->server_string, want.name);
                s->tcp_port = 445;
                s->ipc_timeout = 10;                   // The server stops 10 to 20 s after this run ends
                s->file_max = 10000;
                s->share_fake_fscaps = 64;             // FILE_SUPPORTS_SPARSE_FILES, as Samba says
                s->sub_auth[0] = 21; s->sub_auth[1] = 0x4d50433a; s->sub_auth[2] = 0x53616d70; // The server's SID
                s->bind_interfaces_only = 1;
                s->ifc_list_sz = uint32_t(interface.size() + 1);
                std::memcpy(message.data() + sizeof(StartupRequest), interface.c_str(), interface.size() + 1);
                startup = link.send(event::StartingUp, message.data(), message.size(), true);
                if (!startup) error = "the SMB server did not take its settings";
            }
            if (startup) { started = true; report(FileShareState::Starting, {}); }
            else { report(FileShareState::Failed, error); retry = now + std::chrono::seconds(5); }
        }
        if (!started) { wait(250); continue; }
        // Reset under this run (kill_server from elsewhere, or a heartbeat
        // that found no answer): it waits in "startup" for a start, so start
        // it again. Checked once a second, from 2 s after it said it runs.
        if (serving && now >= next_look) {
            next_look = now + std::chrono::seconds(1);
            if (server_state() == "startup") {
                std::cerr << "File Sharing: the SMB server was reset; starting it again" << std::endl;
                forget();
                continue;
            }
        }

        pollfd ready {link.fd, POLLIN, 0};
        if (poll(&ready, 1, 250) <= 0) continue;
        ssize_t received = recv(link.fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (received <= 0) continue;
        unsigned got = unsigned(received);
        std::vector<SmbShareInfo> shares {{file_share_name, "", smb_share_disk}, {"IPC$", "IPC Service", smb_share_ipc}};
        if (card.empty()) shares.erase(shares.begin());
        for (auto* n = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(n, got); n = NLMSG_NEXT(n, got)) {
            if (n->nlmsg_type == NLMSG_ERROR) {
                auto* failure = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(n));
                if (startup && n->nlmsg_seq == startup) {
                    startup = 0;
                    if (failure->error == 0) {
                        serving = true;
                        next_look = now + std::chrono::seconds(2); // Its start finishes on a kernel worker
                        report(FileShareState::Serving, {});
                    }
                    else {
                        std::string error = std::string("the SMB server refused to start: ") + std::strerror(-failure->error);
                        std::cerr << "File Sharing: " << error << std::endl;
                        forget(); report(FileShareState::Failed, error); retry = now + std::chrono::seconds(5);
                    }
                }
                continue;
            }
            if (n->nlmsg_type != link.family) continue;
            auto* g = reinterpret_cast<const genlmsghdr*>(NLMSG_DATA(n));
            const uint8_t* data = nullptr; size_t size = 0;
            Netlink::each_attribute(n, [&](uint16_t type, const uint8_t* at, size_t length) { if (type == g->cmd) { data = at; size = length; } });
            if (!data) continue;
            switch (g->cmd) {
            case event::LoginRequest: {
                // "guest" (or no name) is a guest. Any other name is a user
                // whose password is empty: the server checks it, and the
                // session has a key to sign with. macOS turns away a named
                // login answered as a guest; Windows 11 turns guests away.
                if (size < sizeof(LoginRequest)) break;
                LoginRequest request; std::memcpy(&request, data, sizeof(request));
                std::string account = text(request.account);
                bool guest = account.empty() || same_name(account, "guest");
                LoginResponse answer {};
                answer.handle = request.handle;
                copy(answer.account, account);
                answer.status = uint16_t(UserOk | (guest ? UserGuest : 0));
                answer.hash_sz = sizeof(empty_password);
                if (!guest) std::memcpy(answer.hash, empty_password, sizeof(empty_password));
                link.send(event::LoginResponse, &answer, sizeof(answer));
                break;
            }
            case event::ShareConfigRequest: {
                if (size < sizeof(ShareConfigRequest)) break;
                ShareConfigRequest request; std::memcpy(&request, data, sizeof(request));
                std::string name = text(request.share_name);
                std::vector<uint8_t> message(sizeof(ShareConfigResponse));
                auto* answer = reinterpret_cast<ShareConfigResponse*>(message.data());
                answer->handle = request.handle;
                if (same_name(name, "IPC$")) {
                    answer->flags = ShareAvailable | SharePipe;
                    copy(answer->share_name, "IPC$");
                } else if (same_name(name, file_share_name) && !card.empty()) {
                    // CROSSMNT: the card's folder is where it is mounted, and
                    // the server finds a share's top folder without crossing
                    // into a mount unless told to.
                    answer->flags = ShareAvailable | ShareBrowseable | ShareWriteable | ShareGuestOk | ShareHideDotFiles | ShareCrossMount;
                    answer->create_mask = 0744; answer->directory_mask = 0755;
                    answer->force_uid = no_owner; answer->force_gid = no_owner;
                    copy(answer->share_name, file_share_name);
                    answer->payload_sz = uint32_t(card.size() + 1);
                    message.insert(message.end(), card.c_str(), card.c_str() + card.size() + 1);
                    answered = card;
                } // Else no such share: flags 0
                link.send(event::ShareConfigResponse, message.data(), message.size());
                break;
            }
            case event::TreeConnectRequest: {
                if (size < sizeof(TreeConnectRequest)) break;
                TreeConnectRequest request; std::memcpy(&request, data, sizeof(request));
                std::string name = text(request.share);
                TreeConnectResponse answer {};
                answer.handle = request.handle;
                bool on_card = same_name(name, file_share_name);
                if (same_name(name, "IPC$")) answer.connection_flags = TreeGuest;
                else if (on_card && !card.empty()) answer.connection_flags = uint16_t(TreeGuest | TreeWritable | (answered == card ? 0 : TreeUpdate));
                else answer.status = TreeNoShare;
                if (answer.status == TreeOk) trees[{request.session_id, request.connect_id}] = on_card;
                link.send(event::TreeConnectResponse, &answer, sizeof(answer));
                report(FileShareState::Serving, {});
                break;
            }
            case event::TreeDisconnectRequest: {
                if (size < sizeof(TreeDisconnectRequest)) break;
                TreeDisconnectRequest request; std::memcpy(&request, data, sizeof(request));
                trees.erase({request.session_id, request.connect_id});
                report(FileShareState::Serving, {});
                break;
            }
            case event::RpcRequest: {
                if (size < sizeof(RpcCommand)) break;
                RpcCommand request; std::memcpy(&request, data, sizeof(request));
                const uint8_t* payload = data + sizeof(request);
                size_t length = std::min<size_t>(request.payload_sz, size - sizeof(request));
                RpcCommand answer {request.handle, RpcStatusOk, 0};
                std::vector<uint8_t> reply;
                auto pipe = pipes.find(request.handle);
                if (request.flags & RpcIoctl) {
                    if (pipe == pipes.end()) answer.flags = RpcBadHandle;
                    else reply = pipe->second.transact(payload, length, shares);
                } else if (request.flags & RpcRead) {
                    if (pipe == pipes.end()) answer.flags = RpcBadHandle;
                    else reply = pipe->second.read();
                } else if (request.flags & RpcWrite) {
                    if (pipe == pipes.end()) answer.flags = RpcBadHandle;
                    else pipe->second.write(payload, length, shares);
                } else if (request.flags & RpcOpen) {
                    const char* name = request.flags & RpcSrvsvc ? "srvsvc" : request.flags & RpcWkssvc ? "wkssvc"
                                     : request.flags & RpcSamr ? "samr" : request.flags & RpcLsarpc ? "lsarpc" : "";
                    pipes.insert_or_assign(request.handle, SmbPipe(name));
                } else if (request.flags & RpcClose) {
                    pipes.erase(request.handle);
                } else answer.flags = RpcNotImplemented; // RAP: SMB 1's share list
                answer.payload_sz = uint32_t(reply.size());
                std::vector<uint8_t> message(sizeof(answer) + reply.size());
                std::memcpy(message.data(), &answer, sizeof(answer));
                if (!reply.empty()) std::memcpy(message.data() + sizeof(answer), reply.data(), reply.size());
                link.send(event::RpcResponse, message.data(), message.size());
                break;
            }
            default: break; // Heartbeat, logout: nothing to answer
            }
        }
    }
}
