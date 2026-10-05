# Pre-script for env:pico: before `buildfs` / `uploadfs`, writes
# data/config.bin from the env's `custom_config_json` with the web's encoder
# (web/scripts/config-image.ts), so the filesystem image carries the config.
# A plain `upload` is untouched, so a config pushed by CFG_SET survives it.
# Needs pnpm on PATH and `pnpm install` run in web/.

import shutil
import subprocess
from os.path import join

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

if {"buildfs", "uploadfs"} & set(COMMAND_LINE_TARGETS):
    root = env.subst("$PROJECT_DIR")
    src = join(root, env.GetProjectOption("custom_config_json"))
    out = join(env.subst("$PROJECT_DATA_DIR"), "config.bin")
    pnpm = shutil.which("pnpm")
    if pnpm is None:
        print("config_image: pnpm not found on PATH")
        env.Exit(1)
    rc = subprocess.call([pnpm, "--dir", join(root, "web"), "exec", "vite-node",
                          "scripts/config-image.ts", "--", src, out])
    if rc != 0:
        print("config_image: failed; is `pnpm install` run in web/?")
        env.Exit(1)
