/*
 * fuxi_activation.c: SpringBoard reaches the lock screen instead of the first-boot assistant.
 *
 * Adapted from HakiOS, oosexgang/hackios-xnu-userland, src/inject_sb.c (MIT, see ../NOTICE.md):
 * the first two items of that file's list of SpringBoard fixes --
 *
 *   1. when mobileactivationd is not there to answer, the device is "activated" anyway, and
 *   2. the setup assistant is never entered, whatever SpringBoard wants it for.
 *
 * The third item of that list, starting SpringBoard late on a boot argument (sb_delay=N), is not
 * taken: fuxi has no wrapper around SpringBoard's launchd job to wait in, and nothing in this
 * port needs the delay. The rest of the upstream file -- the battery and touch plumbing, the
 * debug notifications, the poster switcher and the side button's long press -- is not taken.
 *
 * Why this file has no addresses where HakiOS has two. The upstream replacements are found by
 * their address in the 16.5 (20F66) shared cache, and a method whose implementation is not at
 * that address is left alone. The check is safe, but the addresses are not portable: every
 * cache build moves them, and a stale one is a hook that silently does not happen. Here the
 * class and the selector are named and the Objective-C runtime is asked where they are
 * (objc_getClass, class_getInstanceMethod). The runtime's answer is the only one that can be
 * right for the cache actually mapped, whatever the slide, the ASLR and the build are.
 *
 * The one offset HakiOS does have to know -- the int at +8 of SBLockdownManager that holds the
 * activation answer -- is read from the class's own metadata as well: the _state ivar if it is
 * there, otherwise the first int ivar whose name says "state". An ivar layout that gains a
 * field in front of it therefore does not turn into a write to the wrong place. If the names
 * are gone, the hook is not installed, one line is logged, and SpringBoard does what it does
 * without us: a miss, never a blind write.
 *
 * Copyright (c) 2026 the UniXNU-fuxi authors, for the parts written here.
 * HakiOS parts: Copyright (c) 2025-2026 the HakiOS authors, MIT License.
 */

#include "fuxi_soft_render.h"

#include <objc/runtime.h>
#include <stdlib.h>
#include <string.h>

/* ---- the phone is activated --------------------------------------------------------------- */

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * first int ivar of the object: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no
 * answer. mobileactivationd is not in this profile's job set (rc.jobs.daemons) and could not
 * talk to Apple even if it were, so the answer never comes and SpringBoard takes the phone for
 * bricked, showing the first-boot "hello" instead of the lock screen.
 */
#define LOCKDOWN_ACTIVATED	2
#define LOCKDOWN_STATE_IVAR	"_state"

static Ivar activation_state;
static unsigned activation_calls;

/*
 * The int ivar that holds the answer. The name first; then any int ivar of the class whose name
 * still says "state" (the layout of a newer build, or a rename to _activationState). The scan
 * walks the class chain and takes an int, not a BOOL: _state qualifies, _settingUpActivationState
 * does not.
 */
static Ivar
state_ivar_of(Class cls)
{
	Ivar ivar = class_getInstanceVariable(cls, LOCKDOWN_STATE_IVAR);
	Class c;

	if (ivar != NULL) {
		const char *type = ivar_getTypeEncoding(ivar);

		if (type != NULL && type[0] == 'i') {
			return ivar;
		}
		fuxi_say("fuxi: %s.%s is not an int (%s); looking for the state ivar\n",
		    class_getName(cls), LOCKDOWN_STATE_IVAR, type != NULL ? type : "?");
	}
	for (c = cls; c != Nil; c = class_getSuperclass(c)) {
		unsigned count = 0, i;
		Ivar *list = class_copyIvarList(c, &count);

		for (i = 0; i < count; i++) {
			const char *name = ivar_getName(list[i]);
			const char *type = ivar_getTypeEncoding(list[i]);

			if (name != NULL && type != NULL && type[0] == 'i' &&
			    (strstr(name, "state") != NULL || strstr(name, "State") != NULL)) {
				Ivar found = list[i];

				free(list);
				return found;
			}
		}
		free(list);
	}
	return NULL;
}

