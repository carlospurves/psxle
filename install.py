# This script installs PSXLE
# Run:
#   python install.py <Optional: location of binaries, default is "psxle_build">
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

install_location = os.path.expanduser("~/.psxle")
psxle_bin = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else "psxle_build"

if not os.path.isdir(install_location):
    if os.path.isfile(install_location):
        fatal(install_location, "is a file. Install must be into a directory.")
    else:
        try:
            os.mkdir(install_location)
        except OSError as exc:
            if exc.errno != errno.EEXIST:
                fatal("Could not make install directory:", str(exc))
            pass

if not os.path.isdir(psxle_bin):
    fatal("PSXLE build not found... Try running \"python build.py\".")

print("PSXLE will be installed into", install_location)

raw_input("Press enter to continue...")

to_copy = ["peopsxgl/libpeopsxgl.so", "dfsound/libDFSound.so", "dfcdrom/libDFCdrom.so", "bladesio1/libBladeSio1.so", "dfinput/libDFInput.so"]

try:
    os.mkdir(install_location+"/plugins")
except OSError as exc:
    if exc.errno != errno.EEXIST:
        fatal("Could not move to install directory:", str(exc))
    pass


print("Copying executable...")
shutil.copy("./"+psxle_bin+"/gui/pcsxr", install_location+"/psxle")

for c in to_copy:
    print("Copying", c.split("/")[1])
    shutil.copy("./"+psxle_bin+"/plugins/"+c, install_location+"/plugins/"+c.split("/")[1])

print("Done.")
