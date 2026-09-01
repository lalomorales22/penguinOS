#include "eos_cam.h"

#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

static const char *TAG = "eos_cam";

// ---------------------------------------------------------------- the pins
//
// The XIAO ESP32-S3 Sense camera pinout. These are the values Seeed publishes
// and the ones esp32-camera carries as CAMERA_MODEL_XIAO_ESP32S3. VERIFIED on
// hardware: the sensor answered at SCCB address 0x3c and streams.
//
// THE SENSOR IS AN OV3660, NOT AN OV2640. The retail listing for this board
// says OV2640; the part on the module reports PID 0x3660 and esp32-camera
// identifies it as an OV3660 - a 3MP sensor rather than a 2MP one. The driver
// handles both, so nothing here changes, but do not trust the listing when
// choosing frame sizes: this sensor offers larger ones than an OV2640 would.
//
// A WRONG PIN HERE DOES NOT LOOK LIKE A WRONG PIN. esp_camera_init() will
// happily succeed with a bad data line and every frame comes back black or
// striped, which reads as a software bug for a long time before anyone
// suspects the wiring. The same is true of a camera ribbon that is seated but
// not latched - the commonest failure on this board, since the sensor is on a
// separate module with a flex connector.
//
// So: if init succeeds and frames are black, suspect the ribbon FIRST and these
// constants second, and check that the connector's latch is down.
#define CAM_PIN_PWDN   -1     // not brought out on this board
#define CAM_PIN_RESET  -1     // ditto; the sensor resets with the SoC
#define CAM_PIN_XCLK   10
#define CAM_PIN_SIOD   40     // SCCB data  (I2C-like, but not I2C)
#define CAM_PIN_SIOC   39     // SCCB clock
// NOTE the data lines are NOT in GPIO order: 15, 17, 18, 16, 14, 12, 11, 48.
// That looks like a transcription error and invites someone to "tidy" it. It is
// correct - five independent sources agree, including Seeed's schematic, whose
// net labels read IO15/DVP_Y2, IO17/DVP_Y3, IO18/DVP_Y4, IO16/DVP_Y5. Swapping
// two of them does not fail init; it scrambles colour, which is a much slower
// thing to notice. Copy this block, do not retype it.
#define CAM_PIN_D7     48
#define CAM_PIN_D6     11
#define CAM_PIN_D5     12
#define CAM_PIN_D4     14
#define CAM_PIN_D3     16
#define CAM_PIN_D2     18
#define CAM_PIN_D1     17
#define CAM_PIN_D0     15
#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_PCLK   13

static eos_cam_info_t s_info;

// ---------------------------------------------------------------- lifecycle

int eos_cam_init(void)
{
    memset(&s_info, 0, sizeof s_info);

    // The frame buffers live in PSRAM. Without it the sensor cannot be brought
    // up at any useful size at all, so say so plainly rather than failing
    // deeper in with a less obvious message.
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "no PSRAM: the camera's frame buffers have nowhere to live");
        return -1;
    }

    camera_config_t cfg = {
        .pin_pwdn = CAM_PIN_PWDN, .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD, .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6, .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4, .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC, .pin_href = CAM_PIN_HREF, .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_1,     // TIMER_0 is the backlight on the
        .ledc_channel = LEDC_CHANNEL_1,   // screen boards; keep the habit

        // RGB565 straight off the sensor, NOT JPEG. The screen boards need raw
        // pixels and cannot afford a decoder; a browser wanting JPEG is served
        // by frame2jpg() on demand, which is the cheap direction to convert.
        .pixel_format = PIXFORMAT_RGB565,

        // QVGA is 320x240, which is EXACTLY a 240x320 panel rotated a quarter
        // turn. The common case therefore needs no scaling at all - only a
        // rotation - and nearest-neighbour scaling never touches the picture.
        .frame_size   = FRAMESIZE_QVGA,

        .fb_count     = 2,                // one being filled, one being served
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,   // a viewfinder wants NEW frames,
                                              // not a queue of stale ones
    };

    esp_err_t e = esp_camera_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "  a pin is wrong, or the camera ribbon is not latched down");
        return -2;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) { ESP_LOGE(TAG, "sensor did not answer after init"); return -3; }

    camera_sensor_info_t *si = esp_camera_sensor_get_info(&s->id);
    snprintf(s_info.sensor, sizeof s_info.sensor, "%s", si ? si->name : "unknown");

    // The sensor is mounted such that its natural output is upside down on this
    // module. Correcting it here means every consumer gets a picture the right
    // way up without each of them knowing about the mounting.
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);

    s_info.ready = true;
    s_info.src_w = 320;
    s_info.src_h = 240;
    ESP_LOGI(TAG, "camera up: %s, capturing %ux%u RGB565 into PSRAM",
             s_info.sensor, s_info.src_w, s_info.src_h);
    return 0;
}