/*
 * The answer, without asking. The original is not called: there is no daemon to ask, and calling
 * it would only wait for the XPC timeout before writing the same 0. Its other work is the flag
 * that says the question is being asked, which is 0 again by the time a call returns.
 */
static void
setup_activation_state(id self, __unused SEL selector)
{
	int *state = (int *)((char *)self + ivar_getOffset(activation_state));

	*state = LOCKDOWN_ACTIVATED;
	if (activation_calls++ == 0) {
		fuxi_say("fuxi: SpringBoard: the device is activated (%s +%td, state %d)\n",
		    ivar_getName(activation_state), ivar_getOffset(activation_state), *state);
	}
}

/* ---- the setup assistant is never entered ------------------------------------------------- */

/*
 * -[SBSetupManager _setSetupRequiredReason:] is how SpringBoard decides that the setup assistant
 * is wanted, and why. No reason is a reason here: this system is not going to be set up through
 * that assistant, and entering it is the other way to lose the lock screen. The reason is said
 * once per boot, then the original is asked for reason 0.
 *
 * The return type is BOOL in the class's interface (a setup reason was taken) and was void in
 * the 16.5 header HakiOS compiled against; BOOL is the signature that is harmless to both, and
 * the original's answer is passed through rather than invented.
 */
static IMP original_set_setup_required_reason;
static unsigned setup_reasons_said;

static BOOL
set_setup_required_reason(id self, SEL selector, unsigned long long reason)
{
	if (reason != 0 && setup_reasons_said++ < 8) {
		fuxi_say("fuxi: SpringBoard wants the setup assistant (reason %llu); not entered\n",
		    reason);
	}
	if (original_set_setup_required_reason == NULL) {
		return NO;
	}
	return ((BOOL (*)(id, SEL, unsigned long long))original_set_setup_required_reason)(self, selector, 0);
}

/* ---- installing the two hooks -------------------------------------------------------------- */

static bool
replace_instance_method(const char *class_name, const char *selector_name, IMP replacement, IMP *original)
{
	Class cls = objc_getClass(class_name);
	Method method;
	IMP found;

	if (cls == NULL) {
		fuxi_say("fuxi: %s is not loaded; -%s left alone\n", class_name, selector_name);
		return false;
	}
	method = class_getInstanceMethod(cls, sel_registerName(selector_name));
	if (method == NULL) {
		fuxi_say("fuxi: -[%s %s] is not there; left alone\n", class_name, selector_name);
		return false;
	}
	found = method_getImplementation(method);
	method_setImplementation(method, replacement);
	if (original != NULL) {
		*original = found;
	}
	fuxi_say("fuxi: -[%s %s] (%s) replaced\n", class_name, selector_name,
	    method_getTypeEncoding(method));
	return true;
}

__attribute__((constructor))
static void
fuxi_activation_init(void)
{
	const char *name = getprogname();
	Class lock;

	if (name == NULL || strcmp(name, "SpringBoard") != 0) {
		return;
	}
	lock = objc_getClass("SBLockdownManager");
	if (lock == NULL) {
		fuxi_say("fuxi: activation: no SBLockdownManager; the first-boot answer is left alone\n");
	} else {
		activation_state = state_ivar_of(lock);
		if (activation_state == NULL) {
			fuxi_say("fuxi: activation: SBLockdownManager has no int state ivar;"
			    " the first-boot answer is left alone\n");
		} else if (replace_instance_method("SBLockdownManager", "_setupActivationState",
		    (IMP)setup_activation_state, NULL)) {
			fuxi_say("fuxi: activation: the answer is kept in %s +%td\n",
			    ivar_getName(activation_state), ivar_getOffset(activation_state));
		} else {
			activation_state = NULL;
		}
	}
	replace_instance_method("SBSetupManager", "_setSetupRequiredReason:",
	    (IMP)set_setup_required_reason, &original_set_setup_required_reason);
	fuxi_say("fuxi: activation %s, setup assistant %s\n",
	    activation_state != NULL ? "answered here" : "NOT hooked",
	    original_set_setup_required_reason != NULL ? "never entered" : "NOT hooked");
}
