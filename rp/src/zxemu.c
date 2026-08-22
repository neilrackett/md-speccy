/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ZX Spectrum emulator front-end for the SidecarTridge Multi-device
 * framebuffer template. Ported from the upstream zx2040 project
 * (Copyright (C) 2024 Salvatore Sanfilippo, MIT licence), replacing
 * its ST77xx display / GPIO input / PWM audio / flash game storage
 * with the template's 320x200 framebuffer, Atari ST keyboard input,
 * YM2149 audio and SD-card game storage. The emulator core under zx/
 * is vendored unmodified under its original licence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "audio.h"
#include "fb.h"
#include "fb_chunked.h"
#include "palette.h"
#include "cart_shared.h"  // __cart_app_free() for the buffers parked below
#include "debug.h"
#include "zxemu.h"

#include "device_config.h" // Audio / display metrics.

// VRAM update tracking function, this is used inside mem.h.
void vram_set_dirty_bitmap(uint16_t addr);
void vram_set_dirty_attr(uint16_t addr);
void vram_force_dirty(void);

#define CHIPS_IMPL
#include "chips_common.h"
#include "mem.h"
#include "z80.h"
#include "kbd.h"
#include "clk.h"
#include "zx.h"
#include "zx-roms.h"

#include "ff.h"
#include "sdcard.h"
#include "aconfig.h"
#include "settings/settings.h"
#include "builtin_game.h"      // embedded games seeded into the app folder
#include "builtin_pongwars.h"

// The Spectrum bitmap (256x192) is shown 1:1, centred in the 320x200
// framebuffer; the surrounding margin is the Spectrum border.
#define ZX_FB_X0 ((FB_CHUNKED_W - 256) / 2)   // 32
#define ZX_FB_Y0 ((FB_CHUNKED_H - 192) / 2)   // 4

#define ZX_DEFAULT_SCANLINE_PERIOD 150

// Spectrum display file: 6144 bytes of bitmap + 768 bytes of attributes.
#define ZX_VRAM_SIZE 6912u

// Snapshot filename classification (defined in the game-storage section;
// the menu renderer uses it to hide the extension).
enum { SNAP_NONE = 0, SNAP_Z80, SNAP_SNA };
static int snapshot_type(const char *name);

// Blink phase for the Spectrum FLASH attribute, toggled by the frame
// counter in zxemu_render_frame().
static uint32_t zx_blink_phase = 0;

// Scratch copy of the Spectrum VRAM. The menu / About overlay draws
// destructively into VRAM, so we snapshot it here, draw, decode to the
// framebuffer, then restore -- compositing the overlay for one frame
// without leaving artifacts in VRAM once it's dismissed.
static uint8_t s_vram_save[ZX_VRAM_SIZE] __cart_app_free("vram_save");

// Beeper sample ring storage for zx_t.audiobuf (a pointer since the
// md-speccy port -- see zx.h). Uncleared is fine: the consumer only
// reads bits the producer has written.
static uint32_t zx_audiobuf_store[AUDIOBUF_LEN] __cart_app_free("audiobuf");

/* Modified for even RGB565 conversion. */
static uint32_t zxpalette[16] = {
    0x000000,     // std black
    0xD80000,     // std blue
    0x0000D8,     // std red
    0xD800D8,     // std magenta
    0x00D800,     // std green
    0xD8D800,     // std cyan
    0x00D8D8,     // std yellow
    0xD8D8D8,     // std white
    0x000000,     // bright black
    0xFF0000,     // bright blue
    0x0000FF,     // bright red
    0xFF00FF,     // bright magenta
    0x00FF00,     // bright green
    0xFFFF00,     // bright cyan
    0x00FFFF,     // bright yellow
    0xFFFFFF,     // bright white
};

void load_game(int game_id);

/* =============================== Games list =============================== */

#define ZX_MAX_GAMES     64     // Max .z80/.sna snapshots listed from SD
                                // (bounded by the cart-hole RAM GamesTable
                                // lives in; the scan stops cleanly at the cap).

// Snapshots baked into the firmware (flash) and seeded into the app
// folder on boot -- see populate_games_list().
static const struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
} BuiltinGames[] = {
    {"3dshow_demo.z80", zx_builtin_game,     zx_builtin_game_len},
    {"pongwars.z80",    zx_builtin_pongwars, zx_builtin_pongwars_len},
};
#define ZX_BUILTIN_GAMES (sizeof(BuiltinGames) / sizeof(BuiltinGames[0]))

struct game_entry {
    char name[28];          // Z80 snapshot filename (SD) or label.
    void *addr;             // Flash/RAM address (baked-in build only).
    uint32_t size;          // Length in bytes.
};

// Populated during initialization from the SD app folder (or the single
// baked-in game). Static storage -- no heap fragmentation.
struct game_entry GamesTable[ZX_MAX_GAMES] __cart_app_free("games");
uint32_t GamesTableSize;

/* ==================== Atari ST keyboard -> ZX Spectrum ==================== */

// ST IKBD scancodes we treat specially (cursor cluster + a few others).
#define ST_SC_ESC     0x01
#define ST_SC_BACKSP  0x0E
#define ST_SC_ENTER   0x1C
#define ST_SC_LSHIFT  0x2A
#define ST_SC_RSHIFT  0x36
#define ST_SC_ALT     0x38
#define ST_SC_SPACE   0x39
#define ST_SC_CAPSLK  0x3A
#define ST_SC_CLRHOME 0x47
#define ST_SC_UP      0x48
#define ST_SC_LEFT    0x4B
#define ST_SC_RIGHT   0x4D
#define ST_SC_DOWN    0x50
#define ST_SC_INSERT  0x52
#define ST_SC_DELETE  0x53

// ZX key codes as registered in the emulator kbd matrix. Sym Shift has a
// standalone code upstream; Caps Shift does not, so we register one at
// init (see zxemu_init) at an otherwise-unused code.
#define ZX_KEY_CAPS   0x88   // standalone Caps Shift (registered on EMU.zx.kbd)
#define ZX_KEY_CAPSLK 0x89   // Caps Lock = Caps Shift + 2 (registered at init)
#define ZX_KEY_SYM    0x0F   // standalone Sym Shift (already in the matrix)
#define ZX_KEY_DELETE 0x0C   // Caps Shift + 0
#define ZX_KEY_ENTER  0x0D
#define ZX_KEY_CLEFT  0x08   // Spectrum cursor keys (Caps Shift + 5/6/7/8)
#define ZX_KEY_CRIGHT 0x09
#define ZX_KEY_CDOWN  0x0A
#define ZX_KEY_CUP    0x0B

