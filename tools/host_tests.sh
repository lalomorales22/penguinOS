#!/bin/sh
# Every host suite in one place. Run from the repo root. Prints one line per
# suite plus a total; exits non-zero if any suite reports a failure or a
# compile error.
#
# The build lines are copied from each component's README and must stay in
# step with them: a suite that stops being listed here is a suite that stops
# being run.
set -u
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-cc}
FLAGS="-std=c99 -Wall -Wextra -O1"
OUT=${TMPDIR:-/tmp}/eos-host
mkdir -p "$OUT"
fail=0
total=0

run() {
    name=$1; shift
    if ! $CC $FLAGS "$@" -o "$OUT/t_$name" 2> "$OUT/$name.cc"; then
        printf '%-10s BUILD FAILED\n' "$name"; cat "$OUT/$name.cc"; fail=1; return
    fi
    out=$("$OUT/t_$name" 2>&1) || { printf '%-10s RUN FAILED\n' "$name"; echo "$out" | tail -5; fail=1; return; }
    line=$(echo "$out" | grep -E '[0-9]+ checks,' | tail -1)
    n=$(echo "$line" | sed -E 's/ checks,.*//; s/.*[^0-9]//')
    f=$(echo "$line" | sed -E 's/.* failed.*//; s/.*checks, *//; s/[^0-9].*//')
    f=$(echo "$line" | sed -E 's/^.*checks, *//; s/ .*//')
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    case "$f" in ''|*[!0-9]*) f=0 ;; esac
    total=$((total + n))
    [ "$f" -ne 0 ] && fail=1
    printf '%-10s %6s checks %s failed\n' "$name" "$n" "$f"
}

run wm      -Ikernel/wm/include kernel/wm/eos_wm.c kernel/wm/test/test_wm.c
run theme   -Ikernel/theme/include kernel/theme/eos_theme.c kernel/theme/test/test_theme.c
run avatar  -Ikernel/avatar/include kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
            kernel/avatar/test/test_vox.c -lm
run brain   -Ikernel/svc/include kernel/svc/eos_brain.c kernel/svc/test/test_brain.c
run shell   -Ikernel/wm/include -Ikernel/hal/include -Ikernel/shell/include \
            kernel/wm/eos_wm.c kernel/shell/eos_keys.c kernel/shell/eos_bar.c \
            kernel/shell/test/test_shell.c
run launcher -Ikernel/wm/include -Ikernel/hal/include -Ikernel/shell/include \
            -Iboards/generated kernel/wm/eos_wm.c kernel/shell/eos_keys.c \
            kernel/shell/eos_bar.c kernel/shell/eos_launcher.c \
            kernel/shell/test/test_launcher.c
run pointer -Ikernel/hal/include -Ikernel/wm/include -Ikernel/shell/include \
            -Ikernel/svc/include -Iboards/generated \
            kernel/shell/eos_pointer.c kernel/wm/eos_wm.c kernel/hal/eos_input.c \
            kernel/svc/eos_ble.c kernel/shell/test/test_pointer.c
run font    -Ikernel/font/include -Ikernel/hal/include -Ikernel/wm/include \
            kernel/font/eos_font.c kernel/font/test/test_font.c
run integ   -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
            -Ikernel/font/include -Ikernel/shell/include -Ikernel/svc/include \
            -Ikernel/avatar/include -Ikernel/qr/include -Iboards/generated \
            kernel/test/test_integration.c kernel/wm/eos_wm.c kernel/theme/eos_theme.c \
            kernel/font/eos_font.c kernel/shell/eos_bar.c kernel/shell/eos_keys.c \
            kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c kernel/qr/eos_qr.c -lm
run display -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
            -Iboards/generated -Ikernel/font \
            kernel/hal/backend/esp_lcd/eos_display_st7789.c kernel/theme/eos_theme.c \
            kernel/hal/backend/esp_lcd/test/test_display.c -lm
run qr      -Ikernel/qr/include kernel/qr/eos_qr.c kernel/qr/test/test_qr.c
run net     -Ikernel/svc/include kernel/svc/eos_net.c kernel/svc/eos_radio.c \
            kernel/svc/test/test_net.c
run ble     -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include \
            -Ikernel/shell/include kernel/svc/eos_ble.c kernel/svc/eos_radio.c \
            kernel/hal/eos_input.c kernel/wm/eos_wm.c kernel/shell/eos_keys.c \
            kernel/svc/test/test_ble.c
