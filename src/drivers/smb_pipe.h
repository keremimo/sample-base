// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One named pipe a computer opened on File Sharing's IPC$ share
// (drivers/file_share.h): DCE/RPC (MS-RPCE, connection-oriented, NDR,
// little-endian) as the kernel's SMB server hands it over. A bind (or alter
// context) to any interface is taken with NDR 2.0 and every other transfer
// syntax refused. On srvsvc, NetrShareEnum (opnum 15, levels 0 and 1) lists
// the shares and NetrShareGetInfo (opnum 16, levels 0, 1 and 1005) describes
// one (MS-SRVS 3.1.4.8, 3.1.4.10): what a computer's file browser asks to show
// the server's shares. Every other call gets a fault (nca_s_op_rng_error),
// which clients take as "not supported". Plain C++, for the tests.
constexpr uint32_t smb_share_disk = 0;          // STYPE_DISKTREE
constexpr uint32_t smb_share_ipc = 0x80000003;  // STYPE_IPC | STYPE_SPECIAL

struct SmbShareInfo {
    std::string name, remark; // UTF-8
    uint32_t type = smb_share_disk;
};

class SmbPipe {
public:
    // `name` as the computer opened it: "srvsvc", "\\srvsvc", "wkssvc"...
    explicit SmbPipe(std::string name);
    // PDUs from the computer (one, or several back to back) and the replies
    // to them; a fragment of a longer request waits for the rest.
    std::vector<uint8_t> transact(const uint8_t* data, size_t size, const std::vector<SmbShareInfo>& shares);
    // A write keeps its replies for the next read (SMB2 WRITE, then READ).
    void write(const uint8_t* data, size_t size, const std::vector<SmbShareInfo>& shares);
    std::vector<uint8_t> read();

private:
    std::vector<uint8_t> answer(const uint8_t* pdu, size_t size, const std::vector<SmbShareInfo>& shares);
    std::vector<uint8_t> bind_ack(const uint8_t* pdu, size_t size, uint8_t type);
    std::vector<uint8_t> call(uint16_t opnum, const std::vector<uint8_t>& stub, const std::vector<SmbShareInfo>& shares, bool& known);

    std::string interface; // "srvsvc"
    bool srvsvc = false;
    uint16_t max_xmit = 4280;
    uint32_t assoc_group = 0;
    std::vector<uint8_t> pending; // replies waiting for a read
    std::vector<uint8_t> request; // the stub of a request arriving in fragments
};
