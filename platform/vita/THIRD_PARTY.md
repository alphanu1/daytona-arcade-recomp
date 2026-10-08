# Vita target dependency record

The root [THIRD_PARTY.md](../../THIRD_PARTY.md) remains authoritative for
SoftFloat, ymfm and the shared runtime. This target uses those same pinned
sources fetched by `scripts/setup.py`, without copying or modifying them.
No additional third-party source is vendored by the port.

## SDL2

Upstream: https://github.com/libsdl-org/SDL (zlib license).
The VitaSDK package recipe inspected uses SDL 2.32.8, upstream tag
`release-2.32.8`, with source archive SHA-256:
`0ca83e9c9b31e18288c7ec811108e58bac1f1bb5ec6577ad386830eac51c787e`.
Recipe: https://github.com/vitasdk/packages/blob/master/sdl2/VITABUILD
(inspected recipe blob `58e053c7c4846f691bc731c392adb061c52dfefe`).

SDL2 is linked from the user's VitaSDK installation. The port does not
fetch a moving SDL branch, vendor its implementation, use PVR/PIB, or change
SDL. Headers and the linked package must come from the same SDK install.
Reference port documentation: https://github.com/libsdl-org/SDL/blob/SDL2/docs/README-vita.md

## VitaSDK

Installation and release documentation: https://vitasdk.org/
Toolchain source: https://github.com/vitasdk/vita-toolchain
SDK headers: https://github.com/vitasdk/vita-headers

The workflow pins the documented SDK image `vitasdk/vitasdk:2026.08-20260815`,
not `latest`. The SDK is a collection of separately licensed components;
retain its corresponding license files when redistributing components.
The SDK toolchain's `vita.toolchain.cmake` was inspected at blob
`1d8f2b3f0058d2be4626165439ba8774b0d38200`; its installed version is supplied
by the SDK rather than copied into this repository. CMake SELF/VPK helpers
were inspected at blob `cd1162d0845dcd1a2c98690225369a90424149d0`.

The application calls the public homebrew SceCtrl/SceIofilemgr APIs through
SDK import stubs. No proprietary Sony SDK, firmware binary, font, or shader
is added. The small menu glyphs and portable SoftFloat configuration are
project-owned code. The specialization and floating-point policy are
unchanged from the shared build.

## libvita2d (GPU-fast frontend)

Upstream: https://github.com/xerpi/libvita2d (MIT).
Source pinned at a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed.
Archive SHA-256:97b48d7955882b283d67450936e1ca04aad1549f733141d5b0fa7953cf805bbc.
CMake fetches this MIT source into the ignored build directory and compiles
it locally with its existing public homebrew shader objects. The custom
display-ring adapter has been removed; presentation uses unmodified upstream
swap_buffers and display synchronization. No shader or matrix changes.
The compiler includes math.h for the upstream JPEG loader's ceil declaration.
Upstream source and license are not edited; the license is included in the VPK.

The experimental exported-matrix adapter was withdrawn after real hardware
lost textured geometry (IMG_2856). Do not assume the host matrix model proves
the installed GXM shader/viewport behaviour. Recovery uses the original
libvita2d draw interface without replacing its matrix. Existing shader binaries
are linked from the public homebrew package; no proprietary compiler is used.

The MIT License (MIT)

Copyright (c) 2015 Sergi Granell (xerpi), xerpi.g.12@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
# ImGui menu integration

The GXM launcher links the same pinned Dear ImGui core recorded in the root
THIRD_PARTY.md (MIT). Its unmodified core provides widgets/font atlas; the
Vita input and vita2d draw adapter is project code. The VPK includes
licenses/imgui.txt. No proprietary shader compiler or additional plugin is
needed for the menu; it uses libvita2d's existing public homebrew shaders.

# vitaGL build (`--gpu-gl`)

The vitaGL build links libraries that `scripts/setup_vitagl.py` fetches at pinned
commits and builds into `extern/vitagl/` (git-ignored, never committed; their licence
files are copied to `extern/vitagl/install/licenses/`):

* vitaGL by Rinnegatamante (https://github.com/Rinnegatamante/vitaGL), LGPL-3.0;
* vitaShaRK by Rinnegatamante (https://github.com/Rinnegatamante/vitaShaRK), LGPL-3.0;
* math-neon by Lachlan Tychsen-Smith, Vita port by Rinnegatamante
  (https://github.com/Rinnegatamante/math-neon), MIT;
* SceShaccCgExt by Bythos (https://github.com/bythos14/SceShaccCgExt), **GPL-3.0**, required
  by vitaShaRK.

Without that directory the build falls back to the same libraries from the user's
VitaSDK installation. Shaders are compiled at run time by the console's
`libshacccg.suprx`, which the user installs; it is not distributed with the game. The same ImGui menu is drawn
through vitaGL by project code (`imgui_vita.h`, `gl_ui_triangles` in
`gpu_gl.cpp`). Distributing a VPK of this build statically linked to vitaGL
carries the LGPL-3.0 obligations (allow relinking with a modified vitaGL). SceShaccCgExt
is GPL-3.0: a distributed VPK that links it is a combined work under the GPL-3.0
(corresponding source to be offered). The project's own source stays BSD-3-Clause.
