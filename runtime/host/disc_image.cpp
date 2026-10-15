// Disc image readers (see disc_image.h).
//
// RVZ layout (as documented by the Dolphin project, docs/WiaAndRvz.md): header 1 (0x48 bytes), header 2 (disc
// type, compression, chunk size, the first 0x80 bytes of the disc, and the locations of the compressed
// raw-data and group tables).  A GameCube disc has no partitions: raw-data entries cover the disc, each split
// into chunk-sized groups.  A group is stored zeroed (size 0), plain, or zstd-compressed (bit 31 of its size);
// with a non-zero packed size its data is "RVZ packed": runs of literal bytes and of junk (the disc's padding,
// regenerated from a 68-byte Lagged Fibonacci seed).
#include "disc_image.h"
#include <windows.h>
#include <mutex>
#include <vector>
#define ZSTD_STATIC_LINKING_ONLY
#include "../../third_party/zstd/lib/zstd.h"

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 be64(const u8* p) { return ((u64)be32(p) << 32) | be32(p + 4); }

static HANDLE open_file(const std::string& utf8_path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, &w[0], n);
    return CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
}

// reads up to len bytes at off; returns the count read
static u32 read_file(HANDLE f, u64 off, void* dst, u32 len) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(f, dst, len, &got, &ov)) return 0;
    return got;
}

// ---------------------------------------------------------------- ISO / GCM
struct IsoImage : DiscImage {
    HANDLE f;
    explicit IsoImage(HANDLE h) : f(h) {
        LARGE_INTEGER sz;
        GetFileSizeEx(f, &sz);
        size = (u64)sz.QuadPart;
    }
    ~IsoImage() override { CloseHandle(f); }
    bool read(u64 off, void* dst, u32 len) override {
        u32 got = read_file(f, off, dst, len);
        if (got < len) memset((u8*)dst + got, 0, len - got);
        return true;
    }
};

// ---------------------------------------------------------------- RVZ
// The junk generator (Dolphin's LaggedFibonacciGenerator): 521 words, lags 521/32, seeded with 17 words.
class Lfg {
    static const u32 K = 521, J = 32, SEED = 17;
    u32 buf[K];
    u32 pos = 0;   // byte position in buf
    void forward() {
        for (u32 i = 0; i < J; i++) buf[i] ^= buf[i + K - J];
        for (u32 i = J; i < K; i++) buf[i] ^= buf[i - J];
    }
public:
    void seed(const u8* s) {
        pos = 0;
        for (u32 i = 0; i < SEED; i++) buf[i] = be32(s + 4 * i);
        for (u32 i = SEED; i < K; i++) buf[i] = (buf[i - 17] << 23) ^ (buf[i - 16] >> 9) ^ buf[i - 1];
        // the output takes bits 18-25 where a plain shift would take 16-23; folded in here, stored big-endian
        for (u32& x : buf) x = _byteswap_ulong((x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000));
        for (int i = 0; i < 4; i++) forward();
    }
    void skip(u64 n) {
        pos += (u32)(n % (K * 4));
        n /= K * 4;
        while (n--) forward();
        while (pos >= K * 4) { forward(); pos -= K * 4; }
    }
    void get(u8* out, u64 n) {
        while (n) {
            u32 take = (u32)std::min<u64>(n, K * 4 - pos);
            memcpy(out, (const u8*)buf + pos, take);
            out += take; n -= take; pos += take;
            if (pos == K * 4) { forward(); pos = 0; }
        }
    }
};

struct RvzImage : DiscImage {
    HANDLE f;
    u32 compression = 0, chunk = 0;
    u8 disc_header[0x80];
    struct Raw { u64 start, end; u32 group, ngroups; };   // start rounded down to 0x8000
    struct Group { u64 file_off; u32 stored; bool compressed; u32 packed; };
    std::vector<Raw> raws;
    std::vector<Group> groups;
    ZSTD_DCtx* dctx = nullptr;
    std::mutex mtx;
    struct Cached { u32 index = ~0u; u64 used = 0; std::vector<u8> data; };
    Cached cache[16];
    u64 tick = 0;
    std::vector<u8> in, unpacked;

