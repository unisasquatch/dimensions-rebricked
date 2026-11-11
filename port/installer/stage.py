# Gathers everything the Windows installer ships into installer/stage: the port's own programs and their runtime
# libraries, nothing else.
#   stage/app:  ldport.exe, Dimensions Rebricked.exe, ldpkg.exe, SDL3.dll, nvngx_dlss.dll (+ its licence), VC++ runtime,
#               imports.tsv, fsr1/
#
# A shared setup.exe is a fresh install: it never carries anything from the PC it is built on (saves, Toy Pad
# figures, settings.ini, the shader cache, logs) or anything from the game. The game data comes from the player's own
# packages at install time, settings start at the port's defaults, and shaders are compiled on the player's PC.
import os
import shutil
import sys

here = os.path.dirname(os.path.abspath(__file__))
port = os.path.dirname(here)
project = os.path.dirname(port)
build = os.path.join(port, "build")
crt = sys.argv[1]
stage = os.path.join(here, "stage")
shutil.rmtree(stage, ignore_errors=True)
app = os.path.join(stage, "app")
os.makedirs(app)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


copy(os.path.join(build, "ldport.exe"), os.path.join(app, "ldport.exe"))
copy(os.path.join(build, "SDL3.dll"), os.path.join(app, "SDL3.dll"))
# NVIDIA DLSS runtime (NVIDIA RTX SDKs licence, shipped with its licence text).
copy(os.path.join(build, "nvngx_dlss.dll"), os.path.join(app, "nvngx_dlss.dll"))
copy(os.path.join(project, "port", "third_party", "dlss", "LICENSE.txt"), os.path.join(app, "NVIDIA_RTX_SDK_LICENSE.txt"))
copy(os.path.join(here, "build", "Dimensions Rebricked.exe"), os.path.join(app, "Dimensions Rebricked.exe"))
copy(os.path.join(here, "build", "ldpkg.exe"), os.path.join(app, "ldpkg.exe"))
copy(os.path.join(project, "docs", "imports.tsv"), os.path.join(app, "imports.tsv"))
for f in os.listdir(os.path.join(build, "fsr1")):
    copy(os.path.join(build, "fsr1", f), os.path.join(app, "fsr1", f))
for f in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
    copy(os.path.join(crt, f), os.path.join(app, f))

# Fail the build rather than ship anything personal by mistake.
banned = ("savedata", "toypad", "shadercache", "settings.ini", ".log", "pkgkeys", "keys.inc")
for root, dirs, files in os.walk(stage):
    for name in dirs + files:
        if any(b in name.lower() for b in banned):
            sys.exit("stage.py: refusing to stage %s" % os.path.join(root, name))
print("staged", stage)