run httpd   -Ikernel/svc/include kernel/svc/eos_httpd.c kernel/svc/test/test_httpd.c
run storage -Ikernel/hal/include -Ikernel/wm/include -Iboards/generated \
            kernel/hal/backend/storage/eos_storage_idf.c \
            kernel/hal/backend/storage/test/test_storage.c
run settings -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include \
            -Iboards/generated kernel/svc/eos_settings.c kernel/svc/eos_httpd.c \
            kernel/hal/backend/storage/eos_storage_idf.c \
            kernel/svc/test/test_settings.c
run apps    -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include \
            -Ikernel/avatar/include -Iboards/generated \
            kernel/svc/eos_apps.c kernel/svc/eos_httpd.c \
            kernel/hal/backend/storage/eos_storage_idf.c \
            kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
            kernel/svc/test/test_apps.c
run draw    -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
            -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
            -Ikernel/svc/include -Iboards/generated -Ifirmware/main \
            firmware/main/test/test_shell_draw.c firmware/main/eos_buddy_model.c \
            firmware/main/eos_app_registry.c firmware/main/eos_app_basic.c \
            firmware/main/eos_app_chat.c firmware/main/eos_app_files.c \
            firmware/main/eos_app_media.c firmware/main/eos_app_party.c \
            firmware/main/eos_led.c \
            kernel/hal/backend/storage/eos_storage_idf.c \
            kernel/hal/backend/esp_lcd/eos_display_st7789.c kernel/wm/eos_wm.c \
            kernel/theme/eos_theme.c kernel/shell/eos_bar.c kernel/shell/eos_keys.c \
            kernel/shell/eos_launcher.c kernel/shell/eos_pointer.c \
            kernel/hal/eos_input.c \
            kernel/font/eos_font.c kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c -lm
run appsui  -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
            -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
            -Ikernel/svc/include -Iboards/generated -Ifirmware/main \
            firmware/main/test/test_apps_ui.c \
            firmware/main/eos_app_registry.c firmware/main/eos_app_basic.c \
            firmware/main/eos_app_chat.c firmware/main/eos_app_files.c \
            firmware/main/eos_app_media.c firmware/main/eos_app_party.c \
            firmware/main/eos_led.c firmware/main/eos_shell_draw.c \
            kernel/hal/backend/esp_lcd/eos_display_st7789.c \
            kernel/hal/backend/storage/eos_storage_idf.c \
            kernel/svc/eos_apps.c kernel/svc/eos_httpd.c \
            kernel/wm/eos_wm.c kernel/theme/eos_theme.c kernel/shell/eos_bar.c \
            kernel/shell/eos_keys.c kernel/shell/eos_launcher.c \
            kernel/shell/eos_pointer.c kernel/hal/eos_input.c \
            kernel/font/eos_font.c \
            kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c -lm
run dispatch -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
            -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
            -Ikernel/svc/include -Iboards/generated -Ifirmware/main \
            firmware/main/test/test_dispatch.c firmware/main/eos_shell_input.c \
            firmware/main/eos_shell_draw.c firmware/main/eos_buddy_model.c \
            firmware/main/eos_app_registry.c firmware/main/eos_app_basic.c \
            firmware/main/eos_app_chat.c firmware/main/eos_app_files.c \
            firmware/main/eos_app_media.c firmware/main/eos_app_party.c \
            firmware/main/eos_led.c \
            kernel/hal/backend/esp_lcd/eos_display_st7789.c \
            kernel/hal/backend/storage/eos_storage_idf.c \
            kernel/svc/eos_apps.c kernel/svc/eos_httpd.c \
            kernel/wm/eos_wm.c kernel/theme/eos_theme.c kernel/shell/eos_bar.c \
            kernel/shell/eos_keys.c kernel/shell/eos_launcher.c \
            kernel/shell/eos_pointer.c kernel/hal/eos_input.c \
            kernel/font/eos_font.c \
            kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c -lm
run setup   -Wpedantic -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
            -Ikernel/font/include -Ikernel/qr/include -Ifirmware/main \
            firmware/main/test/test_setup_screen.c firmware/main/eos_setup_screen.c \
            kernel/qr/eos_qr.c kernel/font/eos_font.c kernel/theme/eos_theme.c -lm

printf '%-10s %6d checks total\n' TOTAL "$total"
exit $fail