    explicit RvzImage(HANDLE h) : f(h) {}
    ~RvzImage() override {
        if (dctx) ZSTD_freeDCtx(dctx);
        CloseHandle(f);
    }

    // a table: compressed with the image's method, `stored` bytes at `off`, `out_len` bytes once decompressed
    bool read_table(u64 off, u32 stored, u32 out_len, std::vector<u8>& out) {
        std::vector<u8> raw(stored);
        if (read_file(f, off, raw.data(), stored) != stored) return false;
        if (compression == 0) {
            if (stored < out_len) return false;
            out.assign(raw.begin(), raw.begin() + out_len);
            return true;
        }
        out.resize(out_len);
        size_t n = ZSTD_decompressDCtx(dctx, out.data(), out_len, raw.data(), stored);
        return !ZSTD_isError(n) && n == out_len;
    }

    std::string init() {
        u8 h1[0x48];
        if (read_file(f, 0, h1, sizeof h1) != sizeof h1) return "The RVZ file is truncated.";
        u32 h2_size = be32(h1 + 0x0C);
        size = be64(h1 + 0x24);
        std::vector<u8> h2(std::max<u32>(h2_size, 0xDC));
        if (h2_size < 0xD5 || read_file(f, 0x48, h2.data(), h2_size) != h2_size) return "The RVZ file is truncated.";
        if (be32(&h2[0]) != 1) return "This RVZ file holds a Wii disc, not a GameCube disc.";
        compression = be32(&h2[4]);
        chunk = be32(&h2[0x0C]);
        if (compression != 0 && compression != 5)
            return "This RVZ file uses bzip2 or LZMA compression; only Zstandard (Dolphin's default) is supported. "
                   "Convert it again in Dolphin with Compression: Zstandard, or to a plain ISO.";
        if (chunk < 0x8000 || chunk > 0x4000000) return "The RVZ file has an unsupported chunk size.";
        memcpy(disc_header, &h2[0x10], sizeof disc_header);
        dctx = ZSTD_createDCtx();
        u32 nraw = be32(&h2[0xB4]), ngroup = be32(&h2[0xC4]);
        std::vector<u8> t;
        if (nraw > 0x10000 || ngroup > 0x1000000 ||
            !read_table(be64(&h2[0xB8]), be32(&h2[0xC0]), nraw * 0x18, t))
            return "The RVZ file is damaged (raw data table).";
        for (u32 i = 0; i < nraw; i++) {
            const u8* e = &t[i * 0x18];
            u64 off = be64(e), sz = be64(e + 8);
            raws.push_back({off - off % 0x8000, off + sz, be32(e + 0x10), be32(e + 0x14)});
        }
        if (!read_table(be64(&h2[0xC8]), be32(&h2[0xD0]), ngroup * 0x0C, t)) return "The RVZ file is damaged (group table).";
        for (u32 i = 0; i < ngroup; i++) {
            const u8* e = &t[i * 0x0C];
            u32 sz = be32(e + 4);
            groups.push_back({(u64)be32(e) << 2, sz & 0x7FFFFFFF, (sz >> 31) != 0, be32(e + 8)});
        }
        for (auto& r : raws)
            if ((u64)r.group + r.ngroups > groups.size() || (r.end - r.start + chunk - 1) / chunk > r.ngroups)
                return "The RVZ file is damaged (group index).";
        return "";
    }

