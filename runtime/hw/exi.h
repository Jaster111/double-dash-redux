// EXI device interface shared by exi.cpp (channels, IPL/SRAM) and memcard.cpp.
#pragma once
#include "../common.h"

struct ExiDevice {
    virtual ~ExiDevice() {}
    virtual void select() {}
    virtual void deselect() {}
    // full-duplex byte exchange; write = the byte comes from the CPU (immediate write / read-write)
    virtual u8 transfer(u8 in, bool write) = 0;
    virtual bool present() { return true; }
    virtual void dma_read(u8* dst, u32 len) { for (u32 i = 0; i < len; i++) dst[i] = transfer(0, false); }
    virtual void dma_write(const u8* src, u32 len) { for (u32 i = 0; i < len; i++) transfer(src[i], true); }
};

ExiDevice* memcard_device(int slot);    // memcard.cpp (nullptr = no card in the slot)
void exi_raise_exiint(int chn);         // device interrupt line (memory card: command done)

// SRAM-ex fields kept by the IPL chip for memory cards
void sram_set_flash_id(int slot, const u8 id[12]);
