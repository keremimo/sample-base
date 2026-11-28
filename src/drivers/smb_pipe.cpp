// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "drivers/smb_pipe.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace {
// PDU types and flags (C706 12.6, MS-RPCE 2.2.2).
enum : uint8_t { Request = 0, Response = 2, Fault = 3, Bind = 11, BindAck = 12, AlterContext = 14, AlterContextResponse = 15 };
constexpr uint8_t first_fragment = 0x01, last_fragment = 0x02, object_uuid = 0x80;
constexpr size_t header_size = 16;
constexpr uint16_t largest_fragment = 4280;   // What Windows offers
constexpr uint32_t op_range_error = 0x1c010002;  // nca_s_op_rng_error: no such call here
constexpr uint32_t invalid_level = 124;          // ERROR_INVALID_LEVEL
constexpr uint32_t share_not_found = 2310;       // NERR_NetNameNotFound
// NDR 2.0's transfer syntax 8a885d04-1ceb-11c9-9fe8-08002b104860, as it travels.
constexpr uint8_t ndr_syntax[16] = {0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60};

// Little-endian NDR in: each value aligned to its size from the start.
struct Reader {
    const uint8_t* data;
    size_t size, at = 0;
    bool ok = true;
    void align(size_t n) { at = (at + n - 1) / n * n; }
    bool take(size_t n) { if (!ok || at > size || size - at < n) { ok = false; return false; } return true; }
    uint8_t u8() { if (!take(1)) return 0; return data[at++]; }
    uint16_t u16() { align(2); if (!take(2)) return 0; uint16_t v = uint16_t(data[at] | data[at + 1] << 8); at += 2; return v; }
    uint32_t u32() {
        align(4); if (!take(4)) return 0;
        uint32_t v = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
        at += 4; return v;
    }
    void skip(size_t n) { if (take(n)) at += n; }
    // A conformant varying UTF-16 string, as UTF-8 without its terminating NUL.
    std::string string() {
        u32(); u32(); uint32_t count = u32();
        std::string text;
        for (uint32_t n = 0; n < count && ok; ++n) {
            uint32_t c = u16();
            if (c >= 0xd800 && c < 0xdc00 && n + 1 < count) { uint32_t low = u16(); ++n; c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00); }
            if (c == 0) break;
            if (c < 0x80) text += char(c);
            else if (c < 0x800) { text += char(0xc0 | c >> 6); text += char(0x80 | (c & 0x3f)); }
            else if (c < 0x10000) { text += char(0xe0 | c >> 12); text += char(0x80 | (c >> 6 & 0x3f)); text += char(0x80 | (c & 0x3f)); }
            else { text += char(0xf0 | c >> 18); text += char(0x80 | (c >> 12 & 0x3f)); text += char(0x80 | (c >> 6 & 0x3f)); text += char(0x80 | (c & 0x3f)); }
        }
        return text;
    }
};

// Little-endian NDR out, aligned the same way. Pointers get referent ids
// counting up from 0x20000, as Windows numbers them.
struct Writer {
    std::vector<uint8_t> out;
    uint32_t next = 0x20000;
    void align(size_t n) { while (out.size() % n) out.push_back(0); }
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { align(2); out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
    void u32(uint32_t v) { align(4); for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(v >> shift)); }
    void bytes(const uint8_t* data, size_t size) { out.insert(out.end(), data, data + size); }
    uint32_t pointer() { uint32_t id = next; next += 4; u32(id); return id; }
    // A conformant varying UTF-16 string with its terminating NUL.
    void string(const std::string& text) {
        std::vector<uint16_t> units;
        for (size_t n = 0; n < text.size();) {
            uint8_t b = uint8_t(text[n]);
            uint32_t c = b, extra = 0;
            if (b >= 0xf0) { c = b & 0x07; extra = 3; } else if (b >= 0xe0) { c = b & 0x0f; extra = 2; } else if (b >= 0xc0) { c = b & 0x1f; extra = 1; }
            ++n;
            for (; extra && n < text.size(); --extra, ++n) c = c << 6 | (uint8_t(text[n]) & 0x3f);
            if (c >= 0x10000) { c -= 0x10000; units.push_back(uint16_t(0xd800 + (c >> 10))); units.push_back(uint16_t(0xdc00 + (c & 0x3ff))); }
            else units.push_back(uint16_t(c));
        }
        units.push_back(0);
        u32(uint32_t(units.size())); u32(0); u32(uint32_t(units.size()));
        for (auto unit : units) u16(unit);
    }
};

