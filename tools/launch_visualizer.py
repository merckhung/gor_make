#!/usr/bin/env python3
import os
import subprocess
import sys

env = os.environ.copy()
env["DISPLAY"] = ":0"
env["WAYLAND_DISPLAY"] = "wayland-0"
env["XAUTHORITY"] = "/run/user/469354/.mutter-Xwaylandauth.4R71R3"
env["SDL_VIDEODRIVER"] = "x11"

cmd1 = ["./bazel-bin/gor_make/skia_visualizer", "/usr/local/google/home/merckhung/android_bp.json"]
cmd2 = ["./bazel-bin/gor_make/skia_visualizer", "/usr/local/google/home/merckhung/chromium_gn.json"]

p1 = subprocess.Popen(cmd1, env=env, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p2 = subprocess.Popen(cmd2, env=env, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

print(f"Started Android Visualizer (PID {p1.pid}) and Chromium Visualizer (PID {p2.pid})")