    // decoded bytes of group `gi`, which holds `len` disc bytes starting at disc offset `disc_off`
    const std::vector<u8>* group_data(u32 gi, u64 disc_off, u32 len) {
        Cached* slot = &cache[0];
        for (auto& c : cache) {
            if (c.index == gi) { c.used = ++tick; return &c.data; }
            if (c.used < slot->used) slot = &c;
        }
        const Group& g = groups[gi];
        std::vector<u8>& out = slot->data;
        slot->index = ~0u;
        out.assign(len, 0);
        if (g.stored) {
            in.resize(g.stored);
            if (read_file(f, g.file_off, in.data(), g.stored) != g.stored) return nullptr;
            const std::vector<u8>* src = &in;
            if (g.compressed) {
                u32 n = g.packed ? g.packed : len;
                unpacked.resize(n);
                size_t got = ZSTD_decompressDCtx(dctx, unpacked.data(), n, in.data(), in.size());
                if (ZSTD_isError(got)) { LOG("disc", "RVZ group %u: %s", gi, ZSTD_getErrorName(got)); return nullptr; }
                unpacked.resize(got);
                src = &unpacked;
            }
            if (!g.packed) memcpy(out.data(), src->data(), std::min<size_t>(len, src->size()));
            else {
                // runs: u32 size (bit 31: junk, followed by its 68-byte seed) then the literal bytes
                const u8* p = src->data();
                const u8* end = p + src->size();
                u64 o = 0;
                Lfg lfg;
                while (o < len && p + 4 <= end) {
                    u32 hdr = be32(p);
                    p += 4;
                    u32 n = (u32)std::min<u64>(hdr & 0x7FFFFFFF, len - o);
                    if (hdr >> 31) {
                        if (p + 68 > end) break;
                        lfg.seed(p);
                        p += 68;
                        lfg.skip((disc_off + o) % 0x8000);
                        lfg.get(out.data() + o, n);
                    } else {
                        n = (u32)std::min<u64>(n, end - p);
                        memcpy(out.data() + o, p, n);
                        p += hdr & 0x7FFFFFFF;
                    }
                    o += n;
                }
            }
        }
        slot->index = gi;
        slot->used = ++tick;
        return &out;
    }

    bool read(u64 off, void* dst, u32 len) override {
        std::lock_guard<std::mutex> lock(mtx);
        u8* d = (u8*)dst;
        memset(d, 0, len);
        bool ok = true;
        for (const Raw& r : raws) {
            u64 a = std::max(off, r.start), b = std::min(off + len, r.end);
            while (a < b) {
                u64 i = (a - r.start) / chunk, gstart = r.start + i * chunk;
                u32 glen = (u32)std::min<u64>(chunk, r.end - gstart);
                const std::vector<u8>* g = group_data(r.group + (u32)i, gstart, glen);
                u64 n = std::min(b, gstart + glen) - a;
                if (g) memcpy(d + (a - off), g->data() + (a - gstart), n);
                else ok = false;
                a += n;
            }
        }
        if (off < sizeof disc_header) memcpy(d, disc_header + off, (size_t)std::min<u64>(len, sizeof disc_header - off));
        return ok;
    }
};

DiscImage* disc_image_open(const std::string& utf8_path, std::string* why) {
    HANDLE f = open_file(utf8_path);
    if (f == INVALID_HANDLE_VALUE) { *why = "The file could not be opened."; return nullptr; }
    u8 magic[4] = {};
    read_file(f, 0, magic, 4);
    if (!memcmp(magic, "RVZ\x01", 4)) {
        RvzImage* img = new RvzImage(f);
        *why = img->init();
        if (why->empty()) return img;
        delete img;
        return nullptr;
    }
    if (!memcmp(magic, "WIA\x01", 4) || !memcmp(magic, "CISO", 4) || be32(magic) == 0x01C00BB1 /* GCZ */) {
        CloseHandle(f);
        *why = "This is a compressed disc image in a format the port cannot read (WIA / GCZ / CISO). Convert it in "
               "Dolphin (right-click the game, Convert File...) to RVZ or ISO.";
        return nullptr;
    }
    return new IsoImage(f);
}
