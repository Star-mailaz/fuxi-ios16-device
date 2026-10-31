/*
 * fuxi_soft_render.h: the software-render profile of the fuxi userspace injection.
 *
 * What this is, and where it comes from: the window server (backboardd) must draw with
 * CoreAnimation's own software rasteriser, and the pieces that make it do so are taken from
 * HakiOS, oosexgang/hackios-xnu-userland (MIT, see ../NOTICE.md), src/inject.c: the
 * render-server prompt that stops it from trying to build a Metal or OpenGL ES context again,
 * the display update hooks, and the removal of the five-second ambient-light wait that nothing
 * would ever answer on this board.
 *
 * What is NOT taken from there: the OpenGL ES library (src/eagl.c, Mesa behind it), the Metal
 * bridge, and every hook that exists only to watch the GPU. They are not compiled here and are
 * not in this directory: FUXI_RENDERER selects the renderer, and a build that wants the GPU
 * selects the other profile. That is the rule the porting spec sets: a renderer is a profile,
 * not a commented-out block.
 *
 * Copyright (c) 2026 the UniXNU-fuxi authors, for the parts written here. The routines named
 * below are adapted from HakiOS, Copyright (c) 2025-2026 the HakiOS authors, MIT License.
 */

#ifndef FUXI_SOFT_RENDER_H
#define FUXI_SOFT_RENDER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/*
 * The renderer this library implements. FUXI_RENDERER_SOFTWARE is the only value this file
 * builds for; FUXI_RENDERER_ADRENO is refused at build time rather than silently degraded,
 * because the Adreno path lives in another component (Adreno740/AdrenoMetal) and has its own
 * injection needs.
 */
#define FUXI_RENDERER_SOFTWARE 1
#define FUXI_RENDERER_ADRENO   2

#ifndef FUXI_RENDERER
#define FUXI_RENDERER FUXI_RENDERER_SOFTWARE
#endif

#if FUXI_RENDERER != FUXI_RENDERER_SOFTWARE
#error "fuxi_soft_render.c is the software-render profile; select the Adreno component instead"
#endif

/*
 * Where the dyld shared cache was built to be, before the slide. The addresses below are
 * unslid: the cache is mapped at CACHE_BASE + slide, and every expected address is compared
 * after adding the slide, so a cache that is not the one these were derived from is left
 * alone instead of being written to at the wrong place.
 */
#define FUXI_CACHE_BASE 0x180000000UL

/*
 * Addresses inside the 16.7.16 (20H392) dyld shared cache of iPhone10,3.
 *
 * 0 means "not derived yet": fuxi_replace_virtual() refuses such an entry, says so once, and
 * the hook is skipped. That is the honest state of the port: the fields below are the ones
 * this cache has to supply, and each is filled from a symbol lookup on that cache and then
 * checked by fuxi_replace_virtual() against what is actually in the table before it is used.
 */
struct fuxi_cache_offsets {
	/* CA::WindowServer::AppleCLCDDisplay */
	uintptr_t vt_apple_clcd_display;
	uintptr_t clcd_is_ready;              /* vtable +0x0b0 */
	uintptr_t clcd_ignore_update_p;       /* vtable +0x138 */
	uintptr_t clcd_begin_update;          /* vtable +0x170 */
	uintptr_t clcd_finish_update;         /* vtable +0x178 */
	/* CA::WindowServer::EAGLServer */
	uintptr_t vt_eagl_server;
	uintptr_t eagl_renderer;              /* vtable +0x1a0 */
	uintptr_t eagl_render_update;         /* vtable +0x178 */
	/* SpringBoard: the snapshot format of a view that leaves the screen */
	uintptr_t sb_snapshot_output_format;
	/* containermanagerd: +[MCMUserIdentityCache personasAreSupported] */
	uintptr_t cm_personas_are_supported;
};

/*
 * The table itself. Each library built from this directory has its own copy: a dylib is injected
 * alone into its program (../inject.conf), and the SpringBoard library is not loaded together
 * with the window server's, so neither can take the other's symbols. The definitions are the
 * same; there is still one place to fill them in.
 *
 * The activation chain is not in this table: fuxi_activation.c finds its two methods by class
 * name and selector at run time, so there is no address here for this cache to supply.
 */
static const struct fuxi_cache_offsets fuxi_cache_16_7_16 __attribute__((unused)) = {
	.vt_apple_clcd_display = 0,
	.clcd_is_ready = 0,
	.clcd_ignore_update_p = 0,
	.clcd_begin_update = 0,
	.clcd_finish_update = 0,
	.vt_eagl_server = 0,
	.eagl_renderer = 0,
	.eagl_render_update = 0,
	.sb_snapshot_output_format = 0,
	.cm_personas_are_supported = 0,
};

/*
 * Log a line to the console (standard error). Inline for the reason the table is: both libraries
 * speak, and one is not loaded with the other.
 */
static inline void fuxi_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static inline void
fuxi_say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		write(2, line, (size_t)n);
	}
}

/* The slide of the mapped cache, as the constructor found it. */
uintptr_t fuxi_cache_slide(void);

/* Replace one vtable entry that must still hold "expected"; NULL when it does not. */
void *fuxi_replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement);

#endif /* FUXI_SOFT_RENDER_H */
