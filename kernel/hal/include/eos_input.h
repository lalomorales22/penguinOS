// eos_input — one event queue, four sources, one keycode space.
//
// A BLE keyboard, the board's GPIO buttons, an optional touch controller, and
// events injected from the phone page or USB serial all arrive as the same
// eos_event_t in the same ring. Nothing above this line knows or cares which
// one moved.
//
// The keycode space IS USB HID usage codes, unchanged. The K809 already speaks
// them, so no translation table sits between the radio and the queue, and the
// modifier byte in every event is literally the first byte of the HID report.
// Buttons and injected events borrow the same numbers. That is what makes a
// super-key bind work identically whether the user pressed it on the keyboard
// or tapped it on their phone.
//
// The one non-obvious constraint: web and serial sources cannot be trusted to
// send a release. A dropped release packet leaves a direction latched forever,
// which on the arcade code meant a ship that would not stop moving. So every
// injected hold carries an expiry (cfg.web_hold_ms, 600ms in practice) and the
// held-key state decays on its own. Keyboard holds do not expire — BLE
// delivers releases reliably, and a disconnect clears the whole held set.
//
// eos_input_push() is callable from a BLE callback or a GPIO ISR. Everything
// else is main-loop only. Nothing here allocates; the ring is a fixed array.

#ifndef EOS_INPUT_H
#define EOS_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_board.h"

// ---------------------------------------------------------------- keycodes
//
// USB HID keyboard usage page, verbatim. Only the usages the OS actually binds
// are named; any other usage still travels through the queue as a raw number.

#define EOS_KEY_NONE      0x00
#define EOS_KEY_A         0x04
#define EOS_KEY_B         0x05
#define EOS_KEY_C         0x06
#define EOS_KEY_D         0x07
#define EOS_KEY_E         0x08
#define EOS_KEY_F         0x09
#define EOS_KEY_G         0x0A
#define EOS_KEY_H         0x0B
#define EOS_KEY_I         0x0C
#define EOS_KEY_J         0x0D
#define EOS_KEY_K         0x0E
#define EOS_KEY_L         0x0F
#define EOS_KEY_M         0x10
#define EOS_KEY_N         0x11
#define EOS_KEY_O         0x12
#define EOS_KEY_P         0x13
#define EOS_KEY_Q         0x14
#define EOS_KEY_R         0x15
#define EOS_KEY_S         0x16
#define EOS_KEY_T         0x17
#define EOS_KEY_U         0x18
#define EOS_KEY_V         0x19
#define EOS_KEY_W         0x1A
#define EOS_KEY_X         0x1B
#define EOS_KEY_Y         0x1C
#define EOS_KEY_Z         0x1D
#define EOS_KEY_1         0x1E
#define EOS_KEY_2         0x1F
#define EOS_KEY_3         0x20
#define EOS_KEY_4         0x21
#define EOS_KEY_5         0x22
#define EOS_KEY_6         0x23
#define EOS_KEY_7         0x24
#define EOS_KEY_8         0x25
#define EOS_KEY_9         0x26
#define EOS_KEY_0         0x27
#define EOS_KEY_ENTER     0x28
#define EOS_KEY_ESC       0x29
#define EOS_KEY_BKSP      0x2A
#define EOS_KEY_TAB       0x2B
#define EOS_KEY_SPACE     0x2C
#define EOS_KEY_MINUS     0x2D
#define EOS_KEY_EQUAL     0x2E
#define EOS_KEY_LBRACKET  0x2F
#define EOS_KEY_RBRACKET  0x30
#define EOS_KEY_BACKSLASH 0x31
#define EOS_KEY_SEMICOLON 0x33
#define EOS_KEY_QUOTE     0x34
#define EOS_KEY_GRAVE     0x35
#define EOS_KEY_COMMA     0x36
#define EOS_KEY_PERIOD    0x37
#define EOS_KEY_SLASH     0x38
#define EOS_KEY_CAPSLOCK  0x39
#define EOS_KEY_F1        0x3A
#define EOS_KEY_F2        0x3B
#define EOS_KEY_F3        0x3C
#define EOS_KEY_F4        0x3D
#define EOS_KEY_F5        0x3E
#define EOS_KEY_F6        0x3F
#define EOS_KEY_F7        0x40
#define EOS_KEY_F8        0x41
#define EOS_KEY_F9        0x42
#define EOS_KEY_F10       0x43
#define EOS_KEY_F11       0x44
#define EOS_KEY_F12       0x45
#define EOS_KEY_INSERT    0x49
#define EOS_KEY_HOME      0x4A
#define EOS_KEY_PGUP      0x4B
#define EOS_KEY_DELETE    0x4C
#define EOS_KEY_END       0x4D
#define EOS_KEY_PGDN      0x4E
#define EOS_KEY_RIGHT     0x4F
#define EOS_KEY_LEFT      0x50
#define EOS_KEY_DOWN      0x51
#define EOS_KEY_UP        0x52
#define EOS_KEY_LCTRL     0xE0
#define EOS_KEY_LSHIFT    0xE1
#define EOS_KEY_LALT      0xE2
#define EOS_KEY_LGUI      0xE3
#define EOS_KEY_RCTRL     0xE4
#define EOS_KEY_RSHIFT    0xE5
#define EOS_KEY_RALT      0xE6
#define EOS_KEY_RGUI      0xE7

