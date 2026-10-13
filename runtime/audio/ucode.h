// Interface for high-level emulated DSP microcodes.
#pragma once
#include "../common.h"

struct UcodeHle {
    virtual ~UcodeHle() {}
    virtual void mail(u32 m) = 0;
};

UcodeHle* make_zelda_ucode(u32 hash);       // zelda_ucode.cpp (nullptr if not a Zelda-family ucode)

void dsp_send_mail_to_cpu(u32 mail, bool irq);
u8* aram_ptr(u32 a);
