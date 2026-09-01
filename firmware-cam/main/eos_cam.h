// eos_cam — the camera half of a penguinOS camera node.
//
// This node has no screen. Its whole job is to hand a frame to a board that
// does, in the one format that board can afford to receive.
//
// WHY RAW RGB565 AND NOT JPEG. The consuming board in this fleet is a Cheap
// Yellow Display: 240x320, and about 30,000 bytes of free heap once Wi-Fi, BLE
// and the web server are up, with a largest block near 29,000. A JPEG decoder's
// working memory alone would not fit beside that, let alone the decoded frame.
// This node has 8MB of PSRAM. So the work happens HERE - capture, rotate,
// scale - and what crosses the wire is exactly the pixels the panel will show.
//
// The consumer never holds a whole frame either. 240x320 RGB565 is 153,600
// bytes; it asks for one horizontal strip at a time and blits each as it
// arrives, which is 7,680 bytes for a 16-row strip.

#ifndef EOS_CAM_H
#define EOS_CAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Largest frame this node will scale to. Bounded so a malformed request cannot
// ask for a gigabyte: w*h*2 must stay inside what PSRAM can spare beside the
// camera's own frame buffers.
#define EOS_CAM_MAX_W 640
#define EOS_CAM_MAX_H 640

typedef struct {
    bool     ready;          // the sensor answered and is streaming
    uint16_t src_w, src_h;   // what the sensor is capturing at
    uint32_t frames;         // frames served since boot
    uint32_t errors;         // captures that failed
    char     sensor[16];     // MEASURED "OV3660" on this unit, despite the
                             // retail listing saying OV2640
} eos_cam_info_t;

// Bring the sensor up. Returns 0 on success, negative on failure - and the
// failure modes are worth telling apart, so they are distinct:
//   -1  no PSRAM: the frame buffers have nowhere to live
//   -2  esp_camera_init failed: usually a pin wrong, or the ribbon not seated
//   -3  the sensor initialised but reports an id we do not know
int  eos_cam_init(void);

void eos_cam_info(eos_cam_info_t *out);

// Capture a frame, rotate and scale it to w by h, and KEEP it. Strips are then
// served out of that held frame by eos_cam_strip().
//
// This exists because of the consumer's memory, not this node's. A board with a
// 29KB largest free block cannot hold a 153,600-byte frame, so it asks for one
// 16-row strip at a time and blits each as it arrives - twenty requests for a
// full picture. If each of those captured afresh, every strip would come from a
// different moment and the assembled picture would tear across every band.
// Holding one frame and slicing it makes the twenty strips agree.
int  eos_cam_hold(int w, int h, int rotate);

// Copy `rows` rows starting at row `y` out of the held frame. Returns the byte
// count written, or negative if there is no held frame or the range is outside
// it.
int  eos_cam_strip(uint16_t *dst, int y, int rows, size_t cap);

// Capture, then rotate and scale into `dst` as RGB565, `w` by `h`.
//
// rotate is 0, 90, 180 or 270 and is applied BEFORE scaling, because the
// sensor is landscape and most of these panels are portrait - asking for
// 240x320 from a 320x240 sensor without a rotation gives a correctly sized
// picture of a squashed world.
//
// Returns 0 on success. dst must hold w*h*2 bytes.
int  eos_cam_frame_rgb565(uint16_t *dst, int w, int h, int rotate);

// The same frame as JPEG, for a browser. Caller frees with eos_cam_jpeg_free.
// Browsers decode JPEG for nothing, so the web app gets the cheap path and the
// screen boards get the raw one.
int  eos_cam_jpeg(uint8_t **out, size_t *len, int quality);

// The sensor's own test pattern, and the single most useful diagnostic on this
// board. The camera sits on a separate module behind a flex connector, and a
// ribbon that is seated but NOT LATCHED still makes SCCB contact - so
// esp_camera_init() succeeds, the sensor answers its ID, and every frame comes
// back black. That is indistinguishable from a software bug, and people lose
// hours to it.
//
// The colour bar is generated INSIDE the sensor and travels out over the same
// D0..D7 lines a real frame would. So: a clean bar means the flex and all eight
// data lines are good and the fault is elsewhere. A black or scrambled bar
// means the ribbon or the wiring, and no amount of reading code will fix it.
void eos_cam_colorbar(bool on);
void eos_cam_jpeg_free(uint8_t *buf);

#endif // EOS_CAM_H
