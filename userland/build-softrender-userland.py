#!/usr/bin/env python3
"""Build the fuxi userspace injection for the software-render profile.

usage: build-softrender-userland.py [--output DIR] [--sdk PATH]

Two dynamic libraries, and nothing else:

  libFuxiSoftRender.dylib        inject/fuxi_soft_render.c        (window server, container managers)
  libFuxiSpringBoardSoft.dylib   inject/fuxi_springboard_soft.c   (SpringBoard)

What this replaces, and what it deliberately does not: HakiOS's userland/build.sh compiles five
libraries and a variable amount around them -- an OpenGL ES library over Mesa, a Metal bridge, a
media library, a tweak loader -- under one `GPU=`/`GL=` switch (see NOTICE.md). fuxi takes the
job list and the injection table organisation from there, and none of the GPU-conditional code:
the renderer here is a value of FUXI_RENDERER fixed by the profile, so there is no path in this
build that could load eagl.c, Mesa, Turnip or a Metal bundle even if they were present.

The device's payload policy decides where the libraries end up; the paths below match
userland/inject.conf, which the launchd job set reads at run time.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
INJECT = os.path.join(ROOT, "inject")
RENDERER_SOFTWARE = 1

SOURCES = {
    "libFuxiSoftRender.dylib": ["fuxi_soft_render.c"],
    "libFuxiSpringBoardSoft.dylib": ["fuxi_springboard_soft.c"],
}

FRAMEWORKS = {
    "libFuxiSoftRender.dylib": ["CoreFoundation", "IOKit", "Foundation"],
    "libFuxiSpringBoardSoft.dylib": ["CoreFoundation", "Foundation"],
}


def fail(message):
    sys.stderr.write("build-softrender-userland: %s\n" % message)
    raise SystemExit(1)


def check_profile():
    """The profile is a value, not a comment: refuse anything but the software renderer."""
    header = os.path.join(INJECT, "fuxi_soft_render.h")
    with open(header, encoding="utf-8") as handle:
        text = handle.read()
    for needed in ("#define FUXI_RENDERER_SOFTWARE 1", "#ifndef FUXI_RENDERER"):
        if needed not in text:
            fail("%s does not define the renderer profile switch" % header)
    define = "-DFUXI_RENDERER=%d" % RENDERER_SOFTWARE
    # A source that mentions the GPU libraries must not be in this build.
    for name in ("eagl.c", "metal", "turnip", "freedreno"):
        for library, sources in SOURCES.items():
            for source in sources:
                if name in source:
                    fail("%s: %s is a GPU component and is not built here" % (library, source))
    return define


def check_job_lists():
    """Both lists the payload needs must be there and must cover the desktop chain."""
    jobs = os.path.join(ROOT, "rc.jobs.daemons")
    inject = os.path.join(ROOT, "inject.conf")
    with open(jobs, encoding="utf-8") as handle:
        labels = [line.strip() for line in handle if line.strip() and not line.startswith("#")]
    with open(inject, encoding="utf-8") as handle:
        table = [line.strip() for line in handle if line.strip() and not line.startswith("#")]
    for required in ("com.apple.backboardd", "com.apple.SpringBoard"):
        if required not in labels:
            fail("%s does not list %s" % (jobs, required))
    programs = set()
    for line in table:
        parts = line.split()
        programs.update(parts[1:])
    for required in ("backboardd", "containermanagerd", "SpringBoard"):
        if required not in programs:
            fail("%s does not inject into %s" % (inject, required))
    return len(labels), len(table)


def compile_library(name, sources, frameworks, output, sdk, define):
    cc = shutil.which("clang")
    if cc is None:
        fail("no clang on PATH (this step needs a macOS toolchain with an iOS SDK)")
    command = [cc, "-dynamiclib", "-O2", "-arch", "arm64", "-mios-version-min=16.0",
               define, "-I", INJECT, "-lobjc",
               "-install_name", "/fuxi/lib/" + name]
    if sdk:
        command += ["-isysroot", sdk]
    for framework in frameworks:
        command += ["-framework", framework]
    command += ["-o", os.path.join(output, name)]
    command += [os.path.join(INJECT, source) for source in sources]
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", default=os.path.join(ROOT, "out"),
                        help="directory the libraries are written to")
    parser.add_argument("--sdk", default=os.environ.get("FUXI_IOS_SDK"),
                        help="iOS SDK to build against (default: the toolchain's own)")
    arguments = parser.parse_args()

    if sys.platform != "darwin":
        fail("the injection is a Darwin library: build it on macOS with an iOS SDK (spec 7: "
             "the Linux host only drives the kernel, payload and firmware steps)")

    define = check_profile()
    labels, injected = check_job_lists()
    os.makedirs(arguments.output, exist_ok=True)
    for name, sources in SOURCES.items():
        compile_library(name, sources, FRAMEWORKS[name], arguments.output, arguments.sdk, define)
        print("built", os.path.join(arguments.output, name))
    print("profile: software renderer (%s), %d daemons, %d programs injected" % (define, labels, injected))


if __name__ == "__main__":
    main()