// Positional ST-scancode -> ZX key-code table (0 = unmapped). Alphanumerics
// map 1:1; Space/Enter/Backspace/Shift/Alt as noted. The cursor cluster and
// Esc are handled explicitly in zxemu_handle_key.
//
// Punctuation keys map to the same symbol printed on the ST keycap. On the
// Spectrum those are Symbol Shift combos, but the kbd matrix registers the
// punctuation codes with the Sym Shift modifier, so it applies Sym Shift
// automatically -- one ST key = the combo. (Shifted punctuation still only
// matches the Spectrum layout, e.g. Alt+T for '>', not ST Shift + '.'.)
static const uint8_t st2zx[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',
    [0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',
    [0x31]='n',[0x32]='m',
    // Punctuation (auto Sym Shift via the matrix): - = ; ' , . /
    [0x0C]='-',[0x0D]='=',[0x27]=';',[0x28]='\'',
    [0x33]=',',[0x34]='.',[0x35]='/',
    [ST_SC_SPACE]=' ',   [ST_SC_ENTER]=ZX_KEY_ENTER,
    [ST_SC_BACKSP]=ZX_KEY_DELETE, [ST_SC_DELETE]=ZX_KEY_DELETE,
    [ST_SC_CAPSLK]=ZX_KEY_CAPSLK,
    [ST_SC_LSHIFT]=ZX_KEY_CAPS, [ST_SC_RSHIFT]=ZX_KEY_CAPS,
    [ST_SC_ALT]=ZX_KEY_SYM,
};

// Kempston mask contributed by the cursor keys / Ins / Clr-Home (OR'd with
// the real ST joystick each frame). Bits are ZX_JOYSTICK_* from zx.h.
static uint8_t zx_cursor_kempston = 0;

/* ========================== Global state and defines ====================== */

struct emustate {
    zx_t zx;    // The emulator state.
    int debug;  // Debugging mode

    uint32_t tick; // Frame number since last game load.

    // Cursor keys act as the Kempston joystick (0, default) or as the
    // Spectrum cursor keys / Caps Shift+5-8 (1). Toggled from the menu.
    uint32_t cursor_as_keys;

    // Is the game selection / config menu shown?
    int menu_active;
    int about_active;           // "About" pop-over shown (modal over the menu).
    uint32_t menu_left_at_tick; // EMU.tick when the menu was closed.
    int selected_game;          // Game index of currently selected game in
                                // the UI. If less than 0 a settings item is
                                // selected instead.
    int loaded_game;            // Game index of the game currently loaded.
    uint32_t show_border;       // If 0, Spectrum border is not drawn.
    uint32_t scaling;           // Spectrum -> display scaling factor.
    uint32_t brightness;        // Display brightness.
    uint32_t partial_update;    // Display partial update true/false.

    // Audio related
    uint32_t volume;            // Audio volume. Controls PWM value.
    volatile uint32_t audio_sample_wait; // Wait time (in busy loop cycles)
                                         // between samples when playing back.

    // All our UI graphic primitives are automatically cropped
    // to the area selected by ui_set_crop_area().
    uint16_t ui_crop_x1, ui_crop_x2, ui_crop_y1, ui_crop_y2;

    uint8_t dirty_vram[24]; // Track rows that changed since last update.
    uint8_t last_update_border_color; // Track last border color to update the
                                      // screen border only if it changed.

    // Speed sampling, averaged over a ~1 s window by zxemu_render_frame()
    // and reported in the About pop-over.
    uint32_t perf_exec_us;   // Mean zx_exec() time per emulated frame.
    uint32_t perf_disp_us;   // Mean display path (overlay + decode) time.
    uint32_t perf_pub_us;    // Mean fb_publish() (transpose+wait+copy) time.
    uint32_t perf_fps_x10;   // Achieved frames per second, in tenths.
} EMU;

// Emulated microseconds to ask zx_exec() for, for one displayed frame.
// zx_exec() always runs on to the end of the bitmap (scanline 256), so
// any request longer than one scanline and shorter than 256 of them
// advances exactly one emulated frame per call; 200 scanlines sits in
// the middle of that window for every scanline_period the menu allows.
// (Full history and the upstream contrast: AGENTS.md "Frame pacing".)
static inline uint32_t zx_frame_usec(void) {
    return (uint32_t)(200ull * (uint32_t)EMU.zx.scanline_period * 1000000ull /
                      EMU.zx.freq_hz);
}

/* ========================== Emulator user interface ======================= */

// Numerical parameters that it is possible to change using the
// user interface.

#define UI_EVENT_NONE 0         // No key pressed while in menu mode.
#define UI_EVENT_LOADGAME 1     // New game loaded.
#define UI_EVENT_CLOCK 2        // Clock speed changed.
#define UI_EVENT_BORDER 3       // Display border option toggled.
#define UI_EVENT_SCALING 4      // Display scaling modified.
#define UI_EVENT_VOLUME 5       // Volume modified.
#define UI_EVENT_SYNC 6         // Audio sync wait time modified.
#define UI_EVENT_BRIGHTNESS 7   // Display brightness modified.
#define UI_EVENT_PARTIAL 8      // Display partial update toggled.
#define UI_EVENT_EXIT 9         // "Exit" action item selected.
#define UI_EVENT_ABOUT 10       // "About" action item selected.
#define UI_EVENT_NAVIGATION 254 // Just moving around in the menu.
#define UI_EVENT_DISMISS 255    // Menu dismissed.

const uint32_t CursorValues[] = {0, 1};
const char *CursorValuesNames[] = {"stick", "keys", NULL};
struct UISettingsItem {
    uint32_t event;     // Event reported if setting is changed.
    const char *name;   // Name of the setting.
    uint32_t *ptr;      // Pointer to the variable of the setting.
    uint32_t step;      // Incremnet/decrement pressing right/left.
    uint32_t min;       // Minimum value alllowed.
    uint32_t max;       // Maximum value allowed.
    const uint32_t *values; // If not NULL, discrete values the variable can
                            // assume.
    const char **values_names; // If not NULL, the name to display for the
                               // values array. If values is defined, this
                               // must be defined as well.
} SettingsList[] = {
    // Left/right adjust the value; items with a NULL ptr are actions
    // triggered by fire (Exit). Only settings that do something here.
    {UI_EVENT_VOLUME,
        "volume", &EMU.volume, 1, 0, 20, NULL, NULL},
    {UI_EVENT_BORDER,
        "border", &EMU.show_border, 1, 0, 1, NULL, NULL},
    {UI_EVENT_NONE,
        "cursor", &EMU.cursor_as_keys, 1, 0, 1, CursorValues, CursorValuesNames},
    {UI_EVENT_NONE,
        "scan-p", (uint32_t*)&EMU.zx.scanline_period, 1, 10, 500, NULL, NULL},
    {UI_EVENT_ABOUT,
        "about", NULL, 0, 0, 0, NULL, NULL},
    {UI_EVENT_EXIT,
        "exit", NULL, 0, 0, 0, NULL, NULL},
};

#define SettingsListLen (sizeof(SettingsList)/sizeof(SettingsList[0]))

// Convert the setting 'id' name and current value into a string
// to show as menu item.
void settings_to_string(char *buf, size_t buflen, int id) {
    if (SettingsList[id].ptr == NULL) {
        // Action item (e.g. Exit) -- just the name, no value.
        snprintf(buf, buflen, "%s", SettingsList[id].name);
    } else if (SettingsList[id].values == NULL) {
        snprintf(buf,buflen,"%s:%u",
            SettingsList[id].name,
            SettingsList[id].ptr[0]);
    } else {
        int j = 0;
        while(SettingsList[id].values_names[j]) {
            if (SettingsList[id].values[j] == SettingsList[id].ptr[0])
                break;
            j++;
        }
        snprintf(buf,buflen,"%s:%s",
            SettingsList[id].name,
            SettingsList[id].values_names[j] ?
            SettingsList[id].values_names[j] : "?");
    }
}

// Change the specified setting ID value to the next/previous
// value. If we are already at the min or max value, nothing is
// done.
//
// 'dir' shoild be 1 (next value) or -1 (previous value).
uint32_t settings_change_value(int id, int dir) {
    struct UISettingsItem *si = SettingsList+id;
    if (si->ptr == NULL) {
        return si->event;   // action item -- nothing to adjust
    } else if (si->values == NULL) {
        if (si->ptr[0] == si->min && dir == -1) return UI_EVENT_NONE;
        else if (si->ptr[0] == si->max && dir == 1) return UI_EVENT_NONE;
        si->ptr[0] += si->step * dir;
        if (si->ptr[0] < si->min) si->ptr[0] = si->min;
        else if (si->ptr[0] > si->max) si->ptr[0] = si->max;
    } else {
        int j = 0;
        while (si->values_names[j]) {
            if (si->values[j] == si->ptr[0]) break;
            j++;
        }

        // In case of non standard value found, recover
        // setting the first valid value.
        if (si->values_names[j] == NULL) {
            j = 0;
            si->ptr[0] = si->values[0];
        }
        
        if (j == 0 && dir == -1) return UI_EVENT_NONE;
        if (si->values_names[j+1] == NULL && dir == 1) return UI_EVENT_NONE;
        j += dir;
        si->ptr[0] = si->values[j];
    }
    return si->event;
}

// Set the draw window of the ui_* functions. This is useful in order
// to limit drawing the menu inside its area, without doing too many
// calculations about font sizes and such.
void ui_set_crop_area(uint16_t x1, uint16_t x2, uint16_t y1, uint16_t y2) {
    EMU.ui_crop_x1 = x1;
    EMU.ui_crop_x2 = x2;
    EMU.ui_crop_y1 = y1;
    EMU.ui_crop_y2 = y2;
}

// Allow to draw everywhere on the screen. Called after we finished
// updating a specific area to restore the normal state.
void ui_reset_crop_area(void) {
    ui_set_crop_area(0,st77_width-1,0,st77_height-1);
}

// This function writes a box (with the specified border, if given) directly
// inside the ZX Spectrum CRT framebuffer. We use this primitive to draw our
// UI, this way when we refresh the emulator framebuffer copying it to our
// phisical display, the UI is also rendered.
//
// bcolor and color are from 0 to 15, and use the Spectrum palette (sorry :D).
// bcolor is the color of the border. If you don't want a border, just use
// bcolor the same as color.
void ui_fill_box(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color, uint8_t bcolor) {
    uint16_t x2 = x+width-1;
    uint16_t y2 = y+height-1;
    uint8_t *vmem = EMU.zx.ram[EMU.zx.display_ram_bank];

    for (int py = y; py <= y2; py++) {
        for (int px = x; px <= x2; px++) {
            // Don't draw outside the current mask.
            if (px < EMU.ui_crop_x1 ||
                px > EMU.ui_crop_x2 ||
                py < EMU.ui_crop_y1 ||
                py > EMU.ui_crop_y2) continue;

            // Border or inside?
            uint8_t c = (px==x || px==x2 || py==y || py==y2) ? bcolor : color;

            // Write directly into the Spectrum VMEM
            if (px >= 256 || py >= 192) continue; // VMEM limits.
            uint32_t vbyte = ((py & 0xC0)<<5) | ((py & 0x07)<<8) |
                             ((py & 0x38)<<2) | (px>>3);
            uint32_t bit = 1<<(7-(px&7));
            vmem[vbyte] = vmem[vbyte] & (0xff^bit);
            if (c != 0) vmem[vbyte] |= bit;
        }
    }
}

// Draw a character on the screen.
// We use the font in the Spectrum ROM to avoid providing one.
// Size is the size multiplier.
void ui_draw_char(uint16_t px, uint16_t py, uint8_t c, uint8_t color, uint8_t size) {
    c -= 0x20; // The Spectrum ROM font starts from ASCII 0x20 char.
    uint8_t *font = dump_amstrad_zx48k_bin+0x3D00;
    for (int y = 0; y < 8; y++) {
        uint32_t row = font[c*8+y];
        for (int x = 0; x < 8; x++) {
            if (row & 0x80)
                ui_fill_box(px+x*size,py+y*size,size,size,color,color);
            row <<= 1;
        }
    }
}

// Draw the string 's' using the ROM font by calling ui_draw_char().
// Size is the font size multiplier. 1 = 8x8 font, 2 = 16x16, ...
void ui_draw_string(uint16_t px, uint16_t py, const char *s, uint8_t color, uint8_t size) {
    while (*s) {
        ui_draw_char(px,py,s[0],color,size);
        s++;
        px += 8*size;
    }
}

// Load the prev/next game in the list (dir = -1 / 1).
void ui_go_next_prev_game(int dir) {
    EMU.selected_game += dir;
    if (EMU.selected_game == -SettingsListLen-1) {
        EMU.selected_game = GamesTableSize-1;
    } else if (EMU.selected_game == GamesTableSize) {
        EMU.selected_game = -SettingsListLen;
    }
}

// Fire / Enter / Space on the highlighted item: load a game and drop into
// play, or fire an action (Exit). Settings items do nothing on select.
static void ui_menu_select(void) {
    if (EMU.selected_game >= 0) {
        load_game(EMU.selected_game);
        EMU.menu_active = 0;
        EMU.menu_left_at_tick = EMU.tick;
    } else {
        struct UISettingsItem *si = &SettingsList[-EMU.selected_game - 1];
        if (si->event == UI_EVENT_EXIT) ikbd_request_boot_gem();
        else if (si->event == UI_EVENT_ABOUT) EMU.about_active = 1;
    }
}

// Left/right on a settings item adjusts its value.
static void ui_menu_adjust(int dir) {
    if (EMU.selected_game < 0)
        settings_change_value(-EMU.selected_game - 1, dir);
}

// If the menu is active, draw it into the Spectrum VRAM. Cell-aligned
// (each 8 px row = one attribute cell row) so items can be coloured
// independently without Spectrum attribute clash: games in white, the
// non-file items (settings / Exit) in bright cyan, the highlighted row
// inverted (solid colour block, black text).
void ui_draw_menu(void) {
    const int menu_x = 152;             // cell-aligned (19 * 8)
    const int menu_w = 256 - menu_x;    // to the right screen edge
    const int menu_y = 0;
    const int rows   = 16;              // 16 * 8 = 128 px tall
    const int menu_h = rows * 8;

    uint8_t *vmem = EMU.zx.ram[EMU.zx.display_ram_bank];

    // Clear the menu region bitmap so the game behind doesn't show through.
    ui_fill_box(menu_x, menu_y, menu_w, menu_h, 0, 0);

    // Scroll so the highlighted item stays roughly centred.
    int first = (int)EMU.selected_game - rows / 2;
    if (first < -(int)SettingsListLen) first = -(int)SettingsListLen;

    ui_set_crop_area(menu_x, 255, menu_y, menu_y + menu_h - 1);

    int y = menu_y;
    for (int j = first; y < menu_y + menu_h; j++, y += 8) {
        if (j >= (int)GamesTableSize) break;
        const bool is_game = (j >= 0);
        const bool sel = (j == EMU.selected_game);

        // Ink colour: games white (7), non-file items bright cyan (13).
        const uint8_t ink = is_game ? 7 : 13;
        const uint8_t attr = (uint8_t)((ink & 7) | ((ink & 8) ? 0x40 : 0));
        const int cy = y >> 3;
        for (int cx = menu_x >> 3; cx < 32; cx++)
            vmem[0x1800 + (cy << 5) + cx] = attr;

        // Build the display text (game name with any ".z80"/".sna" hidden).
        char line[40];
        if (is_game) {
            strncpy(line, GamesTable[j].name, sizeof(line) - 1);
            line[sizeof(line) - 1] = 0;
            size_t n = strlen(line);
            if (snapshot_type(line)) line[n-4] = 0;
        } else {
            settings_to_string(line, sizeof(line), -j - 1);
        }

        if (sel) {
            // Inverted highlight: solid colour block with black text.
            ui_fill_box(menu_x, y, menu_w, 8, 1, 1);
            ui_draw_string(menu_x + 1, y, line, 0, 1);
        } else {
            ui_draw_string(menu_x + 1, y, line, 1, 1);
        }
    }
    ui_reset_crop_area();
}

// "About" pop-over: version + credits, centred over the screen. Opened from
// the About menu item and dismissed by any key / joystick press. Cell-aligned
// (8 px grid) so the title can be tinted bright cyan and the body left white.
static const char *AboutLines[] = {
    "MD/Speccy " RELEASE_VERSION,
    "",
    "ZX Spectrum 48K emulator",
    "by Neil Rackett",
    "neilrackett.com/atarist",
    "",
    NULL,   // speed readout, filled in below
    NULL,   // display/publish breakdown, filled in below
};
#define ABOUT_NLINES ((int)(sizeof(AboutLines) / sizeof(AboutLines[0])))

// Backing store for the two patched-in stats lines above.
static char AboutStats[2][28];

// Format a microsecond count as "N.M" milliseconds (two printf args).
#define MS(us) (unsigned long)((us) / 1000u), (unsigned long)(((us) % 1000u) / 100u)

void ui_draw_about(void) {
    const int box_cw = 26;                       // width in cells (chars)
    const int box_x  = ((32 - box_cw) / 2) * 8;  // centred, cell-aligned
    const int box_w  = box_cw * 8;
    const int rows   = ABOUT_NLINES + 2;         // one padding row top + bottom
    const int box_h  = rows * 8;
    const int box_y  = ((24 - rows) / 2) * 8;    // centred, cell-aligned

    uint8_t *vmem = EMU.zx.ram[EMU.zx.display_ram_bank];
    ui_set_crop_area(box_x, box_x + box_w - 1, box_y, box_y + box_h - 1);

    // Opaque black interior + white border (bitmap only; colours come from
    // the attribute cells set below).
    ui_fill_box(box_x, box_y, box_w, box_h, 0, 1);

    const int cx0 = box_x >> 3, cx1 = (box_x + box_w) >> 3, cy0 = box_y >> 3;
    for (int cy = cy0; cy < cy0 + rows; cy++)
        for (int cx = cx0; cx < cx1; cx++)
            vmem[0x1800 + (cy << 5) + cx] = 0x07;   // white ink, black paper

    // Title row bright cyan (13 = ink 5 | bright).
    for (int cx = cx0; cx < cx1; cx++)
        vmem[0x1800 + ((cy0 + 1) << 5) + cx] = 0x45;

    // Live speed readout: emulation time per frame against the 20 ms PAL
    // VBL budget, and the frame rate actually achieved. Both are sampled in
    // zxemu_render_frame() and include this pop-over's own compositing
    // cost, so they read a little below the in-play figures.
    snprintf(AboutStats[0], sizeof(AboutStats[0]), "emu %lu.%lums  fps %lu.%lu",
             MS(EMU.perf_exec_us),
             (unsigned long)(EMU.perf_fps_x10 / 10u),
             (unsigned long)(EMU.perf_fps_x10 % 10u));
    snprintf(AboutStats[1], sizeof(AboutStats[1]), "dsp %lu.%lums  pub %lu.%lums",
             MS(EMU.perf_disp_us), MS(EMU.perf_pub_us));
    AboutLines[ABOUT_NLINES - 2] = AboutStats[0];
    AboutLines[ABOUT_NLINES - 1] = AboutStats[1];

    for (int i = 0; i < ABOUT_NLINES; i++) {
        const char *s = AboutLines[i];
        int len = (int)strlen(s);
        if (len == 0) continue;
        int tx = box_x + (box_w - len * 8) / 2;     // centre the line
        ui_draw_string(tx, box_y + (i + 1) * 8, s, 1, 1);
    }
    ui_reset_crop_area();
}

/* =========================== Emulator implementation ====================== */

// Set a bitmap signaling which part of the screen RAM was touched and
// is yet to be updated on the display. This function is called when the
// address 'addr' is in the range of the VRAM bitmap area.
void __not_in_flash_func(vram_set_dirty_bitmap)(uint16_t addr) {
    uint16_t y = ((addr&0x1800)>>5) | ((addr&0x700)>>8) | ((addr&0xe0)>>2);
    EMU.dirty_vram[y>>3] |= 1 << (y&7);
}

// Like vram_set_dirty_bitmap() but called for addresses in the range
// of the color attributes.
void __not_in_flash_func(vram_set_dirty_attr)(uint16_t addr) {
    // Mark all the 8 rows affected in one operation.
    EMU.dirty_vram[((addr-0x5800)>>5) & 31] = 0xff;
}

// Clean the bitmap of modified scanlines.
void vram_reset_dirty(void) {
    memset(EMU.dirty_vram,0,sizeof(EMU.dirty_vram));
}

// Set the bitmap of modified scanlines to all ones: this way
// the update_display() function will be forced to do a full
// refresh, border included.
void vram_force_dirty(void) {
    memset(EMU.dirty_vram,0xff,sizeof(EMU.dirty_vram));
    EMU.last_update_border_color = 0xff; // Impossible color: update forced.
}

// ZX Spectrum palette to RGB565 conversion. We do it at startup to avoid
// burning CPU cycles later.
// Push the 16-entry ZX Spectrum palette to the Atari ST shifter
// palette. zxpalette entries are stored 0x00BBGGRR (red in the low
// byte); the ST shifter palette takes 3-bit-per-channel values.
void zx_set_palette(void) {
    for (int i = 0; i < 16; i++) {
        uint32_t c = zxpalette[i];
        palette_set_entry(i,
            PALETTE_RGB((c & 0xff) >> 5,
                        ((c >> 8) & 0xff) >> 5,
                        ((c >> 16) & 0xff) >> 5));
    }
}

// Nibble -> byte-lane mask for the decode below: bit 3 of the nibble
// (the leftmost of its 4 pixels) selects byte lane 0 of a little-endian
// word. Validated offline against the byte-wise loop it replaced, over
// every (bits, attr, blink) combination.
static const uint32_t zx_nib_mask[16] = {
    0x00000000u, 0xFF000000u, 0x00FF0000u, 0xFFFF0000u,
    0x0000FF00u, 0xFF00FF00u, 0x00FFFF00u, 0xFFFFFF00u,
    0x000000FFu, 0xFF0000FFu, 0x00FF00FFu, 0xFFFF00FFu,
    0x0000FFFFu, 0xFF00FFFFu, 0x00FFFFFFu, 0xFFFFFFFFu,
};

// Decode the 256x192 Spectrum bitmap + attribute VRAM into the
// template's chunked framebuffer, one palette-index byte per pixel,
// centred at (ZX_FB_X0, ZX_FB_Y0). The surrounding margin is the
// border colour. Runs every VBL; the m68k blits the result to the ST.
//
// Incremental: only rows flagged in EMU.dirty_vram (maintained by the
// mem.h write hooks) are re-decoded, and the border/canvas is cleared
// only when the border colour changes -- callers that bypass the write
// hooks (menu overlay, snapshot loads, FLASH blink) must call
// vram_force_dirty() first. Each attribute cell is written as two
// 32-bit stores via zx_nib_mask (fb_chunked_buffer is 4-byte aligned
// and ZX_FB_X0 / FB_CHUNKED_W keep every row start aligned).
#pragma GCC push_options
#pragma GCC optimize("O2")
void __not_in_flash_func(update_display)(void) {
    const uint8_t *vmem = EMU.zx.ram[EMU.zx.display_ram_bank];
    const uint8_t border = EMU.show_border ? EMU.zx.border_color : 0;

    // Border changed (or vram_force_dirty's 0xff sentinel): repaint the
    // whole canvas; the row loop below then rebuilds the bitmap area.
    bool full = false;
    if (border != EMU.last_update_border_color) {
        fb_chunked_clear(border);
        EMU.last_update_border_color = border;
        full = true;
    }

    for (int py = 0; py < 192; py++) {
        if (!full && !(EMU.dirty_vram[py >> 3] & (1u << (py & 7)))) continue;
        const uint8_t *row = vmem +
            (((py & 0xC0) << 5) | ((py & 0x07) << 8) | ((py & 0x38) << 2));
        const uint8_t *attrrow = vmem + 0x1800 + ((py >> 3) << 5);
        uint32_t *dst = (uint32_t *)(fb_chunked_buffer +
            (ZX_FB_Y0 + py) * FB_CHUNKED_W + ZX_FB_X0);

        for (int bx = 0; bx < 32; bx++) {
            uint8_t bits = row[bx];
            uint8_t attr = attrrow[bx];
            uint8_t bright = (attr & 0x40) >> 3;    // 0 or 8
            uint8_t ink = (attr & 7) | bright;
            uint8_t paper = ((attr >> 3) & 7) | bright;
            if ((attr & 0x80) && zx_blink_phase) {  // FLASH: swap ink/paper
                uint8_t t = ink; ink = paper; paper = t;
            }
            const uint32_t inkw = ink * 0x01010101u;
            const uint32_t paperw = paper * 0x01010101u;
            const uint32_t mhi = zx_nib_mask[bits >> 4];
            const uint32_t mlo = zx_nib_mask[bits & 15u];
            *dst++ = (inkw & mhi) | (paperw & ~mhi);
            *dst++ = (inkw & mlo) | (paperw & ~mlo);
        }
    }

    vram_reset_dirty();
}
#pragma GCC pop_options

// Clear all keys. Useful when we switch game, to make sure that no
// key downs are left from a previous game.
void flush_zx_key_press(zx_t *zx) {
    for (int j = 0; j < KBD_MAX_KEYS; j++) zx_key_up(zx,j);
    zx_cursor_kempston = 0;
    zx_joystick(zx, 0);
}

// Set the audio volume by altering the PWM counter wrap value.
// The zx.h file will always set the channel level to 1 or 0
// (Z80 audio pin high or low), so the greater the counter value
// the smaller the volume.
// Audio volume is applied by the YM fill callback (there is no PWM on
// this target), so this is just a store for the settings menu.
void set_volume(uint32_t volume) {
    (void)volume;
}

/* =============================== Game storage ============================= */

// Case-insensitive snapshot extension test (FatFs may hand back either an
// LFN with original case or an upper-case 8.3 short name). Both supported
// extensions are 4 chars, so stripping one is always name[n-4] = 0.
static int snapshot_type(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return SNAP_NONE;
    const char *e = name + n - 4;
    if (e[0] != '.') return SNAP_NONE;
    if ((e[1] == 'z' || e[1] == 'Z') && e[2] == '8' && e[3] == '0')
        return SNAP_Z80;
    if ((e[1] == 's' || e[1] == 'S') &&
        (e[2] == 'n' || e[2] == 'N') &&
        (e[3] == 'a' || e[3] == 'A'))
        return SNAP_SNA;
    return SNAP_NONE;
}

// Load a 48K .sna snapshot: a 27-byte register header followed by a raw
// 48 KB dump of $4000-$FFFF (no compression). PC is not in the header --
// the save pushed it onto the stack, so loading emulates RETN: pop PC,
// SP += 2, IFF2 -> IFF1. 128K .sna images are a different size and are
// rejected (this is a 48K machine).
static bool zx_quickload_sna(zx_t *sys, chips_range_t data) {
    if (data.size != 27 + 0xC000) return false;
    const uint8_t *h = data.ptr;
    memcpy(sys->ram[0], h + 27, 0x4000);            // $4000
    memcpy(sys->ram[1], h + 27 + 0x4000, 0x4000);   // $8000
    memcpy(sys->ram[2], h + 27 + 0x8000, 0x4000);   // $C000
    z80_reset(&sys->cpu);
    sys->cpu.i   = h[0];
    sys->cpu.hl2 = (h[2] << 8) | h[1];
    sys->cpu.de2 = (h[4] << 8) | h[3];
    sys->cpu.bc2 = (h[6] << 8) | h[5];
    sys->cpu.af2 = (h[8] << 8) | h[7];
    sys->cpu.l = h[9];  sys->cpu.h = h[10];
    sys->cpu.e = h[11]; sys->cpu.d = h[12];
    sys->cpu.c = h[13]; sys->cpu.b = h[14];
    sys->cpu.iy = (h[16] << 8) | h[15];
    sys->cpu.ix = (h[18] << 8) | h[17];
    sys->cpu.iff1 = sys->cpu.iff2 = (h[19] & (1 << 2)) != 0;
    sys->cpu.r = h[20];
    sys->cpu.f = h[21]; sys->cpu.a = h[22];
    uint16_t sp = (h[24] << 8) | h[23];
    sys->cpu.im = h[25] & 3;
    uint16_t pc = mem_rd(&sys->mem, sp) |
                  (mem_rd(&sys->mem, (uint16_t)(sp + 1)) << 8);
    sys->cpu.sp = (uint16_t)(sp + 2);
    sys->pins = z80_prefetch(&sys->cpu, pc);
    sys->border_color = h[26] & 7;
    return true;
}

// SD app folder (from per-app config, default "/speccy").
static char zx_folder[64] = "/speccy";

// Enumerate the app folder for .z80 / .sna snapshots.
// Returns the number of games found.
int populate_games_list(void) {
    SettingsConfigEntry *fe =
        settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
    if (fe && fe->value[0]) {
        strncpy(zx_folder, fe->value, sizeof(zx_folder) - 1);
        zx_folder[sizeof(zx_folder) - 1] = 0;
    }

    GamesTableSize = 0;
    DIR dir;
    if (f_opendir(&dir, zx_folder) != FR_OK) return 0;

    FILINFO fno;
    while (GamesTableSize < ZX_MAX_GAMES &&
           f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        if (fno.fname[0] == '.') continue;   // skip dot / AppleDouble (._*.z80)
        if (fno.fattrib & (AM_HID | AM_SYS)) continue;
        if (!snapshot_type(fno.fname)) continue;
        struct game_entry *ge = &GamesTable[GamesTableSize++];
        strncpy(ge->name, fno.fname, sizeof(ge->name) - 1);
        ge->name[sizeof(ge->name) - 1] = 0;
        ge->addr = NULL;
        ge->size = (uint32_t)fno.fsize;
    }
    f_closedir(&dir);

    // Make sure the embedded games are always present in the folder,
    // regardless of what else is there. FA_CREATE_NEW fails harmlessly if
    // one already exists (FAT names are case-insensitive), in which case
    // the scan above already listed it; otherwise we write it and add it.
    // They're normal files the user can delete -- they just reappear next
    // boot.
    for (unsigned i = 0; i < ZX_BUILTIN_GAMES && GamesTableSize < ZX_MAX_GAMES;
         i++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", zx_folder, BuiltinGames[i].name);
        FIL f;
        if (f_open(&f, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) continue;
        UINT bw = 0;
        f_write(&f, BuiltinGames[i].data, BuiltinGames[i].size, &bw);
        f_close(&f);
        if (bw == BuiltinGames[i].size) {
            struct game_entry *ge = &GamesTable[GamesTableSize++];
            strncpy(ge->name, BuiltinGames[i].name, sizeof(ge->name) - 1);
            ge->name[sizeof(ge->name) - 1] = 0;
            ge->addr = NULL;
            ge->size = BuiltinGames[i].size;
        }
    }
    return (int)GamesTableSize;
}

// Bring up the emulator: defaults, ST palette, and the Z80 core.
// Replaces upstream init_emulator() minus all the Pico display / GPIO /
// PWM / overclock setup, which the template owns.
void init_emulator(void) {
    EMU.debug = 0;
    vram_force_dirty();  // first decode is a full pass over the boot frame
    EMU.menu_active = 1;
    EMU.about_active = 0;
    EMU.tick = 0;
    EMU.cursor_as_keys = 0;    // cursor keys default to Kempston joystick
    EMU.selected_game = 0;
    EMU.loaded_game = -1;
    EMU.show_border = DEFAULT_DISPLAY_BORDERS;
    EMU.scaling = DEFAULT_DISPLAY_SCALING;
    EMU.volume = 20;
    EMU.brightness = ST77_MAX_BRIGHTNESS;
    EMU.partial_update = DEFAULT_DISPLAY_PARTIAL_UPDATE;
    EMU.audio_sample_wait = 300;
    ui_reset_crop_area();

    // We own the ESC key: it toggles the menu. Exit to GEM is triggered
    // from the menu's Exit item instead (ikbd_request_boot_gem).
    ikbd_set_esc_auto_exit(false);

    // Publish the ZX Spectrum palette to the Atari ST shifter.
    zx_set_palette();

    // Emulator core: 48K, Kempston joystick.
    zx_desc_t zx_desc = {0};
    zx_desc.type = ZX_TYPE_48K;
    zx_desc.joystick_type = ZX_JOYSTICKTYPE_KEMPSTON;
    zx_desc.roms.zx48k.ptr = (void *)dump_amstrad_zx48k_bin;
    zx_desc.roms.zx48k.size = sizeof(dump_amstrad_zx48k_bin);
    zx_desc.audiobuf = zx_audiobuf_store;
    zx_init(&EMU.zx, &zx_desc);
    EMU.zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;

    // Register a standalone Caps Shift key at (column 0, line 0). Upstream
    // registers it only as a modifier; the full-keyboard mapping needs to
    // press it on its own for ST Shift. Sym Shift already has code 0x0F.
    kbd_register_key(&EMU.zx.kbd, ZX_KEY_CAPS, 0, 0, 0);

    // Caps Lock = Caps Shift + 2. No standalone code upstream, so register
    // one at the '2' cell (column 3, line 1) with the Caps Shift modifier
    // (mod 0), so pressing it holds both cells at once.
    kbd_register_key(&EMU.zx.kbd, ZX_KEY_CAPSLK, 3, 1, 1);
}

/* Load the specified game ID (index into GamesTable). */
void load_game(int game_id) {
    if (game_id < 0 || game_id >= (int)GamesTableSize) return;
    struct game_entry *g = &GamesTable[game_id];

    flush_zx_key_press(&EMU.zx);   // No keys held across a game switch.
    EMU.tick = 0;

    if (EMU.loaded_game != game_id)
        EMU.zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;

    // Borrow the chunked framebuffer as a transient load buffer -- it is
    // overwritten by the next update_display() anyway, so this avoids a
    // permanent ~50 KB allocation for the snapshot image.
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", zx_folder, g->name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return;
    uint32_t cap = FB_CHUNKED_SIZE;
    if (g->size && g->size < cap) cap = g->size;
    UINT br = 0;
    f_read(&f, fb_chunked_buffer, cap, &br);
    f_close(&f);
    chips_range_t r = { .ptr = fb_chunked_buffer, .size = br };
    if (snapshot_type(g->name) == SNAP_SNA)
        zx_quickload_sna(&EMU.zx, r);
    else
        zx_quickload(&EMU.zx, r);

    EMU.loaded_game = game_id;
    vram_force_dirty();  // snapshot rewrote VRAM behind the mem.h hooks,
                         // and the load borrowed fb_chunked_buffer
}

/* ============================ Public entry points ========================= */

void zxemu_init(void) {
    init_emulator();

    // Populate the games list (SD scan + embedded demo, or the baked-in
    // game). No game is auto-loaded: init_emulator() leaves the menu
    // active, so the user always starts in the picker and chooses what to
    // run. Until they do, the ZX ROM boot screen renders behind the menu.
    populate_games_list();
}

// Handle one ST key event. ESC toggles the menu; while the menu is up the
// cursor cluster drives navigation; in play everything maps onto the
// Spectrum keyboard, with the cursor cluster acting as a Kempston joystick
// (or the Spectrum cursor keys, per EMU.cursor_as_keys).
void zxemu_handle_key(const ikbd_key_event_t *k) {
#if ZX_INPUT_KEYBOARD
    const uint8_t sc = k->scancode;
    const bool press = k->is_press;

    // The About pop-over is modal: any key press dismisses it and is
    // swallowed (so ESC etc. close the pop-over rather than acting).
    if (EMU.about_active) {
        if (press) EMU.about_active = 0;
        return;
    }

    // ESC toggles the game / settings menu. We own the ESC key here
    // (auto-exit is disabled at init); exit to GEM is the menu's Exit item.
    if (sc == ST_SC_ESC) {
        if (press) {
            EMU.menu_active = !EMU.menu_active;
            if (!EMU.menu_active) EMU.menu_left_at_tick = EMU.tick;
        }
        return;
    }

    // Menu mode: the keyboard drives navigation, nothing reaches the ZX.
    if (EMU.menu_active) {
        if (!press) return;
        switch (sc) {
            case ST_SC_UP:      ui_go_next_prev_game(-1); break;
            case ST_SC_DOWN:    ui_go_next_prev_game(1);  break;
            case ST_SC_LEFT:    ui_menu_adjust(-1); break;
            case ST_SC_RIGHT:   ui_menu_adjust(1);  break;
            case ST_SC_ENTER:
            case ST_SC_SPACE:
            case ST_SC_INSERT:
            case ST_SC_CLRHOME: ui_menu_select(); break;
            default: break;
        }
        return;
    }

    // Play mode. The cursor cluster is the Kempston joystick by default, or
    // the Spectrum cursor keys (Caps Shift + 5..8) when EMU.cursor_as_keys.
    switch (sc) {
        case ST_SC_UP: case ST_SC_DOWN: case ST_SC_LEFT: case ST_SC_RIGHT:
            if (EMU.cursor_as_keys) {
                uint8_t code = (sc == ST_SC_UP)   ? ZX_KEY_CUP   :
                               (sc == ST_SC_DOWN) ? ZX_KEY_CDOWN :
                               (sc == ST_SC_LEFT) ? ZX_KEY_CLEFT : ZX_KEY_CRIGHT;
                if (press) zx_key_down(&EMU.zx, code);
                else       zx_key_up(&EMU.zx, code);
            } else {
                uint8_t bit = (sc == ST_SC_UP)   ? ZX_JOYSTICK_UP   :
                              (sc == ST_SC_DOWN) ? ZX_JOYSTICK_DOWN :
                              (sc == ST_SC_LEFT) ? ZX_JOYSTICK_LEFT : ZX_JOYSTICK_RIGHT;
                if (press) zx_cursor_kempston |=  bit;
                else       zx_cursor_kempston &= ~bit;
            }
            return;
        case ST_SC_INSERT:
        case ST_SC_CLRHOME:
            // Both keys are fire (Kempston button) -- but only in the
            // joystick cursor mode. In "keys" mode the cursors are Spectrum
            // keys and there's no Kempston, so a press does nothing. Release
            // always clears, so the bit can't stick if the mode changed
            // while it was held.
            if (press) {
                if (!EMU.cursor_as_keys) zx_cursor_kempston |= ZX_JOYSTICK_BTN;
            } else {
                zx_cursor_kempston &= ~ZX_JOYSTICK_BTN;
            }
            return;
        default:
            break;
    }

    // Everything else maps positionally onto the Spectrum keyboard.
    uint8_t zk = st2zx[sc & 0x7f];
    if (zk == 0) return;
    if (press) zx_key_down(&EMU.zx, zk);
    else       zx_key_up(&EMU.zx, zk);
#else
    (void)k;
#endif
}

void zxemu_render_frame(void) {
    uint8_t joy = ikbd_get_joystick();  // bit0 up,1 down,2 left,3 right,7 fire
    static uint8_t prev_joy = 0;

    if (EMU.about_active) {
        // Modal pop-over: any joystick press dismisses it (keyboard is
        // handled per-event in zxemu_handle_key).
        if (joy & (uint8_t)~prev_joy) EMU.about_active = 0;
    } else if (EMU.menu_active) {
        // Drive the menu from the physical ST joystick too, edge-triggered
        // so one push is one step (the keyboard path is per-event above).
        uint8_t edge = joy & (uint8_t)~prev_joy;
        if (edge & 0x01) ui_go_next_prev_game(-1);   // up
        if (edge & 0x02) ui_go_next_prev_game(1);    // down
        if (edge & 0x04) ui_menu_adjust(-1);         // left
        if (edge & 0x08) ui_menu_adjust(1);          // right
        if (edge & 0x80) ui_menu_select();           // fire
    } else {
        // Play: Kempston = cursor-key contribution | physical ST joystick.
        uint8_t kemp = zx_cursor_kempston;
        // Ignore the physical joystick for a few frames after the menu
        // closes so the fire that picked a game doesn't leak into play.
        if (EMU.tick >= EMU.menu_left_at_tick + 10) {
            if (joy & 0x01) kemp |= ZX_JOYSTICK_UP;
            if (joy & 0x02) kemp |= ZX_JOYSTICK_DOWN;
            if (joy & 0x04) kemp |= ZX_JOYSTICK_LEFT;
            if (joy & 0x08) kemp |= ZX_JOYSTICK_RIGHT;
            if (joy & 0x80) kemp |= ZX_JOYSTICK_BTN;
        }
        zx_joystick(&EMU.zx, kemp);
    }
    prev_joy = joy;

    // Run the Spectrum for exactly one emulated frame, timed so the About
    // pop-over can report how much of the 20 ms PAL VBL budget the
    // emulation is eating. Accumulators (acc_*) sum over a ~1 s window,
    // then publish per-frame means into the EMU.perf_* fields About
    // reads. (The first window spans boot to first rollover -- its fps
    // reads slightly low once, which isn't worth a special case.)
    static uint32_t acc_win_us = 0, acc_exec_us = 0, acc_frames = 0;
    static uint32_t acc_disp_us = 0, acc_pub_us = 0;
    const uint32_t t0 = time_us_32();
    zx_exec(&EMU.zx, zx_frame_usec());
    const uint32_t t1 = time_us_32();
    acc_exec_us += t1 - t0;
    acc_frames++;
    if (t1 - acc_win_us >= 1000000u) {
        const uint32_t span = t1 - acc_win_us;
        EMU.perf_exec_us = acc_exec_us / acc_frames;
        EMU.perf_disp_us = acc_disp_us / acc_frames;
        EMU.perf_pub_us = acc_pub_us / acc_frames;
        EMU.perf_fps_x10 =
            (uint32_t)(((uint64_t)acc_frames * 10000000u) / span);
        DPRINTF("emu %u disp %u pub %u us/frame, %u.%u fps\n",
                EMU.perf_exec_us, EMU.perf_disp_us, EMU.perf_pub_us,
                EMU.perf_fps_x10 / 10u, EMU.perf_fps_x10 % 10u);
        acc_win_us = t1;
        acc_exec_us = 0;
        acc_disp_us = 0;
        acc_pub_us = 0;
        acc_frames = 0;
    }

    // Toggle the FLASH phase ~twice per second (every 16 frames). The
    // decode is dirty-row incremental, so force a full pass to repaint
    // every FLASH cell.
    if ((EMU.tick & 0x0f) == 0) { zx_blink_phase ^= 1u; vram_force_dirty(); }

    // Draw the menu, or the About pop-over on top of it (the pop-over
    // replaces the menu so nothing peeks out beside it). Both draw into the
    // Spectrum VRAM, so snapshot it first and restore it after the decode:
    // the overlay is composited into the framebuffer for this frame only,
    // never left behind in VRAM (a static game screen would otherwise never
    // repaint the overwritten cells, leaving artifacts after dismissal).
    uint8_t *vram = EMU.zx.ram[EMU.zx.display_ram_bank];
    const bool overlay = EMU.about_active || EMU.menu_active;
    const uint32_t td0 = time_us_32();
    if (overlay) {
        memcpy(s_vram_save, vram, ZX_VRAM_SIZE);
        if (EMU.about_active) ui_draw_about();
        else                  ui_draw_menu();
        vram_force_dirty();   // overlay drew into VRAM behind the hooks
    }

    // Decode VRAM into the framebuffer and hand off to the ST (blocks
    // on the VBL, pacing this loop to 50 Hz).
    update_display();

    if (overlay) {
        memcpy(vram, s_vram_save, ZX_VRAM_SIZE);  // undo the overlay
        vram_force_dirty();  // next frame must repaint what it covered
    }
    const uint32_t td1 = time_us_32();
    acc_disp_us += td1 - td0;

    // Deliberately timed from outside (unlike fb_last_convert_us(),
    // which is transpose+copy only): "pub" includes the VBL wait, so an
    // unexpected stall in the pacing shows up here instead of hiding.
    fb_publish();
    acc_pub_us += time_us_32() - td1;

    EMU.tick++;
}

// round(-2*log2(q/32)) for q = 0..32: attenuation in YM volume steps
// below peak that makes linear output amplitude track a duty cycle of
// q/32 on the YM's logarithmic DAC (~3 dB, i.e. ~1/sqrt(2) amplitude,
// per step). q = 0 is silence (255 attenuates everything).
static const uint8_t duty_att[33] = {
    255, 10, 8, 7, 6, 5, 5, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2,
    2,   2,  2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
};

void zxemu_audio_fill(uint8_t *buf, uint32_t bytes) {
    // The emulator samples the 1-bit beeper into zx.audiobuf during
    // zx_exec(). Decimate the bits produced since the last fill down to
    // the ~112 samples the m68k Timer-B handler consumes per VBL.
    const uint32_t nsamp = bytes / 2;
    const uint32_t total = AUDIOBUF_LEN * 32;   // total beeper-sample bits
    static uint32_t rd = 0;                     // last consumed bit index
    uint32_t w = EMU.zx.audiobuf_byte * 32 + EMU.zx.audiobuf_bit;
    uint32_t avail = (w - rd) & (total - 1);

    // Peak YM volume follows the menu volume setting (0..20 -> 0..15).
    const uint8_t vmax = (uint8_t)((EMU.volume * 15u) / 20u);

    if (avail < nsamp) {
        // Too few fresh samples: hold the current beeper level (DC).
        uint8_t v = EMU.zx.beeper_state ? vmax : 0;
        for (uint32_t i = 0; i < nsamp; i++) { buf[2*i] = v; buf[2*i+1] = v; }
        return;
    }

    // Box-filter decimation: average the beeper bits that fall in each
    // output sample's window (the local duty cycle). The window bounds
    // tile `avail` exactly (Bresenham) -- a fixed avail/nsamp step used
    // to drop the division remainder, skipping ~0.4 ms of audio timeline
    // per fill, which phase-jumped every sustained tone 50 times a second.
    uint32_t idx = rd;
    uint32_t taken = 0;
    for (uint32_t i = 0; i < nsamp; i++) {
        const uint32_t end = (avail * (i + 1)) / nsamp;
        const uint32_t n = end - taken;
        uint32_t ones = 0;
        for (; taken < end; taken++) {
            ones += (EMU.zx.audiobuf[(idx >> 5) & (AUDIOBUF_LEN - 1)]
                     >> (idx & 31)) & 1u;
            idx++;
        }
        // Duty -> YM level via duty_att[]: the DAC is logarithmic, so a
        // linear duty*vmax mapping companded the filtered edge samples
        // into near-silence (undoing the low-pass and adding harmonics).
        // Attenuating in log steps keeps amplitude proportional to duty.
        const uint32_t q = (ones * 32u + n / 2u) / n;   // duty in 32nds
        const uint8_t att = duty_att[q];
        const uint8_t v = (att >= vmax) ? 0 : (uint8_t)(vmax - att);
        buf[2*i] = v;
        buf[2*i+1] = v;
    }
    rd = w;
}
