// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// File Sharing's share list on IPC$ (drivers/smb_pipe.h): a bind taken with
// NDR and other transfer syntaxes refused, srvsvc's NetrShareEnum and
// NetrShareGetInfo answered in NDR, replies cut to the computer's fragment
// size, and any other call a fault.
#include "drivers/smb_pipe.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
const uint8_t ndr[16] = {0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60};
const uint8_t ndr64[16] = {0x33, 0x05, 0x71, 0x71, 0xba, 0xbe, 0x37, 0x49, 0x83, 0x19, 0xb5, 0xdb, 0xef, 0x9c, 0xcc, 0x36};
const uint8_t srvsvc[16] = {0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01, 0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88};

struct Out {
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { while (b.size() % 2) b.push_back(0); u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { while (b.size() % 4) b.push_back(0); for (int s = 0; s < 32; s += 8) u8(uint8_t(v >> s)); }
    void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void str(const std::string& t) { u32(uint32_t(t.size() + 1)); u32(0); u32(uint32_t(t.size() + 1)); for (char c : t) u16(uint8_t(c)); u16(0); }
};
struct In {
    const Bytes* p; size_t at;
    const Bytes& b() const { return *p; }
    uint16_t u16() { at = (at + 1) & ~size_t(1); uint16_t v = uint16_t(b()[at] | b()[at + 1] << 8); at += 2; return v; }
    uint32_t u32() { at = (at + 3) & ~size_t(3); uint32_t v = uint32_t(b()[at] | b()[at + 1] << 8 | b()[at + 2] << 16 | uint32_t(b()[at + 3]) << 24); at += 4; return v; }
    std::string str() { u32(); assert(u32() == 0); uint32_t n = u32(); std::string t; for (uint32_t i = 0; i < n; ++i) { uint16_t c = u16(); if (c) t += char(c); } return t; }
};
Bytes pdu(uint8_t type, uint8_t flags, uint32_t call, const Bytes& body) {
    Out o;
    o.u8(5); o.u8(0); o.u8(type); o.u8(flags); o.u8(0x10); o.u8(0); o.u8(0); o.u8(0);
    o.u16(uint16_t(16 + body.size())); o.u16(0); o.u32(call);
    o.raw(body.data(), body.size());
    return o.b;
}
Bytes bind(uint16_t receives) {
    Out o;
    o.u16(4280); o.u16(receives); o.u32(0); o.u8(2); o.u8(0); o.u16(0);
    for (const uint8_t* syntax : {ndr, ndr64}) {
        o.u16(syntax == ndr ? 0 : 1); o.u8(1); o.u8(0);
        o.raw(srvsvc, 16); o.u16(3); o.u16(0);
        o.raw(syntax, 16); o.u32(syntax == ndr ? 2 : 1);
    }
    return pdu(11, 3, 1, o.b);
}
Bytes request(uint16_t opnum, const Bytes& stub, uint32_t call) {
    Out o;
    o.u32(uint32_t(stub.size())); o.u16(0); o.u16(opnum);
    o.raw(stub.data(), stub.size());
    return pdu(0, 3, call, o.b);
}
Bytes share_enum(uint32_t level) {
    Out o;
    o.u32(0x20000); o.str("\\\\192.168.155.1");
    o.u32(level); o.u32(level); o.u32(0x20004); o.u32(0); o.u32(0); // Empty container
    o.u32(0xffffffff);                                              // PreferedMaximumLength
    o.u32(0x20008); o.u32(0);                                       // ResumeHandle
    return o.b;
}
Bytes share_info(const std::string& name, uint32_t level) {
    Out o;
    o.u32(0); o.str(name); o.u32(level);
    return o.b;
}
// The stub of a reply made of one or more response fragments.
Bytes stub_of(const Bytes& reply, size_t& fragments) {
    Bytes stub; fragments = 0;
    for (size_t at = 0; at < reply.size();) {
        assert(reply[at + 2] == 2);
        size_t length = reply[at + 8] | reply[at + 9] << 8;
        stub.insert(stub.end(), reply.begin() + long(at + 24), reply.begin() + long(at + length));
        at += length; ++fragments;
    }
    return stub;
}
} // namespace

