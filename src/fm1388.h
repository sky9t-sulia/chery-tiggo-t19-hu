/* FM1388 voice processor (hands-free microphone): modes in /lib/firmware/FM1388_mode.cfg,
 * through the stock libfm1388.so the same way as ShellApp (FM1388::init / setMode, Ghidra analysis 2026-10-22):
 * call - 6 (BlueTooth8K), otherwise - 8 (BargeIn). Without the library (PC) - does nothing. */
#ifndef FM1388_H
#define FM1388_H

enum { FM1388_CM16K = 1, FM1388_VR16K = 2, FM1388_SIRI = 7, FM1388_BT_CALL = 6, FM1388_DEFAULT = 8 };   /* 1 - CarPlay hands-free 16 kHz (echo cancellation), 7 - Siri */
void fm1388_set_mode(int mode);

#endif
