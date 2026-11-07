#include "update_package.hpp"
#include "update_core.hpp"
#include <algorithm>
#include <cstring>
#include <set>
#include <zlib.h>
namespace pspapp {

// ---- SHA-256 ----
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
    std::memset(buf_, 0, sizeof buf_);
}
void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}
void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += n;
    while (n) {
        size_t take = std::min(n, sizeof buf_ - fill_);
        std::memcpy(buf_ + fill_, p, take);
        fill_ += take; p += take; n -= take;
        if (fill_ == 64) { block(buf_); fill_ = 0; }
    }
}
std::string Sha256::finishHex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (fill_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
        for (int s = 28; s >= 0; s -= 4) out += hex[(v >> s) & 15];
    return out;
}
std::string sha256Hex(ByteSource& src) {
    Sha256 h;
    std::vector<uint8_t> buf(16384);
    for (uint64_t off = 0; off < src.size();) {
        size_t want = (size_t)std::min<uint64_t>(buf.size(), src.size() - off);
        size_t got = src.read(off, buf.data(), want);
        if (got != want) return {}; // short read: never return a digest of partial data
        h.update(buf.data(), got);
        off += got;
    }
    return h.finishHex();
}

// ---- ZIP ----
static uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

ZipError listZip(ByteSource& src, std::vector<ZipEntry>& out, const ZipLimits& lim) {
    const uint64_t total = src.size();
    if (total < 22) return ZipError::Truncated;
    const size_t tailLen = (size_t)std::min<uint64_t>(total, 22 + 65535);
    std::vector<uint8_t> tail(tailLen);
    if (src.read(total - tailLen, tail.data(), tailLen) != tailLen) return ZipError::Truncated;
    size_t eocd = std::string::npos;
    for (size_t i = tailLen - 22 + 1; i-- > 0;)
        if (u32(&tail[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return ZipError::Truncated;
    const uint8_t* e = &tail[eocd];
    if (u16(e + 4) != 0 || u16(e + 6) != 0) return ZipError::Unsupported; // multi-disk
    uint32_t count = u16(e + 10), cdSize = u32(e + 12), cdOff = u32(e + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return ZipError::Unsupported; // zip64
    if (count != u16(e + 8) || count > lim.maxEntries) return ZipError::TooLarge;
    if ((uint64_t)cdOff + cdSize > total || cdSize > (8u << 20)) return ZipError::Corrupt;
    std::vector<uint8_t> cd(cdSize);
    if (cdSize && src.read(cdOff, cd.data(), cdSize) != cdSize) return ZipError::Truncated;
    std::set<std::string> seen;
    uint64_t sum = 0;
    size_t pos = 0;
    out.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 46 > cd.size() || u32(&cd[pos]) != 0x02014b50) return ZipError::Corrupt;
        const uint8_t* h = &cd[pos];
        uint16_t flags = u16(h + 8), method = u16(h + 10), nlen = u16(h + 28), xlen = u16(h + 30), clen = u16(h + 32);
        if (pos + 46 + nlen + xlen + clen > cd.size()) return ZipError::Corrupt;
        ZipEntry z;
        z.path.assign((const char*)h + 46, nlen);
        z.crc = u32(h + 16);
        z.compressedSize = u32(h + 20);
        z.size = u32(h + 24);
        z.localOffset = u32(h + 42);
        z.method = method;
        pos += 46 + nlen + xlen + clen;
        if (flags & 1) return ZipError::Unsupported; // encrypted
        if (z.compressedSize == 0xFFFFFFFFu || z.size == 0xFFFFFFFFu || z.localOffset == 0xFFFFFFFFu) return ZipError::Unsupported;
        z.isDir = !z.path.empty() && z.path.back() == '/';
        std::string check = z.isDir ? z.path.substr(0, z.path.size() - 1) : z.path;
        if (!isSafeArchivePath(check)) return ZipError::BadPath;
        if (!seen.insert(z.path).second) return ZipError::BadPath;
        if (z.isDir) { out.push_back(std::move(z)); continue; }
        if (method != 0 && method != 8) return ZipError::Unsupported;
        if (method == 0 && z.compressedSize != z.size) return ZipError::Corrupt;
        if (z.size > lim.maxFileSize) return ZipError::TooLarge;
        sum += z.size;
        if (sum > lim.maxTotalSize) return ZipError::TooLarge;
        if (z.localOffset + 30 + z.compressedSize > total) return ZipError::Truncated;
        out.push_back(std::move(z));
    }
    return ZipError::Ok;
}

ZipError extractEntry(ByteSource& src, const ZipEntry& e, FileSink& sink) {
    uint8_t lh[30];
    if (src.read(e.localOffset, lh, 30) != 30) return ZipError::Truncated;
    if (u32(lh) != 0x04034b50) return ZipError::Corrupt;
    uint64_t dataOff = e.localOffset + 30 + u16(lh + 26) + u16(lh + 28);
    if (dataOff + e.compressedSize > src.size()) return ZipError::Truncated;
    if (!sink.begin(e.path, e.size)) return ZipError::Sink;
    ZipError err = ZipError::Ok;
    z_stream zs;
    std::memset(&zs, 0, sizeof zs);
    bool inflating = e.method == 8;
    if (inflating && inflateInit2(&zs, -15) != Z_OK) { sink.end(false); return ZipError::Corrupt; }
    std::vector<uint8_t> in(16384), outb(16384);
    uint64_t consumed = 0, produced = 0;
    uLong crc = crc32(0, Z_NULL, 0);
    bool streamEnd = !inflating;
    while (err == ZipError::Ok && (consumed < e.compressedSize || !streamEnd)) {
        size_t want = (size_t)std::min<uint64_t>(in.size(), e.compressedSize - consumed);
        if (want == 0 && inflating && zs.avail_in == 0) { err = ZipError::Truncated; break; }
        if (want && src.read(dataOff + consumed, in.data(), want) != want) { err = ZipError::Truncated; break; }
        consumed += want;
        auto emit = [&](const uint8_t* p, size_t n) {
            produced += n;
            if (produced > e.size) { err = ZipError::Corrupt; return; }
            crc = crc32(crc, p, (uInt)n);
            if (!sink.write(p, n)) err = ZipError::Sink;
        };
        if (!inflating) { emit(in.data(), want); continue; }
        zs.next_in = in.data();
        zs.avail_in = (uInt)want;
        do {
            zs.next_out = outb.data();
            zs.avail_out = (uInt)outb.size();
            int r = inflate(&zs, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) { err = ZipError::Corrupt; break; }
            emit(outb.data(), outb.size() - zs.avail_out);
            if (err != ZipError::Ok) break;
            if (r == Z_STREAM_END) { streamEnd = true; break; }
            if (zs.avail_in == 0 && zs.avail_out != 0) break;
        } while (true);
    }
    if (inflating) inflateEnd(&zs);
    if (err == ZipError::Ok && (produced != e.size || (uint32_t)crc != e.crc)) err = ZipError::BadCrc;
    if (!sink.end(err == ZipError::Ok) && err == ZipError::Ok) err = ZipError::Sink;
    return err;
}

// ---- Resume ----
std::string rangeHeader(uint64_t have) { return "bytes=" + std::to_string(have) + "-"; }

ResumeAction resumeAction(int status, std::string_view cr, uint64_t have, uint64_t expectedTotal) {
    if (status == 200) return ResumeAction::Restart; // server ignored Range: start over
    if (status == 416) return ResumeAction::Restart; // .part longer than file or stale
    if (status != 206) return ResumeAction::Fail;
    // "bytes <start>-<end>/<total>"
    if (cr.substr(0, 6) != "bytes ") return ResumeAction::Restart;
    cr.remove_prefix(6);
    auto num = [&](uint64_t& v) {
        size_t i = 0; v = 0;
        while (i < cr.size() && cr[i] >= '0' && cr[i] <= '9') { if (v > (1ull << 50)) return false; v = v * 10 + (cr[i++] - '0'); }
        cr.remove_prefix(i);
        return i > 0;
    };
    uint64_t start, end, total;
    if (!num(start) || cr.empty() || cr[0] != '-') return ResumeAction::Restart;
    cr.remove_prefix(1);
    if (!num(end) || cr.empty() || cr[0] != '/') return ResumeAction::Restart;
    cr.remove_prefix(1);
    if (!num(total)) return ResumeAction::Restart;
    if (start != have || end < start || end >= total) return ResumeAction::Restart;
    if (expectedTotal && total != expectedTotal) return ResumeAction::Restart;
    return ResumeAction::Append;
}
}
