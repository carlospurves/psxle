# This script builds PSXLE
# Run:
#   python build.py <Optional: build path, default is psxle_build>
# to build, then
#   python install.py
# to intall.

from __future__ import print_function
import platform
import sys
import os
import subprocess
import errno
import shutil

def fatal(*args, **kwargs):
    print("Error:", *args, file=sys.stderr, **kwargs)
    sys.exit(1)

def warn(*args, **kwargs):
    print("Warning:", *args, file=sys.stderr, **kwargs)

if ('linux' not in platform.platform().lower()) or (os.name != "posix"):
    fatal("PSXLE currently only supports Linux.")

if 'ubuntu' not in platform.platform().lower():
    warn("PSXLE is untested on your distribution. Ubuntu 16.04+ is recommended.")


build_path = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else "psxle_build"

if not os.path.isdir(build_path):
    if os.path.isfile(build_path):
        fatal(build_path, "is a file. Build must be into a directory.")
    else:
        try:
            os.mkdir(build_path)
        except OSError as exc:
            if exc.errno != errno.EEXIST:
                fatal("Could not make build directory:", str(exc))
            pass

print("""Build Note:
    PSXLE requires the following dependancies:
       - SDL 2
       - GLIB 2
       - GTK 3
       - xvfb
       - cmake
    If you do not have them, they can be installed using:
         sudo apt install libsdl2-dev
         sudo apt install libglib2.0-dev
         sudo apt install libgtk-3-dev
         sudo apt install xvfb
         sudo apt install cmake

    After building, PSXLE can be installed by running:
        python install.py

    """)

print("    Build will be into", build_path)

raw_input("    Press enter to continue...")

if not os.path.isdir(build_path):
    os.mkdir(build_path)

k = subprocess.Popen("cmake ../backend", cwd=build_path, shell=True)
k.wait()

k = subprocess.Popen("make", cwd=build_path, shell=True)
k.wait()
