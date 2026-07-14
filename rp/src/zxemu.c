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
#include "zxemu.h"

#include "device_config.h" // Buttons / audio / display metrics.

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

#if ZX_GAMES_FROM_SD
#include "ff.h"
#include "sdcard.h"
#include "aconfig.h"
#include "settings/settings.h"
#endif
#include "builtin_game.h"
#include "builtin_keymaps.h"

// The Spectrum bitmap (256x192) is shown 1:1, centred in the 320x200
// framebuffer; the surrounding margin is the Spectrum border.
#define ZX_FB_X0 ((FB_CHUNKED_W - 256) / 2)   // 32
#define ZX_FB_Y0 ((FB_CHUNKED_H - 192) / 2)   // 4

#define ZX_DEFAULT_SCANLINE_PERIOD 150

// Composite gameplay button bitmask read by get_device_button() (see
// device_config.h). Recomposed each frame in zxemu_render_frame() from
// the keyboard mask and, when ZX_INPUT_JOYSTICK is built, the ST
// joystick.
uint32_t zx_input_mask = 0;
// Keyboard-driven button bits, updated by zxemu_handle_key().
static uint32_t zx_kbd_mask = 0;

// Blink phase for the Spectrum FLASH attribute, toggled by the frame
// counter in zxemu_render_frame().
static uint32_t zx_blink_phase = 0;

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

#define ZX_MAX_GAMES     128    // Max .z80 snapshots listed from SD.
#define ZX_KEYMAP_BUF_SZ 4096   // RAM buffer for the SD keymaps.txt.
#define ZX_YM_VOLUME     12     // YM 4-bit volume for beeper "high".

struct game_entry {
    char name[28];          // Z80 snapshot filename (SD) or label.
    void *addr;             // Flash/RAM address (baked-in build only).
    uint32_t size;          // Length in bytes.
};

// Populated during initialization from the SD app folder (or the single
// baked-in game). Static storage -- no heap fragmentation.
struct game_entry GamesTable[ZX_MAX_GAMES];
uint32_t GamesTableSize;

/* ============================== Keymap defines ============================ */

// Kempston joystick key codes: note that this were redefined compared to
// the original zx.h file.
#define KEMPSTONE_FIRE 0xff
#define KEMPSTONE_LEFT 0xfe
#define KEMPSTONE_RIGHT 0xfd
#define KEMPSTONE_DOWN 0xfc
#define KEMPSTONE_UP 0xfb

// "Virtual" pins.
//
// PRESS_AT_TICK is specified when we want a key to be pressed
// after the game starts, when a specific tick (frame) is reached.
// This is often useful in order to select the joystick or for
// similar tasks.
//
// Just specify PRESS_AT_TICK as pin, then the frame number, and
// finally the key.
#define PRESS_AT_TICK   0xfe // Press at the specified frame.
#define RELEASE_AT_TICK 0xfd // Release at the specified frame.
#define KEY_END         0xff // This just marks the end of the key map.

// Extended keymaps allow two device buttons (pins) pressed together to map
// to other Specturm keys. This is useful for games such as Skool Daze
// that have too many keys doing useful things.
// 
// To use this kind of maps, xor KEY_EXT to the first pin, then
// provide as second entry in the row the second pin, and finally
// a single Spectrum key code to trigger.
// 
// IMPORTANT: the extended key maps of a game must be the initial entries,
// before the normal entries. This way we avoid also sensing the keys
// mapped to the single buttons involved.
#define KEY_EXT         0x80

/* ========================== Global state and defines ====================== */

// Don't trust this USEC figure here, since the z80.h file implementation
// is modified to glue together the instruction fetch steps, so we do
// more work per tick.
#define FRAME_USEC (25000)

struct emustate {
    zx_t zx;    // The emulator state.
    int debug;  // Debugging mode

    // We switch betweent wo clocks: one is selected just for zx_exec(), that
    // is the most speed critical code path. For all the other code execution
    // we stay to a lower overclocking mode that is low enough to allow the
    // flash memory to be accessed without issues.
    uint32_t base_clock;
    uint32_t emu_clock;

    uint32_t tick; // Frame number since last game load.

    // Keymap in use right now. Modified by load_game().
    uint8_t keymap[3*100];     // 100 map entries... more than enough.
    char *keymap_file;         // Pointer of the keymap description file
                               // inside the flash memory.

    // Is the game selection / config menu shown?
    int menu_active;
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
} EMU;

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
#define UI_EVENT_NAVIGATION 254 // Just moving around in the menu.
#define UI_EVENT_DISMISS 255    // Menu dismissed.

