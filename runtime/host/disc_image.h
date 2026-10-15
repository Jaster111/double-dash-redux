// Disc image readers: plain ISO / GCM, and RVZ (Dolphin's compressed format; GameCube discs, zstd or no
// compression).  Reads are thread-safe; bytes past the end of the disc read as zeros.
#pragma once
#include "../common.h"
#include <string>

struct DiscImage {
    virtual ~DiscImage() {}
    virtual bool read(u64 off, void* dst, u32 len) = 0;
    u64 size = 0;   // size of the (uncompressed) disc
};

// nullptr with the reason (for the user) in *why when the file cannot be read as a disc image
DiscImage* disc_image_open(const std::string& utf8_path, std::string* why);
