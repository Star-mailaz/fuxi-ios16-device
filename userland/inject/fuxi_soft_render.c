/*
 * fuxi_soft_render.c: the window server draws with CoreAnimation's software rasteriser.
 *
 * Adapted from HakiOS, oosexgang/hackios-xnu-userland, src/inject.c (MIT, see ../NOTICE.md):
 * the renderer prompt, the display update hooks, the ambient-light wait and the user-manager
 * personas answer. What was dropped is everything that exists to reach or watch a GPU, and the
 * whole OpenGL ES side (eagl.c) which is not built in this profile.
 *
 * Copyright (c) 2026 the UniXNU-fuxi authors, for the parts written here.
 * HakiOS parts: Copyright (c) 2025-2026 the HakiOS authors, MIT License.
 */

#include "fuxi_soft_render.h"

#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

/*
 * The table itself, and the log, are in fuxi_soft_render.h: this library and the SpringBoard
 * one are injected separately, so each carries its own copy of both.
 */
static uintptr_t slide;

uintptr_t
fuxi_cache_slide(void)
{
	return slide;
}

/*
 * Replace one entry of a virtual table that must still hold "expected". The tables live in
 * read-only memory shared by every process, so the page is made privately writable first and
 * read-only again afterwards.
 */
void *
fuxi_replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry;
	vm_address_t page;
	kern_return_t kr;
	void *original;

	if (slot == 0 || expected == 0) {
		static unsigned said_missing;

		if (said_missing++ < 16) {
			fuxi_say("fuxi: %s: no offset for this cache yet; left alone\n", what);
		}
		return NULL;
	}
	entry = (uintptr_t *)(slot + slide);
	page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	if (*entry != expected + slide) {
		fuxi_say("fuxi: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		fuxi_say("fuxi: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: the window server's display -------------------------------------------------- */

#define VSLOT(vtable, offset) ((vtable) + 0x10 + (offset))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		fuxi_say("fuxi: is_ready -> %d (call %u)\n", ready, calls_ready);
		last_ready = ready;
	}
	calls_ready++;
	return ready;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int ignore = orig_ignore_update_p(display, update) & 1;

	if (ignore != last_ignore || calls_ignore < 3) {
		fuxi_say("fuxi: ignore_update_p -> %d (call %u)\n", ignore, calls_ignore);
		last_ignore = ignore;
	}
	calls_ignore++;
	return ignore;
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		fuxi_say("fuxi: begin_update -> %#lx\n", r);
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		fuxi_say("fuxi: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while it gets nothing it tries again to
 * make a Metal and then an OpenGL ES context, which costs a lot and cannot succeed here: there
 * is no GPU driver, and this profile does not load an OpenGL ES implementation. One refusal is
 * enough; after it the server draws with CA::WindowServer::Server::sw_renderer(), which is the
 * whole point of the profile.
 */
static void *
my_renderer(void *server)
{
	static int refused, said;
	void *renderer;

	if (refused) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		refused = 1;
		fuxi_say("fuxi: no accelerated renderer; CoreAnimation's software renderer draws\n");
	} else if (!said) {
		said = 1;
		fuxi_say("fuxi: the server has a renderer (%p); this profile expected none\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		fuxi_say("fuxi: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

static void
window_server_fixes(void)
{
	const struct fuxi_cache_offsets *o = &fuxi_cache_16_7_16;

	orig_is_ready = fuxi_replace_virtual("is_ready", VSLOT(o->vt_apple_clcd_display, 0x0b0),
	    o->clcd_is_ready, my_is_ready);
	orig_ignore_update_p = fuxi_replace_virtual("ignore_update_p", VSLOT(o->vt_apple_clcd_display, 0x138),
	    o->clcd_ignore_update_p, my_ignore_update_p);
	orig_begin_update = fuxi_replace_virtual("begin_update", VSLOT(o->vt_apple_clcd_display, 0x170),
	    o->clcd_begin_update, my_begin_update);
	orig_finish_update = fuxi_replace_virtual("finish_update", VSLOT(o->vt_apple_clcd_display, 0x178),
	    o->clcd_finish_update, my_finish_update);
	orig_renderer = fuxi_replace_virtual("renderer", VSLOT(o->vt_eagl_server, 0x1a0),
	    o->eagl_renderer, my_renderer);
	orig_render_update = fuxi_replace_virtual("render_update", VSLOT(o->vt_eagl_server, 0x178),
	    o->eagl_render_update, my_render_update);
	fuxi_say("fuxi: window server hooks %s\n", orig_renderer ? "installed" : "not installed (offsets missing)");
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for an ambient light sensor to turn up among the HID services. There is none on
 * this board and none will come. HakiOS waits for its own touch screen service instead; fuxi
 * has no such service yet (NativeHidInput is not wired to the injection), so the wait is simply
 * not made, and the sequence goes on at once.
 */
static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	fuxi_say("fuxi: no ambient light sensor: not waiting for one\n");
}

static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL) {
		fuxi_say("fuxi: -[BLControl waitForALSArrival]: not found; left alone\n");
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
	fuxi_say("fuxi: the ambient-light wait is gone\n");
}

/* ---- containermanagerd: no personas ----------------------------------------------------------- */

/*
 * Both container managers ask the user manager for the "personas" of the user, of which this
 * system has none, twenty times a second apart before they give up -- and everything that needs
 * a container, the window server first, waits with them. Told that personas are not supported,
 * they do not ask.
 */
static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	const struct fuxi_cache_offsets *o = &fuxi_cache_16_7_16;
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL) {
		fuxi_say("fuxi: +[MCMUserIdentityCache personasAreSupported]: not found; left alone\n");
		return;
	}
	if (o->cm_personas_are_supported == 0 || (uintptr_t)original != o->cm_personas_are_supported + slide) {
		fuxi_say("fuxi: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not the expected one;"
		    " left alone\n", (uintptr_t)original - slide);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	fuxi_say("fuxi: container manager: no personas\n");
}

/* ---- entry ------------------------------------------------------------------------------------ */

__attribute__((constructor))
static void
fuxi_soft_render_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - FUXI_CACHE_BASE;
	fuxi_say("fuxi: software render profile in %s (cache slide %#lx)\n", name, slide);
	if (strcmp(name, "backboardd") == 0) {
		window_server_fixes();
		brightness_fixes();
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	}
}