const uint32_t SettingsZoomValues[] = {50,75,84,100,112,125,150,200};
const char *SettingsZoomValuesNames[] = {"50%","75%","84%","100%","112%","125%","150%","200%",NULL};
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
    {UI_EVENT_CLOCK,
        "clock", &EMU.emu_clock, 5000, 130000, 600000, NULL, NULL},
    {UI_EVENT_BORDER,
        "border", &EMU.show_border, 1, 0, 1, NULL, NULL},
    {UI_EVENT_SCALING,
        "scaling", &EMU.scaling, 0, 0, 0,
        SettingsZoomValues, SettingsZoomValuesNames,
    },
    {UI_EVENT_VOLUME,
        "volume", &EMU.volume, 1, 0, 20, NULL, NULL},
    {UI_EVENT_BRIGHTNESS,
        "bright", &EMU.brightness, 1, 0, ST77_MAX_BRIGHTNESS, NULL, NULL},
    {UI_EVENT_PARTIAL,
        "part-up", &EMU.partial_update, 1, 0, 1, NULL, NULL},
    {UI_EVENT_SYNC,
        "sync",(uint32_t*)&EMU.audio_sample_wait, 5, 0, 1000, NULL, NULL},
    {UI_EVENT_NONE,
        "scan-p", (uint32_t*)&EMU.zx.scanline_period, 1, 10, 500, NULL, NULL}
};

#define SettingsListLen (sizeof(SettingsList)/sizeof(SettingsList[0]))

