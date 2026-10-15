zstd 1.5.6 (https://github.com/facebook/zstd, release tarball sha256
8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1), BSD license (LICENSE).
Only lib/common and lib/decompress are kept: the runtime decompresses RVZ disc images
(runtime/host/disc_image.cpp).  Built with ZSTD_DISABLE_ASM (no .S files under MSVC).