// 0xF0..0xFF are penguinOS usages for hardware HID has no number for. They are
// above the HID range on purpose so a real keyboard can never collide.
#define EOS_KEY_BOOT      0xF0   // the BOOT / IO0 button every ESP32 board has
#define EOS_KEY_USER1     0xF1
#define EOS_KEY_USER2     0xF2
#define EOS_KEY_USER3     0xF3

#define EOS_KEY_COUNT     256

// --------------------------------------------------------------- modifiers
//
// Exactly the HID report's byte 0. The super key is GUI; the shell's
// Omarchy-style binds all test EOS_MOD_SUPER.

#define EOS_MOD_LCTRL   0x01
#define EOS_MOD_LSHIFT  0x02
#define EOS_MOD_LALT    0x04
#define EOS_MOD_LGUI    0x08
#define EOS_MOD_RCTRL   0x10
#define EOS_MOD_RSHIFT  0x20
#define EOS_MOD_RALT    0x40
#define EOS_MOD_RGUI    0x80

#define EOS_MOD_CTRL    (EOS_MOD_LCTRL  | EOS_MOD_RCTRL)
#define EOS_MOD_SHIFT   (EOS_MOD_LSHIFT | EOS_MOD_RSHIFT)
#define EOS_MOD_ALT     (EOS_MOD_LALT   | EOS_MOD_RALT)
#define EOS_MOD_SUPER   (EOS_MOD_LGUI   | EOS_MOD_RGUI)

// The modifier bit a modifier keycode contributes, or 0 for anything else.
static inline uint8_t eos_mod_bit(uint8_t key)
{
    if (key < EOS_KEY_LCTRL || key > EOS_KEY_RGUI) return 0;
    return (uint8_t)(1u << (key - EOS_KEY_LCTRL));
}

static inline bool eos_key_is_mod(uint8_t key)
{
    return key >= EOS_KEY_LCTRL && key <= EOS_KEY_RGUI;
}

// ------------------------------------------------------------------ events

typedef enum {
    EOS_SRC_NONE = 0,
    EOS_SRC_KEYBOARD,   // BLE HID host (NimBLE)
    EOS_SRC_BUTTON,     // a GPIO the board declared
    EOS_SRC_TOUCH,
    EOS_SRC_WEB,        // the phone page
    EOS_SRC_SERIAL,     // USB serial console
} eos_src_t;

static inline const char *eos_src_name(uint8_t s)
{
    switch (s) {
    case EOS_SRC_NONE:     return "none";
    case EOS_SRC_KEYBOARD: return "keyboard";
    case EOS_SRC_BUTTON:   return "button";
    case EOS_SRC_TOUCH:    return "touch";
    case EOS_SRC_WEB:      return "web";
    case EOS_SRC_SERIAL:   return "serial";
    }
    return "?";
}

typedef enum {
    EOS_EV_NONE = 0,
    EOS_EV_KEY_DOWN,
    EOS_EV_KEY_UP,
    EOS_EV_KEY_REPEAT,  // synthesised by eos_input_tick() from held state
    EOS_EV_TEXT,        // one printable character, layout and modifiers already applied
    EOS_EV_TOUCH_DOWN,
    EOS_EV_TOUCH_MOVE,
    EOS_EV_TOUCH_UP,
    EOS_EV_CONNECT,     // a source came up: keyboard bonded, phone page opened
    EOS_EV_DISCONNECT,  // and went away. Held keys from that source are cleared.
} eos_ev_t;

