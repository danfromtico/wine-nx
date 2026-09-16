#!/usr/bin/env python3
"""Stage everything an SD card needs for this build in one archive: the Wine
payload with its test programs, OpenTTD, the audio, OpenGL and Direct3D 9
checkpoints and what WarCraft III needs, with the WarCraft III setup
preselected in the launcher.

Each checkpoint packager runs over the previous one's stage, so the result holds
every program they stage; the build number comes from the runtime's marker.
The Need for Speed games' DLLs are staged on top, with what they import."""
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import functools
import os
import re
import shutil
import subprocess
import sys

probe = Path(__file__).resolve().parents[1]
build = probe / 'build-switch-wow64-dynarec'
tools = probe / 'tools'
marker = re.search(r'nx-wow64-dynarec-(\d+)', (probe / 'source/runtime.c').read_text()).group(1)
stage_root = build / 'full-sd-card'
stage = stage_root / 'switch/wine'

def package(script, **base):
    subprocess.run([sys.executable, str(tools / script)], check=True, env=dict(os.environ, **base))

package('package-wow64-notepad.py')
package('package-wow64-openttd.py')
shutil.rmtree(build / 'audio-sd-card', ignore_errors=True)
package('package-wow64-audio.py', WINE_NX_AUDIO_BASE=str(build / 'openttd-sd-card/switch/wine'))
package('package-wow64-opengl.py', WINE_NX_OPENGL_BASE=str(build / 'audio-sd-card/switch/wine'))
package('package-wow64-d3d9.py', WINE_NX_D3D9_BASE=str(build / 'opengl-sd-card/switch/wine'))
package('package-wow64-war3.py', WINE_NX_WAR3_BASE=str(build / 'd3d9-sd-card/switch/wine'))

# The WarCraft III stage already left the other checkpoints' READMEs out, which
# describe one checkpoint each; its own is kept next to this package's.
shutil.rmtree(stage_root, ignore_errors=True)
shutil.copytree(build / 'war3-sd-card/switch/wine', stage,
                ignore=shutil.ignore_patterns('*.log', '.DS_Store', 'BUILD-*-README.txt'))

# What the Need for Speed games import that no checkpoint stages: NFSU2's
# SPEED2.EXE, and the XtendedInput dinput8.dll in its folder (which loads the
# real dinput8.dll from syswow64), and Most Wanted's speed.exe, which adds
# d3dx9_26, with the scripts\NFS_XtendedInput.asi its ASI loader loads, which
# adds msvcp140, which loads concrt140 when it starts. quartz delay-loads ddraw
# too. Their imports, and the DLLs that exports they use forward to, come along.
NFS_DLLS = 'ddraw dinput dinput8 netapi32 shfolder tapi32 dbghelp vcruntime140 msvcp140 concrt140 xinput1_4 d3dx9_26'.split()
# Fallout New Vegas (GOG) imports xinput1_3 and d3dx9_38, and its Galaxy.dll and
# GalaxyWrp.dll import the 2012 runtimes. d3dx9 loads images through
# windowscodecs, which it delay-imports, so no import walk reaches it.
FALLOUT_DLLS = 'xinput1_3 msvcp110 msvcr110 d3dx9_38 windowscodecs'.split()
GAME_DLLS = NFS_DLLS + FALLOUT_DLLS
pe = probe / 'build-wine-wow64-pe'
toolchain = probe / 'toolchains/llvm-mingw-20260505-ucrt-macos-universal/bin'
env = dict(os.environ, PATH=f'{toolchain}:/opt/homebrew/opt/bison/bin:' + os.environ['PATH'])
syswow64 = stage / 'drive_c/windows/syswow64'
staged = {p.name.lower() for p in syswow64.iterdir()}
queue = []

def readobj(option, path):
    return subprocess.check_output([str(toolchain / 'llvm-readobj'), option, str(path)], text=True)

def dll_name(name):
    name = name.lower()
    return name if name.endswith(('.dll', '.drv')) else name + '.dll'

@functools.lru_cache(maxsize=None)
def built(name):
    assert re.fullmatch(r'[a-z0-9_-]+\.(dll|drv)', name), name
    target = f'dlls/{name.removesuffix(".dll")}/i386-windows/{name}'
    subprocess.run(['make', '-C', str(pe), '-j8', target], env=env, check=True)
    return pe / target

@functools.lru_cache(maxsize=None)
def forwards_of(name):
    return dict(re.findall(r'^  Name: (\S+)\n  ForwardedTo: ([^.\s]+)\.', readobj('--coff-exports', built(name)), re.M))

def need(name):
    # api-ms-win-* and ext-ms-* are API sets, which ntdll resolves; no file backs them.
    if name.startswith(('api-ms-', 'ext-ms-')) or name in staged:
        return
    shutil.copy2(built(name), syswow64 / name)
    staged.add(name)
    queue.append(name)

for name in GAME_DLLS:
    need(dll_name(name))
