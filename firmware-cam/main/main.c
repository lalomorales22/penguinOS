// penguinOS camera node.
//
// A board with no screen, running the parts of penguinOS that do not need one.
// It provisions over the same captive portal, answers on the same mDNS name
// pattern, and serves the same /api/* namespace as every other board here -
// and then, instead of a desktop, it serves frames.
//
// The consumer is a board that DOES have a screen and cannot afford a JPEG
// decoder. So /api/cam/frame hands back raw RGB565, already rotated and scaled
// to exactly the size asked for, and the consumer blits it a strip at a time
// without ever holding a whole frame.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "eos_cam.h"
#include "eos_httpd.h"
#include "eos_net.h"

static const char *TAG = "eos";

static eos_net_t   net;
static eos_httpd_t httpd;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// --------------------------------------------------------------- the api
//
// One hook for the three camera routes. Anything else this image does not
// implement falls through to 501, which is what an unregistered route means
// here - and is the honest answer from a board with no filesystem app.

static int q_int(const char *uri, const char *name, int dflt, int lo, int hi)
{
    char buf[16];
    if (eos_httpd_query_get(uri, name, buf, sizeof buf) <= 0) return dflt;
    long v = strtol(buf, NULL, 10);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

static int cam_api(eos_httpd_t *h, int route,
                   const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_cam_info_t info;
    eos_cam_info(&info);

    switch (route) {
    case EOS_ROUTE_CAM_STATUS: {
        static char json[192];
        int n = snprintf(json, sizeof json,
                         "{\"ok\":%s,\"sensor\":\"%s\",\"src_w\":%u,\"src_h\":%u,"
                         "\"frames\":%lu,\"errors\":%lu,\"psram_free\":%u}",
                         info.ready ? "true" : "false", info.sensor,
                         info.src_w, info.src_h,
                         (unsigned long)info.frames, (unsigned long)info.errors,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        r->status       = 200;
        r->kind         = EOS_HTTPD_BODY_BUF;
        r->content_type = "application/json";
        r->cache_control = "no-store";
        r->body         = json;
        r->body_len     = n;
        return 200;
    }

    case EOS_ROUTE_CAM_FRAME: {
        if (!info.ready)
            return eos_httpd_fail(h, r, 503, "no_camera",
                                  "this node has no working sensor");
        // Defaults are a 240x320 portrait panel turned a quarter turn from the
        // sensor's landscape - the common case in this fleet, and the one that
        // needs no resampling at all.
        int w   = q_int(req->uri, "w", 240, 8, EOS_CAM_MAX_W);
        int h_  = q_int(req->uri, "h", 320, 8, EOS_CAM_MAX_H);
        int rot = q_int(req->uri, "rotate", 90, 0, 270);
        rot = (rot / 90) * 90;

        // ?colorbar=1 asks the SENSOR to generate a test pattern, which then
        // travels out over the same eight data lines a real frame does. It is
        // the fastest way to tell a wiring fault from a software one: a clean
        // bar exonerates the flex cable and D0..D7 together. Left on only for
        // this capture, so the next request sees the world again.
        int bar = q_int(req->uri, "colorbar", 0, 0, 1);
        if (bar) eos_cam_colorbar(true);

        // STRIPS. A consumer with a 29KB largest block cannot take 153,600
        // bytes, so it asks for one band at a time: y is the first row and
        // rows is how many. rows=0 means "the whole thing", which is what a
        // script or a desktop browser wants.
        int y    = q_int(req->uri, "y", 0, 0, EOS_CAM_MAX_H - 1);
        int rows = q_int(req->uri, "rows", 0, 0, EOS_CAM_MAX_H);

        // A NEW capture happens only at y == 0. Every later strip is sliced out
        // of that same held frame, so the twenty bands of one picture all come
        // from one moment - otherwise the assembled image tears across every
        // band boundary, which looks like a rendering bug rather than twenty
        // photographs of a moving world.
        if (y == 0) {
            int e = eos_cam_hold(w, h_, rot);
            if (bar) eos_cam_colorbar(false);
            if (e != 0)
                return eos_httpd_fail(h, r, 503, "capture_failed",
                                      "the sensor did not return a frame");
        } else if (bar) {
            eos_cam_colorbar(false);
        }

        if (rows == 0) rows = h_ - y;
        if (y + rows > h_) rows = h_ - y;
        if (rows <= 0)
            return eos_httpd_fail(h, r, 416, "bad_range",
                                  "that row range is outside the frame");

        size_t bytes = (size_t)rows * (size_t)w * 2u;
        // Served out of PSRAM. The static buffer is reused between requests so
        // a viewfinder does not allocate twenty times a second forever.
        static uint16_t *out; static size_t out_cap;
        if (out_cap < bytes) {
            free(out);
            out = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            out_cap = out ? bytes : 0;
        }
        if (!out)
            return eos_httpd_fail(h, r, 503, "no_memory",
                                  "could not allocate a strip that size");

        int n = eos_cam_strip(out, y, rows, out_cap);
        if (n < 0)
            return eos_httpd_fail(h, r, 503, "no_frame",
                                  "no frame is held; ask for y=0 first");

        r->status        = 200;
        r->kind          = EOS_HTTPD_BODY_BUF;
        r->content_type  = "application/octet-stream";
        r->cache_control = "no-store";
        r->body          = (const char *)out;
        r->body_len      = n;
        return 200;
    }

    case EOS_ROUTE_CAM_SNAP: {
        if (!info.ready)
            return eos_httpd_fail(h, r, 503, "no_camera",
                                  "this node has no working sensor");
        uint8_t *jpg = NULL; size_t len = 0;
        int q = q_int(req->uri, "q", 70, 10, 90);
        if (eos_cam_jpeg(&jpg, &len, q) != 0)
            return eos_httpd_fail(h, r, 503, "capture_failed",
                                  "the sensor did not return a frame");
        r->status        = 200;
        r->kind          = EOS_HTTPD_BODY_BUF;
        r->content_type  = "image/jpeg";
        r->cache_control = "no-store";
        r->body          = (const char *)jpg;
        r->body_len      = (int)len;
        return 200;
    }

    default:
        // Everything else - files, console, buddy - belongs to a board with a
        // screen and a filesystem. 501 is the truthful answer.
        return eos_httpd_fail_err(h, r, -7,
                                  "this is a camera node: it serves frames, "
                                  "not files or a desktop");
    }
}

// ---------------------------------------------------------------- app_main

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "penguinOS camera node");
    ESP_LOGI(TAG, "psram  %s, %u bytes free",
             esp_psram_is_initialized() ? "up" : "DOWN",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "heap   internal %u free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // The camera FIRST, deliberately. It is the only reason this node exists,
    // and a node that cannot see should say so before it spends fifteen
    // seconds joining a network to serve nothing.
    int ce = eos_cam_init();
    if (ce != 0)
        ESP_LOGE(TAG, "cam    init failed (%d) - the node will serve status only", ce);

    eos_net_cfg_t ncfg;
    eos_net_idf_defaults(&ncfg);
    eos_net_err_t nerr = eos_net_init(&net, &ncfg);
    bool net_ok = (nerr == EOS_NET_OK);
    if (!net_ok) ESP_LOGE(TAG, "net    init failed: %s", eos_net_err_name(nerr));

    if (net_ok) {
        nerr = eos_net_start(&net);
        if (nerr != EOS_NET_OK)
            ESP_LOGE(TAG, "net    start failed: %s", eos_net_err_name(nerr));
    }
    ESP_LOGI(TAG, "net    mode %s, credentials %s, ap \"%s\"",
             eos_net_mode_name(eos_net_mode(&net)),
             eos_net_cred_name(eos_net_cred(&net)), eos_net_ap_ssid(&net));

    eos_httpd_cfg_t hcfg;
    eos_httpd_cfg_default(&hcfg);
    hcfg.mode = (eos_net_mode(&net) == EOS_NET_SETUP) ? EOS_HTTPD_MODE_SETUP
                                                      : EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&httpd, NULL, NULL, &hcfg);
    eos_httpd_idf_bind(&httpd, net_ok ? &net : NULL);
    eos_httpd_set_api(cam_api);

    if (eos_httpd_start(&httpd) == 0)
        ESP_LOGI(TAG, "httpd  up on port 80 in %s mode",
                 hcfg.mode == EOS_HTTPD_MODE_SETUP ? "setup" : "run");
    else
        ESP_LOGE(TAG, "httpd  refused to start");

    ESP_LOGI(TAG, "ready  GET /api/cam/frame?w=240&h=320&rotate=90  -> raw RGB565");
    ESP_LOGI(TAG, "       GET /api/cam/snap                         -> jpeg");
    ESP_LOGI(TAG, "       GET /api/cam/status                       -> json");

    for (;;) {
        uint32_t t = now_ms();
        eos_net_pump(&net, t);
        eos_httpd_pump(&httpd, t);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