// Convert the setting 'id' name and current value into a string
// to show as menu item.
void settings_to_string(char *buf, size_t buflen, int id) {
    if (SettingsList[id].values == NULL) {
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
    if (si->values == NULL) {
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

// Called when the UI is active. Handle the key presses needed to select
// the game and change the overclock.
//
// Returns 1 is if some event was processed. Otherwise 0.
#define UI_DEBOUNCING_TIME 100000
uint32_t ui_handle_key_press(void) {
    const uint8_t km[] = {
        KEY_LEFT, 0, KEMPSTONE_LEFT,
        KEY_RIGHT, 0, KEMPSTONE_RIGHT,
        KEY_FIRE, 0, KEMPSTONE_FIRE,
        KEY_DOWN, 0, KEMPSTONE_DOWN,
        KEY_UP, 0, KEMPSTONE_UP,
        KEY_END, 0, 0,
    };
    static absolute_time_t last_key_accepted_time = 0;

    // Debouncing
    absolute_time_t now = get_absolute_time();
    if (now - last_key_accepted_time < UI_DEBOUNCING_TIME) return 0;

    uint32_t event = UI_EVENT_NONE; // Event generated by key press, if any.
    int key_pressed = -1;
    for (int j = 0; ;j += 3) {
        if (km[j] == KEY_END) break;
        if (km[j] >= 32) continue; // Skip special codes.
        if (get_device_button(km[j])) {
            key_pressed = km[j+2];
            break;
        }
    }
    if (key_pressed == -1) return UI_EVENT_NONE; // No key pressed right now.

    event = UI_EVENT_NAVIGATION; // If there is a more specific event, this
                                 // will be set accordingly.
    int value_change_dir = -1;
    switch(key_pressed) {
    case KEMPSTONE_UP: ui_go_next_prev_game(-1); break;
    case KEMPSTONE_DOWN: ui_go_next_prev_game(1); break;
    case KEMPSTONE_RIGHT: value_change_dir = 1; // fall through.
    case KEMPSTONE_LEFT:
        if (EMU.selected_game < 0)
            event = settings_change_value(-EMU.selected_game-1,
                                          value_change_dir);
        break;
    case KEMPSTONE_FIRE:
        if (EMU.selected_game == EMU.loaded_game) {
            // We are forced to reload the game, because the UI
            // messed with the Spectrum video RAM.
            load_game(EMU.selected_game);
            EMU.menu_active = 0;
            EMU.menu_left_at_tick = EMU.tick;
            event = UI_EVENT_DISMISS;
        } else if (EMU.selected_game >= 0) {
            load_game(EMU.selected_game);
            event = UI_EVENT_LOADGAME;
        }
        break;
    }
    last_key_accepted_time = now;
    return event;
}

// Before writing our user interfafce inside the Spectrum video RAM, we need
// to make sure that the colors in the area we will be using will make the
// menu visible.
void ui_set_area_attributes(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t *vmem = EMU.zx.ram[EMU.zx.display_ram_bank];
    for (uint16_t y = y1; y <= y2 && y < 192; y += 8) {
        for (uint16_t x = x1; x <= x2 && x < 256; x += 8) {
            vmem[0x1800+(((y>>3)<<5)|(x>>3))] = 7;
        }
    }
}

// If the menu is active, draw it.
void ui_draw_menu(void) {
    // Draw the menu in the right / top part of the screen.
    int menu_x = 255-100;
    int menu_w = 100;
    int menu_y = 0;
    int menu_h = 192/3*2;
    int font_size = 2;
    menu_h -= menu_h&(8*font_size-1); // Make multiple of font pixel size;
    int vpad = 2;       // Vertical padding of text inside the box.
    menu_h += vpad*2;   // Allow for pixels padding / top bottom.
    menu_h &= ~0x7;     // Make multiple of 8 to avoid color clash.

    ui_fill_box(menu_x, menu_y, menu_w, menu_h, 0, 15);
    ui_set_crop_area(menu_x+1,menu_x+menu_w-2,
                     menu_y+1,menu_y+menu_h-2);

    int first_game = (int)EMU.selected_game - 5;
    int num_settings = (int)SettingsListLen;
    if (first_game < -num_settings) first_game = -num_settings;

    // Make the menu visible.
    ui_set_area_attributes(menu_x,menu_y,menu_x+menu_w-1,menu_y+menu_h-1);

    int y = menu_y+vpad; // Incremented as we write text.
    for (int j = first_game;; j++) {
        if (j >= (int)GamesTableSize || y > menu_y+menu_h) break;

        int color = j >= 0 ? 4 : 6;
        font_size = j >= 0 ? 2 : 1;

        // Highlight the currently selected game, with a box of the color
        // of the font, and the black font (so basically the font is inverted).
        if (j == EMU.selected_game) {
            ui_fill_box(menu_x+2,y,menu_w-2,font_size*8,color,color);
            color = 0;
        }
        if (j < 0) {
            // Show setting item.
            struct UISettingsItem *si = &SettingsList[-j-1];
            char sistr[32];
            settings_to_string(sistr,sizeof(sistr),-j-1);
            ui_draw_string(menu_x+2,y,sistr,color,font_size);
        } else {
            // Show game item.
            ui_draw_string(menu_x+2,y,GamesTable[j].name,color,font_size);
        }
        y += 8*font_size;
    }
    ui_reset_crop_area();
}

/* =========================== Emulator implementation ====================== */

// Set a bitmap signaling which part of the screen RAM was touched and
// is yet to be updated on the display. This function is called when the
// address 'addr' is in the range of the VRAM bitmap area.
void vram_set_dirty_bitmap(uint16_t addr) {
    uint16_t y = ((addr&0x1800)>>5) | ((addr&0x700)>>8) | ((addr&0xe0)>>2);
    EMU.dirty_vram[y>>3] |= 1 << (y&7);
}

// Like vram_set_dirty_bitmap() but called for addresses in the range
// of the color attributes.
void vram_set_dirty_attr(uint16_t addr) {
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

// Decode the 256x192 Spectrum bitmap + attribute VRAM into the
// template's chunked framebuffer, one palette-index byte per pixel,
// centred at (ZX_FB_X0, ZX_FB_Y0). The surrounding margin is cleared
// to the current border colour. Runs every VBL; the m68k blits the
// result to the ST screen. Unlike upstream there is no scaling or
// partial-update: the whole 320x200 buffer is rewritten each frame.
void __not_in_flash_func(update_display)(void) {
    const uint8_t *vmem = EMU.zx.ram[EMU.zx.display_ram_bank];
    const uint8_t border = EMU.show_border ? EMU.zx.border_color : 0;

    // Clear the whole framebuffer to the border colour; the decode
    // below overwrites the central 256x192 window.
    fb_chunked_clear(border);

    for (int py = 0; py < 192; py++) {
        const uint8_t *row = vmem +
            (((py & 0xC0) << 5) | ((py & 0x07) << 8) | ((py & 0x38) << 2));
        const uint8_t *attrrow = vmem + 0x1800 + ((py >> 3) << 5);
        uint8_t *dst = fb_chunked_buffer +
            (ZX_FB_Y0 + py) * FB_CHUNKED_W + ZX_FB_X0;

        for (int bx = 0; bx < 32; bx++) {
            uint8_t bits = row[bx];
            uint8_t attr = attrrow[bx];
            uint8_t bright = (attr & 0x40) >> 3;    // 0 or 8
            uint8_t ink = (attr & 7) | bright;
            uint8_t paper = ((attr >> 3) & 7) | bright;
            if ((attr & 0x80) && zx_blink_phase) {  // FLASH: swap ink/paper
                uint8_t t = ink; ink = paper; paper = t;
            }
            for (int b = 0; b < 8; b++) {
                *dst++ = (bits & 0x80) ? ink : paper;
                bits <<= 1;
            }
        }
    }
}

// This function maps GPIO state to the Spectrum keyboard registers.
// Other than that, certain keys are pressed when a given frame is
// reached, in order to enable the joystick or things like that.
#define HANDLE_KEYPRESS_MACRO 1
#define HANDLE_KEYPRESS_PIN 2
#define HANDLE_KEYPRESS_ALL (HANDLE_KEYPRESS_MACRO|HANDLE_KEYPRESS_PIN)
void handle_zx_key_press(zx_t *zx, const uint8_t *keymap, uint32_t ticks, int flags) {
    // This 128 bit bitmap remembers what keys we put down
    // during this call. This is useful as sometimes key maps
    // have multiple keys mapped to the same Spectrum key, and if
    // some phisical key put down some Spectrum key, we don't want
    // a successive mapping to up it up.
    uint64_t put_down[4] = {0,0};
    #define put_down_set(keycode) put_down[keycode>>6] |= (1ULL<<(keycode&63))
    #define put_down_get(keycode) (put_down[keycode>>6] & (1ULL<<(keycode&63)))

    for (int j = 0; ;j += 3) {
        if (keymap[j] == KEY_END) {
            // End of keymap reached.
            break;
        } else if (keymap[j] == PRESS_AT_TICK || keymap[j] == RELEASE_AT_TICK) {
            if (keymap[j+1] != ticks) continue; // Tick number mismatch.
            if (!(flags & HANDLE_KEYPRESS_MACRO)) continue; // No handling.

            // Press/release keys when a given frame is reached.
            if (keymap[j] == PRESS_AT_TICK) {
                printf("Pressing '%c' at frame %d\n",keymap[j+2],ticks);
                zx_key_down(zx,keymap[j+2]);
            } else {
                printf("Releasing '%c' at frame %d\n",keymap[j+2],ticks);
                zx_key_up(zx,keymap[j+2]);
            }
        } else {
            // Map the GPIO status to the ZX Spectrum keyboard
            // registers.
            if (!(flags & HANDLE_KEYPRESS_PIN)) continue;
            if (!(keymap[j] & KEY_EXT)) {
                // Normal key maps: Pico pin -> two Spectrum keys.
                if (get_device_button(keymap[j])) {
                    if (keymap[j+1]) {
                        put_down_set(keymap[j+1]);
                        zx_key_down(zx,keymap[j+1]);
                    }
                    if (keymap[j+2]) {
                        put_down_set(keymap[j+2]);
                        zx_key_down(zx,keymap[j+2]);
                    }
                } else {
                    // Release.
                    if (!put_down_get(keymap[j+1]) && keymap[j+1])
                        zx_key_up(zx,keymap[j+1]);
                    if (!put_down_get(keymap[j+2]) && keymap[j+2])
                        zx_key_up(zx,keymap[j+2]);
                }
            } else {
                // Extended key maps: two Pico pins -> one Spectrum key.
                if (get_device_button(keymap[j]&0x7f) &&
                    get_device_button(keymap[j+1]))
                {
                    put_down_set(keymap[j+2]);
                    zx_key_down(zx,keymap[j+2]);
                    return; // Return ASAP before processing normal keys.
                } else {
                    if (!put_down_get(keymap[j+2])) zx_key_up(zx,keymap[j+2]);
                }
            }
        }
    }

    // Detect long press of left+right to return back in
    // game selection mode.
    {
        #define LEFT_RIGHT_LONG_PRESS_FRAMES 30
        static int left_right_frames = 0;
        if (get_device_button(KEY_LEFT) && get_device_button(KEY_RIGHT)) {
            left_right_frames++;
            if (left_right_frames == LEFT_RIGHT_LONG_PRESS_FRAMES)
                EMU.menu_active = 1;
        } else {
            left_right_frames = 0;
        }
    }
}

// Clear all keys. Useful when we switch game, to make sure that no
// key downs are left from the previous keymap.
void flush_zx_key_press(zx_t *zx) {
    for (int j = 0; j < KBD_MAX_KEYS; j++) zx_key_up(zx,j);
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

// Case-insensitive ".z80" extension test (FatFs may hand back either an
// LFN with original case or an upper-case 8.3 short name).
static int ends_with_z80(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *e = name + n - 4;
    return e[0] == '.' &&
           (e[1] == 'z' || e[1] == 'Z') &&
           e[2] == '8' && e[3] == '0';
}

#if ZX_GAMES_FROM_SD
// SD app folder (from per-app config, default "/zx").
static char zx_folder[64] = "/zx";
// RAM copy of keymaps.txt; EMU.keymap_file points here.
static char zx_keymap_buf[ZX_KEYMAP_BUF_SZ];

// Load keymaps.txt from the app folder into zx_keymap_buf. If the file
// is absent, write the firmware-default keymaps first, then use them.
static void load_keymaps_file(void) {
    char path[96];
    snprintf(path, sizeof(path), "%s/keymaps.txt", zx_folder);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        // Not present: create it from the built-in default.
        if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
            UINT bw = 0;
            f_write(&f, zx_builtin_keymaps,
                    (UINT)strlen(zx_builtin_keymaps), &bw);
            f_close(&f);
        }
        strncpy(zx_keymap_buf, zx_builtin_keymaps, sizeof(zx_keymap_buf) - 1);
        zx_keymap_buf[sizeof(zx_keymap_buf) - 1] = 0;
        EMU.keymap_file = zx_keymap_buf;
        return;
    }

    UINT br = 0;
    f_read(&f, zx_keymap_buf, sizeof(zx_keymap_buf) - 1, &br);
    zx_keymap_buf[br] = 0;
    f_close(&f);
    EMU.keymap_file = zx_keymap_buf;
}

// Enumerate the app folder for .z80 snapshots (and load keymaps.txt).
// Returns the number of games found.
int populate_games_list(void) {
    SettingsConfigEntry *fe =
        settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
    if (fe && fe->value[0]) {
        strncpy(zx_folder, fe->value, sizeof(zx_folder) - 1);
        zx_folder[sizeof(zx_folder) - 1] = 0;
    }

    load_keymaps_file();

    GamesTableSize = 0;
    DIR dir;
    if (f_opendir(&dir, zx_folder) != FR_OK) return 0;

    FILINFO fno;
    while (GamesTableSize < ZX_MAX_GAMES &&
           f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        if (!ends_with_z80(fno.fname)) continue;
        struct game_entry *ge = &GamesTable[GamesTableSize++];
        strncpy(ge->name, fno.fname, sizeof(ge->name) - 1);
        ge->name[sizeof(ge->name) - 1] = 0;
        ge->addr = NULL;
        ge->size = (uint32_t)fno.fsize;
    }
    f_closedir(&dir);
    return (int)GamesTableSize;
}
#else
// No-SD build: a single baked-in game + firmware-default keymaps.
int populate_games_list(void) {
    struct game_entry *ge = &GamesTable[0];
    strncpy(ge->name, "BUILTIN", sizeof(ge->name) - 1);
    ge->name[sizeof(ge->name) - 1] = 0;
    ge->addr = (void *)zx_builtin_game;
    ge->size = zx_builtin_game_len;
    GamesTableSize = 1;
    EMU.keymap_file = (char *)zx_builtin_keymaps;
    return 1;
}
#endif

// Bring up the emulator: defaults, ST palette, and the Z80 core.
// Replaces upstream init_emulator() minus all the Pico display / GPIO /
// PWM / overclock setup, which the template owns.
void init_emulator(void) {
    EMU.debug = 0;
    EMU.menu_active = 1;
    EMU.base_clock = 225000;   // Informational only (settings menu).
    EMU.emu_clock = 225000;
    EMU.tick = 0;
    EMU.keymap[0] = KEY_END;
    EMU.keymap_file = NULL;
    EMU.selected_game = 0;
    EMU.loaded_game = -1;
    EMU.show_border = DEFAULT_DISPLAY_BORDERS;
    EMU.scaling = DEFAULT_DISPLAY_SCALING;
    EMU.volume = 20;
    EMU.brightness = ST77_MAX_BRIGHTNESS;
    EMU.partial_update = DEFAULT_DISPLAY_PARTIAL_UPDATE;
    EMU.audio_sample_wait = 300;
    ui_reset_crop_area();

    // Publish the ZX Spectrum palette to the Atari ST shifter.
    zx_set_palette();

    // Emulator core: 48K, Kempston joystick.
    zx_desc_t zx_desc = {0};
    zx_desc.type = ZX_TYPE_48K;
    zx_desc.joystick_type = ZX_JOYSTICKTYPE_KEMPSTON;
    zx_desc.roms.zx48k.ptr = (void *)dump_amstrad_zx48k_bin;
    zx_desc.roms.zx48k.size = sizeof(dump_amstrad_zx48k_bin);
    zx_init(&EMU.zx, &zx_desc);
    EMU.zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;
}

// Parse line and fill current map.
// This function turns a description in the following format into the
// three (or six) bytes entry representing one mapping in the keymap.
//
// Cases we need to handle:
//
// 1. xul2   Extended keymap (x). ul (up+left) = keypress of 2
// 2. lx     Standard keymap with one key press. l (left) = press x
// 3. rz2    Standard keymap with two presses: r(right) = press z and 2
// 4. l1|l   Kempstone keypresses are |<dir>, |lrud or |f for fire.
// 5. @10:k1 Auto keypresses: at frame 10, press k for 1 frame.
//
// Note that the case "5" will actually write 6 bytes, one for the press
// and one for the release event.
//
// Return the number of bytes filled (frame entries will fill six
// bytes instead of three), or 0 on syntax error.
int keymap_descr_to_row(char *p, uint8_t *map) {
    char buf[16]; // An entry like @1000:|f100 is still just 12 bytes.

    // Let's work on a copy of the entry, stripping everything on the
    // right so that we just have the map line itself.
    int idx = 0;
    while(*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r'
          && idx < (int)sizeof(buf)-1)
    {
        buf[idx++] = *p++;
    }
    buf[idx] = 0;
    
    // We have to set three bytes in total, so do the conversion
    // in three exact steps.
    int ext = 0; // Ext map if true (two buttons + one key).
    int atframe = 0; // Special automatic keypress at frame entry if true.
    int pos = 0; // Position inside the keymap line.
    for (int j = 0; j < 3; j++) {
        // Handle special conditions: extended and atframe entry.
        if (j == 0) {
            if (ext == 0 && buf[0] == 'x') {
                ext = 1;
                pos++;
            } else if (atframe == 0 && buf[0] == '@') {
                atframe = 1;
                map[0] = PRESS_AT_TICK;
                map[3] = RELEASE_AT_TICK;
                pos++;
            }
        }

        if (j == 1 && atframe) {
            // Read the at-frame frame.
            char *frame = buf+pos;
            while (buf[pos] && buf[pos] != ':')
                pos++;
            if (buf[pos] == 0) return 0; // Syntax error.
            buf[pos] = 0; pos++; // Pos points to key to press.
            map[j] = atoi(frame);
        } else if ((j == 0 && !atframe) || (j == 1 && ext)) {
            // We read the button pin if:
            //
            // First keymap byte and is not at-frame entry.
            // Second keymap byte and it is an extended map (has two pins).
            uint8_t pin;
            switch(buf[pos]) {
            case 'l': pin = KEY_LEFT; break;
            case 'r': pin = KEY_RIGHT; break;
            case 'u': pin = KEY_UP; break;
            case 'd': pin = KEY_DOWN; break;
            case 'f': pin = KEY_FIRE; break;
            default: return 0; // Syntax error.
            }
            if (ext && j == 0) pin |= KEY_EXT;
            map[j] = pin;
            pos++;
        } else if (j == 2 || (j == 1 && !ext)) {
            // Read the mapped Spectrum button or joystick move.
            // Note that normal entries map to up to two buttons.
            // The third entry is always a button press.
            // The second entry is a button press if it's not an extended map.

            // Normal maps (one pin, two buttons) may just have a single
            // button. Stop here if that's the case.
            if (j == 2 && buf[pos] == 0) break;

            if (buf[pos] == '|') {
                // Kempstone moves are prefixed by |
                pos++;
                switch(buf[pos]) {
                case 'l': map[j] = KEMPSTONE_LEFT; break;
                case 'r': map[j] = KEMPSTONE_RIGHT; break;
                case 'u': map[j] = KEMPSTONE_UP; break;
                case 'd': map[j] = KEMPSTONE_DOWN; break;
                case 'f': map[j] = KEMPSTONE_FIRE; break;
                default: return 0; // Syntax error.
                }
            } else {
                // Normal keypress. Just take the byte given by the user.
                if (buf[pos] == '~') buf[pos] = ' ';
                map[j] = buf[pos];
            }
            pos++;
            if (atframe) {
                // Populate the release entry too.
                map[5] = map[2]; // Key to release is the same.
                map[4] = map[1] + atoi(buf+pos); // Release frame.
            }
        }
    }
    return atframe ? 6 : 3;
}

/* This function will parse the keymap file stored in the flash memory
 * (so it must be called with base overclocking, in order to read
 * from flash) and will try to match every entry with the current RAM
 * content in order to find a suitable keymap. */
void get_keymap_for_current_game(int game_id) {
    int got_match = 0; // State telling we got a match with RAM content.
    uint8_t *map = EMU.keymap;
    char *p = EMU.keymap_file;

    int line = 1; // Line number inside keymap file.
    while(1) {
        // Discard comments and empty lines.
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') {
            // If we have a match, a space means our map ended. In this case
            // return.
            if (got_match) {
                *map = KEY_END; // Terminate the map.
                // Log the loaded keymap: useful when things don't
                // work as expected.
                map = EMU.keymap;
                for (int j = 0; j < sizeof(EMU.keymap)/3; j++) {
                    if (map[j*3] == KEY_END) break;
                    printf("map entry %d: %d %d %d\n", j,
                        map[j*3], map[j*3+1], map[j*3+2]);
                }
                return;
            }

            // Otherwise, discard.
            goto next_line;
        }

        // New map. Let's see if we match with it.
        if ((!memcmp(p,"MATCH:",6) && !got_match) ||
            (!memcmp(p,"AND-MATCH",9) && got_match))
        {
            char *end = strchr(p,'\n');
            if (*(end-1) == '\r') end--;
            p = strchr(p,':'); p++;
            unsigned int pattern_len = (end-p);

            // Scan the Spectrum memory for a match.
            int found = 0;
            uint8_t *ram = (uint8_t*)EMU.zx.ram;
            for (uint32_t j = 0; j < 49152-pattern_len; j++) {
                if (ram[j] == p[0] && !memcmp(ram+j,p,pattern_len)) {
                    found = 1;
                    break;
                }
            }

            got_match = found != 0;
            goto next_line;
        }

        // If we reach the default map, just load it.
        if (!memcmp(p,"DEFAULT:",8)) {
            got_match = 1;
            goto next_line;
        }

        // If we don't have a match, skip this map description line.
        if (got_match == 0) goto next_line;

        if (map == EMU.keymap) printf("Loading keymap at line %d\n", line);

        // Keymaps may redefine scanline period for games with special
        // requirements about the CRT timing.
        if (!memcmp(p,"SCANLINE-PERIOD:",16)) {
            int period = atoi(p+16);
            if (period > 0 && period < 500 && EMU.loaded_game != game_id)
                EMU.zx.scanline_period = period;
            goto next_line;
        }

        // Turn this line into three bytes map entry using an helper
        // function.
        int used_bytes;
        used_bytes = keymap_descr_to_row(p,map);
        if (used_bytes == 0) {
            int len = strchr(p,'\n') - p;
            printf("Keymap syntax error at line %d: %.*s\n",line,len,p);
            *map = KEY_END;
            return;
        }
        map += used_bytes; // Go to the next entry;

        if (map > EMU.keymap+sizeof(EMU.keymap)-3) {
            // It is unlikely that a keymap is too long, but let's handle
            // it in case of bugs. The -3 above is there because there are
            // maps that can use two entries (6 bytes instead of 3 bytes).
            map -= 3;
            *map = KEY_END;
            return;
        }

next_line:
        line++;
        if (!memcmp(p,"#END",4)) {
            printf("No default keymap found\n");
            return; // Stop on end of file.
        }
        p = strchr(p,'\n');
        p++;
    }
}

/* Load the specified game ID (index into GamesTable). As a side effect,
 * selects the matching keymap. */
void load_game(int game_id) {
    if (game_id < 0 || game_id >= (int)GamesTableSize) return;
    struct game_entry *g = &GamesTable[game_id];

    flush_zx_key_press(&EMU.zx);   // No keys held across a game switch.
    zx_kbd_mask = 0;
    EMU.tick = 0;

    if (EMU.loaded_game != game_id)
        EMU.zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;

#if ZX_GAMES_FROM_SD
    // Borrow the chunked framebuffer as a transient load buffer -- it is
    // overwritten by the next update_display() anyway, so this avoids a
    // permanent ~50 KB allocation for the .z80 image.
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
    zx_quickload(&EMU.zx, r);
#else
    chips_range_t r = { .ptr = g->addr, .size = g->size };
    zx_quickload(&EMU.zx, r);
#endif

    get_keymap_for_current_game(game_id);
    EMU.loaded_game = game_id;
}

/* ============================ Public entry points ========================= */

void zxemu_init(void) {
    init_emulator();

    // Populate the games list (SD scan or the baked-in game) and boot
    // the first entry. If none is found we still fall through: the ZX
    // ROM boot screen renders, so the user can see the emulator is
    // alive (and, on an SD build, that the folder is empty).
    if (populate_games_list() > 0)
        load_game(EMU.selected_game);
}

// Atari ST IKBD scancodes for the keys we map to gameplay buttons.
#define ZX_SC_UP     0x48
#define ZX_SC_DOWN   0x50
#define ZX_SC_LEFT   0x4B
#define ZX_SC_RIGHT  0x4D
#define ZX_SC_SPACE  0x39
#define ZX_SC_ENTER  0x1C

void zxemu_handle_key(const ikbd_key_event_t *k) {
#if ZX_INPUT_KEYBOARD
    int btn = -1;
    switch (k->scancode) {
        case ZX_SC_LEFT:  btn = ZX_BTN_LEFT;  break;
        case ZX_SC_RIGHT: btn = ZX_BTN_RIGHT; break;
        case ZX_SC_UP:    btn = ZX_BTN_UP;    break;
        case ZX_SC_DOWN:  btn = ZX_BTN_DOWN;  break;
        case ZX_SC_SPACE:
        case ZX_SC_ENTER: btn = ZX_BTN_FIRE;  break;
        default: break;
    }
    if (btn >= 0) {
        if (k->is_press) zx_kbd_mask |= (1u << btn);
        else             zx_kbd_mask &= ~(1u << btn);
    }
#else
    (void)k;
#endif
}

void zxemu_render_frame(void) {
    // Compose this frame's button state: keyboard, plus the ST joystick
    // when that phase is built. get_device_button() reads zx_input_mask.
    zx_input_mask = zx_kbd_mask;
#if ZX_INPUT_JOYSTICK
    {
        uint8_t j = ikbd_get_joystick();  // bit0 up,1 down,2 left,3 right,7 fire
        if (j & 0x01) zx_input_mask |= (1u << ZX_BTN_UP);
        if (j & 0x02) zx_input_mask |= (1u << ZX_BTN_DOWN);
        if (j & 0x04) zx_input_mask |= (1u << ZX_BTN_LEFT);
        if (j & 0x08) zx_input_mask |= (1u << ZX_BTN_RIGHT);
        if (j & 0x80) zx_input_mask |= (1u << ZX_BTN_FIRE);
    }
#endif

    // Menu input (game selection / settings). ui_handle_key_press()
    // itself loads the selected game on FIRE and clears menu_active.
    if (EMU.menu_active) {
        ui_handle_key_press();
    }

    // Map the live buttons onto the Spectrum keyboard for this frame.
    // While the menu is up (or was just dismissed) only the automatic
    // frame-triggered macros run, not the physical buttons.
    int kflags = HANDLE_KEYPRESS_ALL;
    if (EMU.menu_active || EMU.tick < EMU.menu_left_at_tick + 10)
        kflags = HANDLE_KEYPRESS_MACRO;
    handle_zx_key_press(&EMU.zx, EMU.keymap, EMU.tick, kflags);

    // Run the Spectrum for one frame's worth of ticks.
    zx_exec(&EMU.zx, FRAME_USEC);

    // Draw the menu over the emulated screen if it is active.
    if (EMU.menu_active) ui_draw_menu();

    // Toggle the FLASH phase ~twice per second (every 16 frames).
    if ((EMU.tick & 0x0f) == 0) zx_blink_phase ^= 1u;

    // Decode VRAM into the framebuffer and hand off to the ST (blocks
    // on the VBL, pacing this loop to 50 Hz).
    update_display();
    fb_publish();

    EMU.tick++;
}

void zxemu_audio_fill(uint8_t *buf, uint32_t bytes) {
#if ZX_AUDIO_YM
    // The emulator samples the 1-bit beeper into zx.audiobuf during
    // zx_exec(). Decimate the bits produced since the last fill down to
    // the ~112 samples the m68k Timer-B handler consumes per VBL, and
    // map each to a full/zero YM volume on both channels.
    const uint32_t nsamp = bytes / 2;
    const uint32_t total = AUDIOBUF_LEN * 32;   // total beeper-sample bits
    static uint32_t rd = 0;                     // last consumed bit index
    uint32_t w = EMU.zx.audiobuf_byte * 32 + EMU.zx.audiobuf_bit;
    uint32_t avail = (w - rd) & (total - 1);

    if (avail < nsamp) {
        // Too few fresh samples: hold the current beeper level (DC).
        uint8_t v = EMU.zx.beeper_state ? ZX_YM_VOLUME : 0;
        for (uint32_t i = 0; i < nsamp; i++) { buf[2*i] = v; buf[2*i+1] = v; }
        return;
    }

    uint32_t step = avail / nsamp;
    uint32_t idx = rd;
    for (uint32_t i = 0; i < nsamp; i++) {
        uint32_t bit = (EMU.zx.audiobuf[(idx >> 5) & (AUDIOBUF_LEN - 1)]
                        >> (idx & 31)) & 1u;
        uint8_t v = bit ? ZX_YM_VOLUME : 0;
        buf[2*i] = v;
        buf[2*i+1] = v;
        idx += step;
    }
    rd = w;
#else
    for (uint32_t i = 0; i < bytes; i++) buf[i] = 0;
#endif
}
