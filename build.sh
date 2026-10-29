#!/usr/bin/env bash
# Cross-compiles FF16VR (dinput8.dll) from Linux using clang-cl + lld-link
# against the MSVC/Windows SDK sysroot from toolchain.zip.
#   TOOLCHAIN=<unzipped toolchain.zip>  LLVM=<llvm 22+ root>  ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
: "${TOOLCHAIN:?set TOOLCHAIN to the unzipped toolchain.zip folder}"
: "${LLVM:?set LLVM to an LLVM 22+ install (needs bin/clang-cl, bin/lld-link)}"
SDK=10.0.28000.0
OUT=build
mkdir -p "$OUT"

# The SDK was copied off Windows with mixed-case names; give clang/lld a
# case-insensitive view of it.
python3 - "$TOOLCHAIN" "$SDK" > "$OUT/vfs.yaml" <<'PY'
import sys, os, json
t, sdk = sys.argv[1], sys.argv[2]
dirs = ["include", f"Include/{sdk}/ucrt", f"Include/{sdk}/um", f"Include/{sdk}/shared",
        "lib/x64", f"Lib/{sdk}/um/x64", f"Lib/{sdk}/ucrt/x64"]
roots = []
for d in dirs:
    for dp, _, fn in os.walk(os.path.join(t, d)):
        roots.append({"name": dp, "type": "directory",
                      "contents": [{"name": f, "type": "file", "external-contents": os.path.join(dp, f)} for f in fn]})
print(json.dumps({"version": 0, "case-sensitive": False, "roots": roots}))
PY
VFS="$PWD/$OUT/vfs.yaml"

CFLAGS=(--target=x86_64-pc-windows-msvc /std:c++20 /EHsc /O2 /MT /Zi /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE
        -Xclang -ivfsoverlay -Xclang "$VFS"
        /imsvc"$TOOLCHAIN/include" /imsvc"$TOOLCHAIN/Include/$SDK/ucrt" /imsvc"$TOOLCHAIN/Include/$SDK/um"
        /imsvc"$TOOLCHAIN/Include/$SDK/shared" /Ithird_party/openxr/include
        /Ivrkit /Ivrkit/third_party/imgui /DVRKIT_IAT_HOOKS
        -Wno-unused-command-line-argument -Wno-microsoft-cast)

OBJS=()
for src in src/*.cpp vrkit/*.cpp; do
  obj="$OUT/$(basename "${src%.cpp}").obj"
  "$LLVM/bin/clang-cl" "${CFLAGS[@]}" /c "$src" /Fo"$obj"
  OBJS+=("$obj")
done
# Dear ImGui (third party): compiled once, rebuilt only when missing
mkdir -p "$OUT/imgui"
for src in vrkit/third_party/imgui/imgui*.cpp; do
  obj="$OUT/imgui/$(basename "${src%.cpp}").obj"
  [ -f "$obj" ] || "$LLVM/bin/clang-cl" "${CFLAGS[@]}" /W0 /c "$src" /Fo"$obj"
  OBJS+=("$obj")
done

"$LLVM/bin/lld-link" /nologo /dll /machine:x64 /vfsoverlay:"$VFS" /def:src/exports.def /out:"$OUT/dinput8.dll" \
  /pdb:"$OUT/dinput8.pdb" /debug /opt:ref /opt:icf \
  /libpath:"$TOOLCHAIN/lib/x64" /libpath:"$TOOLCHAIN/Lib/$SDK/um/x64" /libpath:"$TOOLCHAIN/Lib/$SDK/ucrt/x64" \
  "${OBJS[@]}" kernel32.lib user32.lib d3d12.lib dxgi.lib d3dcompiler.lib

echo "Built $OUT/dinput8.dll"