// 16 bytes, asserted below. At EOS_INPUT_QUEUE the ring costs 512 bytes of
// static RAM, which is a real fraction of what is left on a board sitting at
// 20KB of free heap — hence the packing, and hence no spare fields.
typedef struct {
    uint8_t  type;   // eos_ev_t
    uint8_t  src;    // eos_src_t
    uint8_t  key;    // EOS_KEY_* for KEY_DOWN/UP/REPEAT, else 0
    uint8_t  mods;   // modifier state at the instant of the event
    uint16_t ch;     // EOS_EV_TEXT: the character. Latin-1 range; 0 otherwise.
    int16_t  x, y;   // touch position in screen pixels, post-rotation
    uint32_t ms;     // millisecond timestamp the source stamped
} eos_event_t;       // the two bytes before ms are alignment padding, not spare

// C99 has no _Static_assert, so this is the negative-array-size idiom: the
// typedef fails to compile if the layout ever drifts off 16 bytes. It matters
// because the ring is sized in events, not bytes, and a padded-out event would
// quietly cost twice the RAM the comment above promises.
typedef char eos_event_is_16_bytes[(sizeof(eos_event_t) == 16) ? 1 : -1];

// ------------------------------------------------------------------ config

#define EOS_INPUT_QUEUE 32   // events; a full ring drops the newest and says so

typedef struct {
    uint16_t repeat_delay_ms;  // held -> first EOS_EV_KEY_REPEAT. 0 disables repeat.
    uint16_t repeat_rate_ms;   // between repeats after that
    uint16_t web_hold_ms;      // injected holds expire after this with no refresh
    bool     repeat_mods;      // normally false: nobody wants a repeating shift
} eos_input_cfg_t;

// The defaults the arcade and terminal code arrived at the hard way.
static inline eos_input_cfg_t eos_input_defaults(void)
{
    eos_input_cfg_t c;
    c.repeat_delay_ms = 400;
    c.repeat_rate_ms  = 60;
    c.web_hold_ms     = 600;
    c.repeat_mods     = false;
    return c;
}

// --------------------------------------------------------------- lifecycle

// Brings up the sources the board declares: the NimBLE HID host if
// input.ble_keyboard, the declared buttons, the touch controller if any. Pass
// NULL for eos_input_defaults(). Allocates nothing — the ring, the held-key
// bitmap and the debounce state are fixed arrays in the backend.
eos_err_t eos_input_init(const eos_input_cfg_t *cfg);

// Call once per main-loop pass with the current millisecond clock. Polls the
// buttons with debounce, expires injected holds, and emits EOS_EV_KEY_REPEAT
// for anything held past the configured delay. Never blocks.
void eos_input_tick(uint32_t now_ms);

// ------------------------------------------------------------------- queue

// Pops the oldest event. False when the ring is empty. Main loop only.
bool eos_input_poll(eos_event_t *out);

// Peeks without popping, so the shell can decide whether a chord is starting
// before it consumes the first key.
bool eos_input_peek(eos_event_t *out);

// Drops everything queued but keeps held state. What you call on a screen
// transition so buffered keys do not fire into the new screen.
void eos_input_flush(void);

// Pushes an event. SAFE FROM AN ISR OR A BLE CALLBACK. Returns false and drops
// the event when the ring is full; it never overwrites unread history, because
// losing a key-up is worse than losing a key-down.
bool eos_input_push(const eos_event_t *e);

// Events dropped for a full ring since init. Nonzero means the main loop is
// not draining fast enough.
uint32_t eos_input_dropped(void);

// -------------------------------------------------------------- held state
//
// The arcade code needs this: a ship moves while left is down, not once per
// keypress, and polling held state is the only way to get that right when the
// repeat rate and the frame rate disagree.

bool     eos_input_held(uint8_t key);
uint8_t  eos_input_mods(void);

// Milliseconds the key has been down, or 0 when it is not. Used for
// long-press: hold escape to force-close a window.
uint32_t eos_input_held_ms(uint8_t key, uint32_t now_ms);

// Any key at all currently down. Cheap idle check for the backlight timer.
bool eos_input_any_held(void);

// Clears every held key without generating key-up events. Called on
// disconnect, and by the shell when it hands focus to a different app.
void eos_input_clear_held(void);

// -------------------------------------------------------------- injection
//
// What a driver calls. Above the HAL nobody touches these.

