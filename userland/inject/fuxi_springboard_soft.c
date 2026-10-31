/*
 * fuxi_springboard_soft.c: SpringBoard on a CoreAnimation software renderer.
 *
 * Adapted from HakiOS, oosexgang/hackios-xnu-userland, src/inject_sb.c (MIT, see
 * ../NOTICE.md): the vibrant-view switch and the snapshot output format -- the two places where
 * the userland does something different because nothing draws with a GPU. The rest of that
 * file (activation, the setup assistant, the battery and touch plumbing, the debug
 * notifications) is userland bring-up work that this port has not taken yet; it is listed in
 * ../README.md so that it is not mistaken for done.
 *
 * Copyright (c) 2026 the UniXNU-fuxi authors, for the parts written here.
 * HakiOS parts: Copyright (c) 2025-2026 the HakiOS authors, MIT License.
 */

#include "fuxi_soft_render.h"

#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string.h>
#include <unistd.h>

/*
 * Does the window server draw with a renderer of its own? In this profile the answer is known
 * at build time: there is no GPU driver and no OpenGL ES implementation is loaded, so
 * CoreAnimation draws with its software renderer. The GPU profile asks the same question of
 * the kernel's GPU service and of the boot arguments; that is a different library, not a
 * different branch here.
 */
static bool
gpu_draws(void)
{
	return false;
}

/*
 * The lock screen's clock is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are a mask -- a copy of them through a colour matrix that keeps red as alpha --
 * through which the filter lets the wallpaper behind come out lighter, and a gradient over the
 * whole screen tones the wallpaper down the same way. A GPU renderer draws both filters;
 * CoreAnimation's software renderer draws neither, so the digits come out red and the top of
 * the wallpaper as it was. The views have a switch, the user default
 * BSUIVibrancyEffectViewDisabled: with it they show their content as drawn, the clock in plain
 * white. It is set for this run of SpringBoard alone (nothing is written to disk).
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue,
	    kCFPreferencesCurrentApplication);
	fuxi_say("fuxi: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/*
 * The picture SpringBoard keeps of an application that leaves the screen: 0 = PNG, 1 and 2 =
 * ATX (an ASTC texture), 3 = JPEG. A device with a wide colour display that can decode ASTC
 * gets ATX, and Darwin compresses it on the processor: fast with the vector code of Apple's
 * cores, but here it costs more than ten seconds per picture, and all that time the
 * application waits for the answer until the ten-second watchdog for scene updates kills it --
 * no application survived going to the background. JPEG takes a fraction of a second; it has
 * no transparency, so a picture that needs it becomes a PNG, which is what Darwin itself does
 * on a device without ASTC.
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/*
 * Installing the snapshot hook needs the class that owns the method, and HakiOS reaches it by
 * its address in the 16.5 shared cache. The 20H392 address is not in
 * fuxi_cache_16_7_16 yet, so the hook is not installed: the picture format stays ATX and an
 * application that goes to the background is still killed by the scene-update watchdog. That
 * is a known gap of this port, not a silent fallback -- see ../README.md.
 */
static void
snapshot_hook(void)
{
	if (fuxi_cache_16_7_16.sb_snapshot_output_format == 0) {
		fuxi_say("fuxi: SpringBoard: snapshot format hook not installed (no 20H392 offset yet)\n");
		return;
	}
	fuxi_say("fuxi: SpringBoard: snapshot format hook ready at %#lx\n",
	    fuxi_cache_16_7_16.sb_snapshot_output_format);
}

__attribute__((constructor))
static void
fuxi_springboard_soft_init(void)
{
	const char *name = getprogname();

	if (name == NULL || strcmp(name, "SpringBoard") != 0) {
		return;
	}
	plain_vibrancy();
	snapshot_hook();
}
