#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "stdbool.h"

#include "syscode.h"

#include "system.h"
#include "cart.h"
#include "nones.h"
#include "utils.h"

static const char *szRomName;
static char szSaveName[256];
static char app_quit = 0;

#define FRAMERATE 60
#define TIMER_MUL (((1000 << 12) + FRAMERATE - 1) / FRAMERATE)
static unsigned timerlast, timertick, timertick_ms;

static void *framebuf_mem;
static uint16_t *framebuf;

typedef void (*scr_update_t)(void *src, uint16_t *dst, unsigned h);
extern const scr_update_t scr_update_fn[];

#define RGB555(v) \
	(((v) >> 8 & 0xf800) | ((v) >> 5 & 0x7c0) | ((v) >> 3 & 0x1f))

uint16_t NesPalette[64] = {
#define X(a, b, c, d) RGB555(a), RGB555(b), RGB555(c), RGB555(d),
X(0x737373, 0x21188c, 0x0000ad, 0x42009c) /* 00 */
X(0x8c0073, 0xad0010, 0xa50000, 0x7b0800)
X(0x422900, 0x004200, 0x005200, 0x003910)
X(0x18395a, 0x000000, 0x000000, 0x000000)
X(0xbdbdbd, 0x0073ef, 0x2139ef, 0x8400f7) /* 10 */
X(0xbd00bd, 0xe7005a, 0xde2900, 0xce4a08)
X(0x8c7300, 0x009400, 0x00ad00, 0x009439)
X(0x00848c, 0x000000, 0x000000, 0x000000)
X(0xffffff, 0x39bdff, 0x5a94ff, 0xce8cff) /* 20 */
X(0xf77bff, 0xff73b5, 0xff7363, 0xff9c39)
X(0xf7bd39, 0x84d610, 0x4ade4a, 0x5aff9c)
X(0x00efde, 0x7b7b7b, 0x000000, 0x000000)
X(0xffffff, 0xade7ff, 0xc6d6ff, 0xd6ceff) /* 30 */
X(0xffc6ff, 0xffc6de, 0xffbdb5, 0xffdead)
X(0xffe7a5, 0xe7ffa5, 0xadf7bd, 0xb5ffce)
X(0x9cfff7, 0xc6c6c6, 0x000000, 0x000000)
#undef X
};

enum {
	KEY_RESET = 1, KEY_EXIT, KEY_SAVE,
	KEY_A = 8, KEY_B, KEY_SELECT, KEY_START,
	KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
};

void lcd_appinit(void) {
	struct sys_display *disp = &sys_data.display;
	int w = disp->w1, h = disp->h1;
	int scaler = sys_data.scaler - 1;
	unsigned crop = 0, wide = 0;
	if (h > w) h = w;
	if (scaler >= 99) crop = 8, scaler -= 100;
	if (scaler >= 49) wide = 1, scaler -= 50;
	if (h <= 68) {
		scaler = 4; crop = 8; wide = h == 68;
		if (h == 48) scaler++;
	} else if ((unsigned)scaler >= 4) {
		if (h >= 320) scaler = 2;
		else if (h >= 240) scaler = 0;
		else if (h >= 176) scaler = 3;
		else scaler = 1;
	}
	sys_data.scaler = scaler << 1 | wide;
	sys_data.user[0] = crop;
	disp->w2 = w;
	disp->h2 = h;
}

static void framebuf_init(void) {
	struct sys_display *disp = &sys_data.display;
	int w = disp->w2, h = disp->h2;
	unsigned size = w * h; uint8_t *p;
	p = malloc(size * 2 + 31);
	framebuf_mem = p;
	p += -(intptr_t)p & 31;
	framebuf = (uint16_t*)p;
	if (sys_data.scaler == 2 * 2) {
		p -= 2;
		do p += w * 2, *(uint16_t*)p = 0;
		while (--h);
	}
}

#define X(num, name) KEYPAD_##name = num,
enum { KEYPAD_ENUM(X) };
#undef X

void keytrn_init(void) {
	uint8_t keymap[64], rkeymap[64];
	int i;
	static const uint8_t keys[] = {
		KEYPAD_LSOFT, KEY_SELECT,   KEYPAD_RSOFT, KEY_START,
		KEYPAD_UP,    KEY_UP,       KEYPAD_LEFT,  KEY_LEFT,
		KEYPAD_RIGHT, KEY_RIGHT,    KEYPAD_DOWN,  KEY_DOWN,
		KEYPAD_2,     KEY_UP,       KEYPAD_4,     KEY_LEFT,
		KEYPAD_6,     KEY_RIGHT,    KEYPAD_5,     KEY_DOWN,
		KEYPAD_1,     KEY_B,        KEYPAD_3,     KEY_B,
		KEYPAD_7,     KEY_B,        KEYPAD_8,     KEY_B,
		KEYPAD_9,     KEY_B,        KEYPAD_CENTER,KEY_A,
		KEYPAD_DIAL,  KEY_A,        KEYPAD_STAR,  KEY_A,
		KEYPAD_0,     KEY_A,        KEYPAD_HASH,  KEY_A,
		KEYPAD_VOLUP, KEY_A,        KEYPAD_VOLDOWN,KEY_B,
		KEYPAD_PLUS,  KEY_START
	};
	static const uint8_t keys_power[] = {
		KEYPAD_LSOFT, KEY_START,   KEYPAD_DIAL,  KEY_EXIT,
		KEYPAD_UP,    KEY_RESET,   KEYPAD_DOWN,  KEY_EXIT,
		KEYPAD_LEFT,  KEY_SELECT,  KEYPAD_RIGHT, KEY_START,
		KEYPAD_CENTER,KEY_B,       KEYPAD_0,     KEY_SAVE
	};
	sys_getkeymap(keymap);

#define FILL_RKEYMAP(keys_array) \
	memset(rkeymap, 0, sizeof(rkeymap)); \
	for (i = 0; i < (int)sizeof(keys_array); i += 2) \
		rkeymap[keys_array[i]] = keys_array[i + 1];

#define FILL_KEYTRN(j) \
	for (i = 0; i < 64; i++) { \
		unsigned a = keymap[i]; \
		sys_data.keytrn[j][i] = a < 64 ? rkeymap[a] : 0; \
	}

	FILL_RKEYMAP(keys)
	FILL_KEYTRN(0)
	FILL_RKEYMAP(keys_power)
	FILL_KEYTRN(1)
#undef FILL_RKEYMAP
#undef FILL_KEYTRN
}