int main() {
    std::vector<SmbShareInfo> shares {{"SD Card", "", smb_share_disk}, {"IPC$", "IPC Service", smb_share_ipc}};
    SmbPipe pipe("\\srvsvc");
    // Bind: NDR taken, NDR64 refused, the pipe named back.
    auto ack = pipe.transact(bind(4280).data(), bind(4280).size(), shares);
    assert(ack.size() >= 24 && ack[2] == 12 && size_t(ack[8] | ack[9] << 8) == ack.size());
    In in {&ack, 16};
    assert(in.u16() == 4280 && in.u16() == 4280 && in.u32() != 0); // Fragment sizes, association group
    std::string port(reinterpret_cast<const char*>(ack.data()) + 26, size_t(ack[24] | ack[25] << 8) - 1);
    assert(port == "\\PIPE\\srvsvc");
    in.at = (26 + port.size() + 1 + 3) & ~size_t(3);
    assert(ack[in.at] == 2);
    in.at += 4;
    assert(in.u16() == 0 && in.u16() == 0 && std::memcmp(ack.data() + in.at, ndr, 16) == 0);
    in.at += 20;
    assert(in.u16() == 2 && in.u16() == 2);

    // NetrShareEnum level 1, as written then read (SMB2 WRITE, READ).
    auto enum1 = request(15, share_enum(1), 2);
    pipe.write(enum1.data(), enum1.size(), shares);
    auto reply = pipe.read();
    assert(pipe.read().empty());
    size_t fragments = 0;
    auto stub = stub_of(reply, fragments);
    assert(fragments == 1);
    In r {&stub, 0};
    assert(r.u32() == 1 && r.u32() == 1 && r.u32() != 0); // Level, switch, container
    assert(r.u32() == 2 && r.u32() != 0 && r.u32() == 2); // EntriesRead, buffer, array size
    for (int n = 0; n < 2; ++n) { assert(r.u32() != 0); r.u32(); assert(r.u32() != 0); }
    assert(r.str() == "SD Card" && r.str() == "" && r.str() == "IPC$" && r.str() == "IPC Service");
    assert(r.u32() == 2 && r.u32() != 0 && r.u32() == 0 && r.u32() == 0); // TotalEntries, resume handle, WERR_OK
    assert(r.at == stub.size());

    // Level 2 is not listed: ERROR_INVALID_LEVEL.
    auto enum2 = request(15, share_enum(2), 3);
    stub = stub_of(pipe.transact(enum2.data(), enum2.size(), shares), fragments);
    assert(stub.size() >= 4 && stub[stub.size() - 4] == 124);

    // NetrShareGetInfo: names match in any case; an unknown one is NERR_NetNameNotFound.
    auto info = request(16, share_info("sd card", 1), 4);
    stub = stub_of(pipe.transact(info.data(), info.size(), shares), fragments);
    r = In {&stub, 0};
    assert(r.u32() == 1 && r.u32() != 0 && r.u32() != 0 && r.u32() == smb_share_disk && r.u32() != 0);
    assert(r.str() == "SD Card");
    auto missing = request(16, share_info("Nope", 1), 5);
    stub = stub_of(pipe.transact(missing.data(), missing.size(), shares), fragments);
    assert(stub.size() == 12 && stub[8] == (2310 & 0xff) && stub[9] == (2310 >> 8));

    // Any other call: a fault, nca_s_op_rng_error.
    auto other = request(21, Bytes(8, 0), 6);
    auto fault = pipe.transact(other.data(), other.size(), shares);
    assert(fault.size() == 32 && fault[2] == 3 && fault[24] == 0x02 && fault[25] == 0x00 && fault[26] == 0x01 && fault[27] == 0x1c);

    // A computer that takes 1024-byte fragments gets a long list in several,
    // first and last marked; the request may come in two fragments too.
    std::vector<SmbShareInfo> many;
    for (int n = 0; n < 40; ++n) many.push_back({"Share " + std::to_string(n), "A remark long enough to fill fragments", smb_share_disk});
    SmbPipe small("srvsvc");
    auto b = bind(1024);
    small.transact(b.data(), b.size(), many);
    auto whole = request(15, share_enum(1), 7);
    Bytes first(whole.begin(), whole.begin() + 40), second(whole.begin(), whole.begin() + 24);
    second.insert(second.end(), whole.begin() + 40, whole.end());
    first[3] = 1; first[8] = 40; first[9] = 0;
    second[3] = 2; second[8] = uint8_t(second.size()); second[9] = uint8_t(second.size() >> 8);
    assert(small.transact(first.data(), first.size(), many).empty());
    reply = small.transact(second.data(), second.size(), many);
    stub = stub_of(reply, fragments);
    assert(fragments > 1 && (reply[3] & 1) && !(reply[3] & 2));
    for (size_t at = 0; at < reply.size(); at += size_t(reply[at + 8] | reply[at + 9] << 8)) assert(size_t(reply[at + 8] | reply[at + 9] << 8) <= 1024);
    r = In {&stub, 12};
    assert(r.u32() == 40);
    return 0;
}
