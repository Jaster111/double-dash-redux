// Extract main.dol from a disc image (ISO / GCM / RVZ) for the recompiler, using the runtime's disc reader.
//   dolextract <disc image> <out.dol>
// Accepts only the DOL the port is built from (Mario Kart: Double Dash!! Europe, GM4P01; same check as
// runtime/host/game_disc.cpp).  Exit code 0 = written, 1 = usage / I/O error, 2 = not the right image.
#include "host/disc_image.h"
#include <string.h>
#include <vector>

static const u32 DOL_SIZE = 3613184, DOL_CRC32 = 0x3B7E37E3;

void rt_log(const char* cat, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", cat);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static u32 crc32(const u8* p, size_t n) {
    u32 c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: dolextract <disc image> <out.dol>\n");
        return 1;
    }
    std::string why;
    DiscImage* img = disc_image_open(argv[1], &why);
    if (!img) {
        fprintf(stderr, "%s: %s\n", argv[1], why.c_str());
        return 2;
    }
    u8 h[0x440];
    if (img->size < sizeof h || !img->read(0, h, sizeof h) || be32(h + 0x1C) != 0xC2339F3D) {
        fprintf(stderr, "%s: not a GameCube disc image\n", argv[1]);
        return 2;
    }
    if (memcmp(h, "GM4P01", 6)) {
        fprintf(stderr, "%s: game ID %.6s, this port needs Mario Kart: Double Dash!! Europe (GM4P01)\n", argv[1], h);
        return 2;
    }
    u32 off = be32(h + 0x420);
    std::vector<u8> dol(DOL_SIZE);
    if (!img->read(off, dol.data(), DOL_SIZE) || crc32(dol.data(), DOL_SIZE) != DOL_CRC32) {
        fprintf(stderr, "%s: main.dol is not the one this port is built from (other revision or modified image)\n",
                argv[1]);
        return 2;
    }
    delete img;
    FILE* f = fopen(argv[2], "wb");
    if (!f || fwrite(dol.data(), 1, DOL_SIZE, f) != DOL_SIZE || fclose(f)) {
        fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    printf("%s: main.dol (%u bytes, CRC32 %08X) -> %s\n", argv[1], DOL_SIZE, DOL_CRC32, argv[2]);
    return 0;
}