static void wait_frame(void) {
	unsigned t0 = timertick, t1, t2;
	t2 = 1;
	do t0 += TIMER_MUL; while (--t2);
	t2 = t0 >> 12;
	t1 = t2 - timertick_ms;
	if (t2 >= 1000) t0 -= TIMER_MUL * 60, t2 -= 1000;
	timertick = t0; timertick_ms = t2;
	t1 += timerlast; timerlast = t1;
	t0 = sys_timer_ms(); t1 -= t0;
	if ((int)t1 > 0 && t1 > 500 / 60) sys_wait_ms(t1);
}

static unsigned key_timer[2];
static unsigned key_flags = 0;

static void PadInputStateUpdate(Nones *nones) {
	int type, key;
	for (;;) {
		type = sys_event(&key);
		switch (type) {
		case EVENT_KEYUP:
			if (key >= 8) nones->buttons[key - 8] = false;
			else if ((key -= KEY_RESET) < 2) key_flags &= ~(1 << key);
			break;
		case EVENT_KEYDOWN:
			if (key >= 8) nones->buttons[key - 8] = true;
			else if (key == KEY_SAVE) CartSaveSram(nones->system->cart);
			else if ((key -= KEY_RESET) < 2) {
				key_flags |= 1 << key;
				key_timer[key] = sys_timer_ms();
			}
			break;
		case EVENT_END: goto end;
		case EVENT_QUIT:
			nones->quit = true;
			app_quit = 1;
			goto end;
		}
	}
end:
	if (key_flags) {
		unsigned i, time = sys_timer_ms();
		for (i = 0; i < 2; i++) {
			if (key_flags >> i & 1)
			if (time - key_timer[i] > 1000) {
				key_flags &= ~(1 << i);
				if (i) {
					nones->quit = true;
					app_quit = 1;
				} else SystemReset(nones->system);
			}
		}
	}
	SystemUpdateJPButtons(nones->system, nones->buttons);
}

int main(int argc, char **argv) {
	if (sys_data.mac & 0x100) {
		unsigned i, r, g, b;
		for (i = 0; i < 64; i++) {
			b = NesPalette[i];
			r = b >> 8 & 0xf8; r |= r >> 5;
			g = b >> 3 & 0xf8; g |= g >> 5;
			b = b << 3 & 0xf8; b |= b >> 5;
			r = (r * 4899 + g * 9617 + b * 1868 + 0x2000) >> 14;
			NesPalette[i] = r << 8;
		}
	}

	while (argc > 1) {
		if (argc > 2 && !strcmp(argv[1], "--")) {
			argc -= 1; argv += 1;
			break;
		} else break;
	}

	if (argc != 2) return 1;
	szRomName = argv[1];

	Nones nones;
	memset(&nones, 0, sizeof(Nones));
	nones.arena = ArenaCreate(1024 * 1024 * 3);
	nones.system = SystemCreate(nones.arena);

	if (CartLoad(nones.arena, nones.system->cart, szRomName)) {
		ArenaDestroy(nones.arena);
		return 1;
	}

	uint16_t *buffers[2];
	const uint32_t buffer_size = (SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
	buffers[0] = ArenaPush(nones.arena, buffer_size);
	buffers[1] = ArenaPush(nones.arena, buffer_size);

	lcd_appinit();
	framebuf_init();
	sys_framebuffer(framebuf);
	sys_start();
	keytrn_init();

	SystemInit(nones.system, nones.arena, false, false, 0, buffers, buffer_size);

	timerlast = sys_timer_ms();
	timertick = 0; timertick_ms = 0;

	while (!nones.quit) {
		PadInputStateUpdate(&nones);
		SystemRun(nones.system, nones.debug_info);
		sys_wait_refresh();
		unsigned crop = sys_data.user[0];
		unsigned h = SCREEN_HEIGHT - crop * 2;
		uint16_t *src_start = nones.system->ppu->buffers[1] + (crop * SCREEN_WIDTH);
		scr_update_fn[sys_data.scaler](src_start, framebuf, h);
		sys_start_refresh();
		wait_frame();
	}

	SystemShutdown(nones.system);
	sys_end();
	if (framebuf_mem) free(framebuf_mem);
	ArenaDestroy(nones.arena);
	return 0;
}
