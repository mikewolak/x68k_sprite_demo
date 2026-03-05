////////////////////////////////////////////////////////////////////////////////
// x68k_video.h — X68000 video and hardware helper declarations
////////////////////////////////////////////////////////////////////////////////
#ifndef X68K_VIDEO_H
#define X68K_VIDEO_H

#include "types.h"

void init_video(int mode);
void wait_vblank(void);
void gvram_fill_dma(uint32_t value);
void init_hblank(void);

#endif // X68K_VIDEO_H