// A PDU's common header with its length left to finish(); `call_id` answers the request's.
void header(Writer& w, uint8_t type, uint8_t flags, uint32_t call_id) {
    w.u8(5); w.u8(0); w.u8(type); w.u8(flags);
    w.u8(0x10); w.u8(0); w.u8(0); w.u8(0); // Little-endian, ASCII, IEEE
    w.u16(0); w.u16(0);                    // Fragment length, no authentication
    w.u32(call_id);
}
void finish(Writer& w, size_t start) {
    size_t length = w.out.size() - start;
    w.out[start + 8] = uint8_t(length); w.out[start + 9] = uint8_t(length >> 8);
}

bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

// NetrShareEnum's [in, out] SHARE_ENUM_STRUCT and what follows it (levels 0, 1).
std::vector<uint8_t> share_enum(Reader in, const std::vector<SmbShareInfo>& shares) {
    if (in.u32()) in.string();             // ServerName
    uint32_t level = in.u32();
    in.u32();                               // The union's switch
    bool resume = false;
    if (in.u32()) {                         // The container the computer sent: empty
        in.u32();
        if (in.u32() == 0) { in.u32(); resume = in.u32() != 0; } // Buffer, PreferedMaximumLength, ResumeHandle
    }
    Writer w;
    uint32_t count = uint32_t(shares.size());
    w.u32(level); w.u32(level);
    bool known = level == 0 || level == 1;
    if (known) {
        w.pointer();                        // The container
        w.u32(count);
        if (count) w.pointer(); else w.u32(0);
        if (count) {
            w.u32(count);
            for (const auto& share : shares) {
                w.pointer();
                if (level == 1) { w.u32(share.type); w.pointer(); }
            }
            for (const auto& share : shares) {
                w.string(share.name);
                if (level == 1) w.string(share.remark);
            }
        }
    } else w.u32(0);
    w.u32(known ? count : 0);              // TotalEntries
    if (resume) { w.pointer(); w.u32(0); } else w.u32(0);
    w.u32(known ? 0 : invalid_level);
    return w.out;
}

// NetrShareGetInfo's [out] SHARE_INFO union and status (levels 0, 1, 1005).
std::vector<uint8_t> share_info(Reader in, const std::vector<SmbShareInfo>& shares) {
    if (in.u32()) in.string();             // ServerName
    std::string name = in.string();
    uint32_t level = in.u32();
    auto share = std::find_if(shares.begin(), shares.end(), [&](const SmbShareInfo& s) { return same_name(s.name, name); });
    Writer w;
    w.u32(level);
    if (share == shares.end()) { w.u32(0); w.u32(share_not_found); return w.out; }
    if (level == 0) { w.pointer(); w.pointer(); w.string(share->name); }
    else if (level == 1) { w.pointer(); w.pointer(); w.u32(share->type); w.pointer(); w.string(share->name); w.string(share->remark); }
    else if (level == 1005) { w.pointer(); w.u32(0); } // SHI1005_FLAGS: none (manual caching, no DFS)
    else { w.u32(0); w.u32(invalid_level); return w.out; }
    w.u32(0);
    return w.out;
}
} // namespace

SmbPipe::SmbPipe(std::string name) : interface(std::move(name)) {
    interface.erase(0, interface.find_first_not_of('\\'));
    srvsvc = same_name(interface, "srvsvc");
}

std::vector<uint8_t> SmbPipe::transact(const uint8_t* data, size_t size, const std::vector<SmbShareInfo>& shares) {
    std::vector<uint8_t> replies;
    while (size >= header_size) {
        size_t length = size_t(data[8] | data[9] << 8);
        if (length < header_size || length > size) break;
        auto reply = answer(data, length, shares);
        replies.insert(replies.end(), reply.begin(), reply.end());
        data += length; size -= length;
    }
    return replies;
}

void SmbPipe::write(const uint8_t* data, size_t size, const std::vector<SmbShareInfo>& shares) {
    auto reply = transact(data, size, shares);
    pending.insert(pending.end(), reply.begin(), reply.end());
}

std::vector<uint8_t> SmbPipe::read() { return std::exchange(pending, {}); }