// Feeds a raw 8-byte HID keyboard input report: mods, reserved, then six
// usages. The HAL diffs it against the previous report and produces the
// KEY_DOWN, KEY_UP and TEXT events, so no driver ever repeats that logic.
// ISR/callback safe.
void eos_input_hid_report(const uint8_t *report, uint8_t len, uint32_t now_ms);

// A key from a source that is not a HID keyboard. `down` false releases it.
// For EOS_SRC_WEB and EOS_SRC_SERIAL a press also arms the hold expiry, so a
// page that stops refreshing releases the key by itself.
void eos_input_inject_key(uint8_t key, bool down, uint8_t mods,
                          uint8_t src, uint32_t now_ms);

// A character with no keycode behind it — the phone page's text field, or a
// byte off the serial console. Emits EOS_EV_TEXT only.
void eos_input_inject_text(uint16_t ch, uint8_t src, uint32_t now_ms);

// Touch position in post-rotation screen pixels. type is one of
// EOS_EV_TOUCH_DOWN / _MOVE / _UP.
void eos_input_inject_touch(uint8_t type, int16_t x, int16_t y,
                            uint8_t src, uint32_t now_ms);

// A source appeared or went away. Going away clears that source's held keys.
void eos_input_inject_conn(uint8_t src, bool up, uint32_t now_ms);

// ------------------------------------------------------------- us keymap
//
// HID usage plus modifiers to a character. US layout, the only one the K809
// reports. Ctrl-A..Z folds to 1..26 so the terminal and CP/M get their control
// codes for free. Returns 0 for keys with no character.
//
// Caps lock is deliberately not handled: the lock state lives in the LED
// output report, which this HAL does not send, so treating capslock as shift
// would be wrong half the time.
static inline uint16_t eos_input_char(uint8_t key, uint8_t mods)
{
    static const char km[0x39][2] = {
        {0,0},{0,0},{0,0},{0,0},
        {'a','A'},{'b','B'},{'c','C'},{'d','D'},{'e','E'},{'f','F'},{'g','G'},
        {'h','H'},{'i','I'},{'j','J'},{'k','K'},{'l','L'},{'m','M'},{'n','N'},
        {'o','O'},{'p','P'},{'q','Q'},{'r','R'},{'s','S'},{'t','T'},{'u','U'},
        {'v','V'},{'w','W'},{'x','X'},{'y','Y'},{'z','Z'},
        {'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},
        {'6','^'},{'7','&'},{'8','*'},{'9','('},{'0',')'},
        {'\n','\n'},{27,27},{'\b','\b'},{'\t','\t'},{' ',' '},
        {'-','_'},{'=','+'},{'[','{'},{']','}'},{'\\','|'},{0,0},
        {';',':'},{'\'','"'},{'`','~'},{',','<'},{'.','>'},{'/','?'},
    };
    if (key == EOS_KEY_DELETE) return 127;
    if (key >= 0x39) return 0;
    if ((mods & EOS_MOD_CTRL) && key >= EOS_KEY_A && key <= EOS_KEY_Z)
        return (uint16_t)(key - EOS_KEY_A + 1);          // ctrl-a == 1
    return (uint16_t)(unsigned char)km[key][(mods & EOS_MOD_SHIFT) ? 1 : 0];
}

// ------------------------------------------------------------ bind helper
//
// One place the shell's key table is matched, so "super+shift+h" means the
// same thing whether it came from BLE or from a tap on the phone.

typedef struct {
    uint8_t key;
    uint8_t mods;       // required bits, in the collapsed EOS_MOD_CTRL form
    uint8_t forbid;     // bits that must be absent; 0 for don't-care
} eos_bind_t;

static inline bool eos_bind_match(const eos_bind_t *b, const eos_event_t *e)
{
    if (e->type != EOS_EV_KEY_DOWN && e->type != EOS_EV_KEY_REPEAT) return false;
    if (e->key != b->key) return false;
    // Every required group must have at least one of its bits present.
    uint8_t need = b->mods;
    if ((need & EOS_MOD_CTRL)  && !(e->mods & EOS_MOD_CTRL))  return false;
    if ((need & EOS_MOD_SHIFT) && !(e->mods & EOS_MOD_SHIFT)) return false;
    if ((need & EOS_MOD_ALT)   && !(e->mods & EOS_MOD_ALT))   return false;
    if ((need & EOS_MOD_SUPER) && !(e->mods & EOS_MOD_SUPER)) return false;
    if (b->forbid & e->mods) return false;
    return true;
}

#endif // EOS_INPUT_H
