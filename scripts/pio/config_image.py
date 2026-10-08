# Pre-script for env:pico: before `buildfs` / `uploadfs`, writes
# data/config.bin from the env's `custom_config_json` with the web's encoder
# (web/scripts/config-image.ts), so the filesystem image carries the config;
# and data/mesh.bin from `custom_mesh_json` (web/scripts/mesh-image.ts), or
# none without it, so the Pico boots flat.
# A plain `upload` is untouched, so a config pushed by CFG_SET survives it.
# Needs pnpm on PATH and `pnpm install` run in web/.

import os
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

    def encode(script, src, out):
        rc = subprocess.call([pnpm, "--dir", join(root, "web"), "exec", "vite-node",
                              script, "--", src, out])
        if rc != 0:
            print(f"config_image: {script} failed; is `pnpm install` run in web/?")
            env.Exit(1)

    encode("scripts/config-image.ts", src, out)

    mesh_out = join(env.subst("$PROJECT_DATA_DIR"), "mesh.bin")
    mesh = env.GetProjectOption("custom_mesh_json", "")
    if mesh:
        encode("scripts/mesh-image.ts", join(root, mesh), mesh_out)
    elif os.path.exists(mesh_out):
        os.remove(mesh_out)