std::vector<uint8_t> SmbPipe::answer(const uint8_t* pdu, size_t size, const std::vector<SmbShareInfo>& shares) {
    if (pdu[0] != 5 || (pdu[4] & 0xf0) != 0x10) return {}; // Version 5, little-endian only
    uint8_t type = pdu[2], flags = pdu[3];
    if (type == Bind || type == AlterContext) return bind_ack(pdu, size, type);
    if (type != Request) return {};
    Reader r {pdu, size};
    r.at = 12;
    uint32_t call_id = r.u32();
    r.u32();                                // Allocation hint
    uint16_t context = r.u16(), opnum = r.u16();
    if (flags & object_uuid) r.skip(16);
    size_t auth = size_t(pdu[10] | pdu[11] << 8);
    size_t end = auth ? size - std::min(size, auth + 8) : size;
    if (!r.ok || end < r.at) return {};
    if (flags & first_fragment) request.clear();
    request.insert(request.end(), pdu + r.at, pdu + end);
    if (!(flags & last_fragment)) return {};
    bool known = false;
    auto stub = call(opnum, request, shares, known);
    request.clear();
    Writer w;
    if (!known) {
        header(w, Fault, first_fragment | last_fragment, call_id);
        w.u32(0); w.u16(context); w.u8(0); w.u8(0);
        w.u32(op_range_error); w.u32(0);
        finish(w, 0);
        return w.out;
    }
    // Fragments no longer than the computer takes, each with what is left.
    size_t most = size_t(max_xmit - 24) & ~size_t(7), done = 0;
    do {
        size_t start = w.out.size(), part = std::min(most, stub.size() - done);
        header(w, Response, uint8_t((done == 0 ? first_fragment : 0) | (done + part == stub.size() ? last_fragment : 0)), call_id);
        w.u32(uint32_t(stub.size() - done)); w.u16(context); w.u8(0); w.u8(0);
        w.bytes(stub.data() + done, part);
        finish(w, start);
        done += part;
    } while (done < stub.size());
    return w.out;
}

std::vector<uint8_t> SmbPipe::bind_ack(const uint8_t* pdu, size_t size, uint8_t type) {
    Reader r {pdu, size};
    r.at = 12;
    uint32_t call_id = r.u32();
    r.u16();                                // The computer's largest fragment out
    uint16_t receives = r.u16();            // ... and in
    uint32_t group = r.u32();
    uint8_t contexts = r.u8();
    r.skip(3);
    if (receives >= 1024) max_xmit = std::min(receives, largest_fragment);
    if (group) assoc_group = group;
    else if (!assoc_group) assoc_group = 0x53f0;
    struct Result { uint16_t result, reason; bool ndr; };
    std::vector<Result> results;
    for (uint8_t n = 0; n < contexts && r.ok; ++n) {
        r.u16();                            // Context id: results go in the same order
        uint8_t syntaxes = r.u8();
        r.skip(1); r.skip(20);              // The interface asked for: any is taken
        bool ndr = false;
        for (uint8_t s = 0; s < syntaxes && r.take(20); ++s) {
            ndr = ndr || (std::memcmp(pdu + r.at, ndr_syntax, 16) == 0 && pdu[r.at + 16] == 2);
            r.skip(20);
        }
        // Taken with NDR, else refused: proposed transfer syntaxes not supported.
        results.push_back(ndr ? Result {0, 0, true} : Result {2, 2, false});
    }
    if (!r.ok) return {};
    Writer w;
    header(w, type == Bind ? BindAck : AlterContextResponse, first_fragment | last_fragment, call_id);
    w.u16(max_xmit); w.u16(largest_fragment); w.u32(assoc_group);
    if (type == Bind) {
        std::string port = "\\PIPE\\" + interface;
        w.u16(uint16_t(port.size() + 1));
        w.bytes(reinterpret_cast<const uint8_t*>(port.c_str()), port.size() + 1);
    } else w.u16(0);
    w.align(4);
    w.u8(uint8_t(results.size())); w.u8(0); w.u16(0);
    for (const auto& result : results) {
        w.u16(result.result); w.u16(result.reason);
        if (result.ndr) { w.bytes(ndr_syntax, 16); w.u32(2); }
        else { for (int n = 0; n < 20; ++n) w.u8(0); }
    }
    finish(w, 0);
    return w.out;
}

std::vector<uint8_t> SmbPipe::call(uint16_t opnum, const std::vector<uint8_t>& stub, const std::vector<SmbShareInfo>& shares, bool& known) {
    known = srvsvc && (opnum == 15 || opnum == 16);
    if (!known) return {};
    Reader in {stub.data(), stub.size()};
    return opnum == 15 ? share_enum(in, shares) : share_info(in, shares);
}