while queue:
    for block in re.findall(r'^Import \{\n(.*?)^\}', readobj('--coff-imports', syswow64 / queue.pop()), re.M | re.S):
        module = dll_name(re.search(r'Name: (.+)', block).group(1))
        if module.startswith(('api-ms-', 'ext-ms-')):
            continue
        need(module)
        symbols = set(re.findall(r'Symbol: (\S+) \(', block))
        for symbol in sorted(symbols & forwards_of(module).keys()):
            need(dll_name(forwards_of(module)[symbol]))
for name in GAME_DLLS:
    assert 'Arch: i386\n' in readobj('--file-headers', syswow64 / dll_name(name)), name

# The launcher lists every program in drive_c; target.txt only preselects one.
(stage / 'target.txt').write_text('sdmc:/switch/wine/drive_c/WarCraft III Setup/war3-setup.exe\n')
(stage / 'run-entry.txt').write_text('1\n')
# The controller stands in for a keyboard; this lists what each control sends
# and how to change it, with every line commented out so the defaults hold.
(stage / 'keys.txt').write_text('''# Keys the controller sends, one NAME=code line each, where code is a Windows
# virtual-key code in decimal or 0x form. Remove the # to change one. A and B
# are not here: they stay the left and right mouse buttons.
#
# UP=0x26      d-pad up, or the left stick pushed up
# DOWN=0x28
# LEFT=0x25
# RIGHT=0x27
# X=0x20       space
# Y=0x46       f
# L=0x09       tab
# R=0x10       shift
# ZL=0x28      down arrow, a brake in a racing game
# ZR=0x26      up arrow, the accelerator
# PLUS=0x1B    escape
# MINUS=0x09   tab
# STICKL=0x11  control
# STICKR=0x12  alt
''')
(stage / f'BUILD-{marker}-README.txt').write_text(f'''Wine-NX build {marker}: the whole SD-card payload.
Copy the switch folder to the SD card, merging folders; it replaces the runtime
NRO and the Wine payload of any earlier build.

The launcher lists the programs in drive_c. The WarCraft III setup is
preselected; WARCRAFT-III-README.txt says how to add the game and run it.

C:\\\\openttd\\\\openttd.exe draws with OpenGL on the Switch GPU (Mesa), plays sound
effects through the win32 driver, and reads openttd.args.txt next to it; putting
-v win32:no_threads there goes back to GDI drawing.
Also staged: C:\\\\pe32-opengl.exe (red, green and blue frames, then PASS and
exit_code=0x0000002a), C:\\\\pe32-audio.exe (audout playback), C:\\\\notepad.exe and
the 7zr benchmark.

Need for Speed Underground 2 and Most Wanted: the DLLs SPEED2.EXE and speed.exe
import are staged (ddraw, dinput8, netapi32, shfolder, tapi32, d3dx9_26 and what
they import), with dbghelp, msvcp140, vcruntime140 and xinput1_4 for XtendedInput:
NFSU2's dinput8.dll and Most Wanted's NFS_XtendedInput.asi. Neither executable
can be moved in memory, so start them through a forwarder set to a 32-bit
address space.

Fallout New Vegas (GOG): xinput1_3, d3dx9_38 and the windowscodecs that loads its
textures are staged, with msvcp110 and msvcr110 for Galaxy.dll and GalaxyWrp.dll.
Its executable relocates, so it needs no forwarder.

The screen: windows are now shown through OpenGL on the GPU, each in its own
layer drawn in stacking order, instead of copying their pixels straight to the
framebuffer. A program's own OpenGL (OpenTTD, Direct3D games) still takes the
whole screen while it draws; the windows come back when it stops. The log shows
"[INIT] windows shown by the OpenGL compositor" and "[NXCOMP]" lines. If windows
do not show or look wrong, put a file switch/wine/framebuffer.txt containing 1
on the SD card to go back to the framebuffer.

wine-nx-runtime.log holds the run. Its [PROGRESS] lines report OpenGL frames,
the time in eglSwapBuffers and in opengl32 calls, the megabytes Wine copies for
32-bit buffer mappings (copy_mb), whether the GPU maps the program's own pages
(pinned=1, or -1 with pin_rc when nvservices refused them), and the slowest
opengl32 calls of the last ten seconds.
''')
subprocess.run([sys.executable, str(tools / 'verify-wow64-package.py'), str(stage)], check=True)

archive = build / f'wine-nx-full-dynarec-{marker}.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as z:
    for f in sorted(stage.rglob('*')):
        if f.is_file() and f.name != '.DS_Store' and f.suffix != '.log':
            z.write(f, f.relative_to(stage_root))
        # Empty folders are places to copy things into, such as drive_c/WarCraft III.
        elif f.is_dir() and not any(f.iterdir()):
            z.write(f, f.relative_to(stage_root))
with ZipFile(archive) as z:
    assert z.testzip() is None
print(archive)