void eos_cam_info(eos_cam_info_t *out)
{
    if (out) *out = s_info;
}

// ------------------------------------------------------- rotate and scale
//
// Rotation is applied BEFORE scaling, and that order is the whole point: the
// sensor is landscape and these panels are portrait. Scaling 320x240 into
// 240x320 without rotating first gives a correctly sized picture of a world
// squashed to half its width - which looks like a bad camera rather than a
// missing transform.
//
// Nearest neighbour, deliberately. The common case is an exact quarter turn
// with no resampling at all, and where it does scale, a viewfinder on a
// two-inch panel gains nothing from bilinear that would justify the arithmetic
// on every pixel of every frame.

int eos_cam_frame_rgb565(uint16_t *dst, int w, int h, int rotate)
{
    if (!dst || w <= 0 || h <= 0) return -1;
    if (w > EOS_CAM_MAX_W || h > EOS_CAM_MAX_H) return -1;
    if (!s_info.ready) return -2;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { s_info.errors++; return -3; }
    if (fb->format != PIXFORMAT_RGB565) { esp_camera_fb_return(fb); return -4; }

    const uint16_t *src = (const uint16_t *)(const void *)fb->buf;
    const int sw = (int)fb->width, sh = (int)fb->height;

    // Dimensions AFTER rotation, which is the space the scale maps from.
    const int rw = (rotate == 90 || rotate == 270) ? sh : sw;
    const int rh = (rotate == 90 || rotate == 270) ? sw : sh;

    for (int dy = 0; dy < h; dy++) {
        const int ry = (int)((int64_t)dy * rh / h);
        uint16_t *row = dst + (size_t)dy * (size_t)w;
        for (int dx = 0; dx < w; dx++) {
            const int rx = (int)((int64_t)dx * rw / w);
            int sx, sy;
            switch (rotate) {
            case 90:  sx = ry;             sy = sh - 1 - rx;  break;
            case 180: sx = sw - 1 - rx;    sy = sh - 1 - ry;  break;
            case 270: sx = sw - 1 - ry;    sy = rx;           break;
            default:  sx = rx;             sy = ry;           break;
            }
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
            row[dx] = src[(size_t)sy * (size_t)sw + (size_t)sx];
        }
    }

    esp_camera_fb_return(fb);
    s_info.frames++;
    return 0;
}

// ------------------------------------------------------------ held frame

static uint16_t *s_held;          // in PSRAM; one frame, reused
static int       s_held_w, s_held_h;

int eos_cam_hold(int w, int h, int rotate)
{
    if (w <= 0 || h <= 0 || w > EOS_CAM_MAX_W || h > EOS_CAM_MAX_H) return -1;

    // Reallocate only when the shape changes. A consumer asks for the same
    // size every time, so in steady state this allocates once and never again -
    // which matters because PSRAM fragments like any other heap.
    if (!s_held || s_held_w != w || s_held_h != h) {
        free(s_held);
        s_held = heap_caps_malloc((size_t)w * (size_t)h * 2u,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_held) { s_held_w = s_held_h = 0; return -2; }
        s_held_w = w; s_held_h = h;
    }
    return eos_cam_frame_rgb565(s_held, w, h, rotate);
}

int eos_cam_strip(uint16_t *dst, int y, int rows, size_t cap)
{
    if (!dst || !s_held) return -1;
    if (y < 0 || rows <= 0 || y + rows > s_held_h) return -2;
    size_t bytes = (size_t)rows * (size_t)s_held_w * 2u;
    if (bytes > cap) return -3;
    memcpy(dst, s_held + (size_t)y * (size_t)s_held_w, bytes);
    return (int)bytes;
}

// ------------------------------------------------------------- diagnostics

void eos_cam_colorbar(bool on)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->set_colorbar) s->set_colorbar(s, on ? 1 : 0);
}

// ------------------------------------------------------------------- jpeg

int eos_cam_jpeg(uint8_t **out, size_t *len, int quality)
{
    if (!out || !len) return -1;
    if (!s_info.ready) return -2;
    if (quality < 10) quality = 10;
    if (quality > 90) quality = 90;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { s_info.errors++; return -3; }

    // Converting RGB565 -> JPEG here is the cheap direction. The expensive one
    // would be decoding JPEG on a board with 29KB of largest free block, which
    // is exactly what capturing in RGB565 avoids.
    bool ok = frame2jpg(fb, quality, out, len);
    esp_camera_fb_return(fb);
    if (!ok) { s_info.errors++; return -4; }
    s_info.frames++;
    return 0;
}

void eos_cam_jpeg_free(uint8_t *buf)
{
    if (buf) free(buf);
}
