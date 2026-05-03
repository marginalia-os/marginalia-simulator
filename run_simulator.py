"""
PlatformIO library build script for the Crosspoint Simulator.

Handles two things automatically when this lib is included as a lib_dep:

1. Ensures every dependency directory in `$PROJECT_LIBDEPS_DIR/$PIOENV` has a
   `library.json` file when only `library.properties` is present. This avoids
   PlatformIO warnings from incompatible libraries.

2. Registers a backward-compatible "run_simulator" custom target.

This file can be loaded more than once in the same PlatformIO process:
- once from this library's `library.json` build hook
- again indirectly when a consuming firmware repo adds the separate
  `run_simulator_project.py` helper for IDE task exposure

Use a process-wide sentinel so the custom target is registered only once even
when multiple registration paths exist.
"""

Import("env")
import os
import json
import builtins

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"
RUN_SIMULATOR_TARGET_OWNER_OPTION = "custom_run_simulator_target_owner"



RESET = "\x1b[0m"
BLUE = "\x1b[34m"
GREEN = "\x1b[32m"
YELLOW = "\x1b[33m"
RED = "\x1b[31m"

PREFIX = "[SIMULATOR BUILD]"

lib_path = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"])

def color_print(prefix, message, color=BLUE):
    print(f"{prefix} {color}{message}{RESET}")

def _ensure_library_json_files(lib_path):
    color_print(PREFIX, f"Scanning library dependencies in: {lib_path}", BLUE)
    if not os.path.isdir(lib_path):
        color_print(PREFIX, "Library path not found, skipping library.json generation.", RED)
        return

    for lib_name in sorted(os.listdir(lib_path)):
        lib_dir = os.path.join(lib_path, lib_name)
        if not os.path.isdir(lib_dir):
            color_print(PREFIX, f"Skipping non-directory entry: {lib_name}", RESET)
            continue

        color_print(PREFIX, f"Checking library: {lib_name}", BLUE)
        json_path = os.path.join(lib_dir, "library.json")
        if os.path.isfile(json_path):
            color_print(PREFIX, f" library.json exists, skipping: {lib_name}", GREEN)
            continue

        prop_path = os.path.join(lib_dir, "library.properties")
        if not os.path.isfile(prop_path):
            color_print(PREFIX, f" no library.properties found, skipping: {lib_name}", YELLOW)
            continue

        library_data = {
            "name": lib_name,
            "version": "1.0.0",
            "frameworks": "*"
        }

        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(library_data, f, indent=4)
            f.write("\n")

        color_print(PREFIX, f" Created library.json for {lib_name}", GREEN)

_ensure_library_json_files(lib_path)


# --- run_simulator custom target ---

def _run_simulator(source, target, env):
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    subprocess.run([binary], cwd=os.getcwd())


target_owner = env.GetProjectOption(RUN_SIMULATOR_TARGET_OWNER_OPTION, "").strip().lower()

if target_owner != "project" and not getattr(builtins, RUN_SIMULATOR_TARGET_KEY, False):
    setattr(builtins, RUN_SIMULATOR_TARGET_KEY, True)
    env.AddCustomTarget(
        name="run_simulator",
        dependencies="$PROGPATH",
        actions=_run_simulator,
        title="Run Simulator",
        description="Build and run the desktop simulator",
        always_build=True,
    )