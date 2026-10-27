/* Rear-view camera: /dev/video0 (mxc_v4l2, ADV7182), YUYV 720x576 interlaced, the frame is a GPU texture without copying.
 * Set up like the stock cameradetect (Ghidra analysis 2026-10-22): ADV7182 reset over I2C, input 1, standard as is,
 * S_PARM, YUYV, 3 mmap buffers; shows the top CAM_ROWS lines. The stock cameradetect holds the device -
 * we stop it before opening, and start it back after closing / on failure (the camera on R will not be lost). */
#ifndef CAM_H
#define CAM_H

#include "gfx.h"

#define CAM_ROWS 480                    /* how many frame lines cameradetect shows */

int  cam_open(void);                    /* 0 - capture is running */
void cam_close(void);                   /* stop */
void cam_stop(int stock);               /* stop; stock - start the stock one (0 - we will reopen right away) */
int  cam_active(void);
gfx_tex *cam_frame(int *w, int *h);     /* last frame (texture) or NULL (none / empty); w, h - frame size */
double cam_last_frame(void);            /* time of the last non-empty frame (0 - none yet) */
int  cam_reverse(double now);           /* reverse (GPIO 84, also watched by cameradetect) */

#endif
