# Building Final Foliage Reenabled

Everything is built from this source plus files from your own copy of the game. No game files are included in this repository.

## Requirements
- Windows 10/11.
- FINAL FANTASY VII REBIRTH, Steam PC build of 28 Oct 2025 (`ff7rebirth_.exe` SHA-256 `752807180c4ed919667ff0ec163046410187e873888dd75248e78aaa72e363dc`, unmodified).
- Visual Studio 2022 with "Desktop development with C++" (MSVC x64).
- Python 3.11 (standard library only).
- [repak](https://github.com/trumank/repak) 0.2.3 → `tools/repak/repak.exe`.
- [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler/releases) 1.9 → `tools/dxc/`, so that these exist:
  - `tools/dxc/bin/x64/dxc.exe`
  - `tools/dxc/bin/x64/dxcompiler.dll`
  - `tools/dxc/bin/x64/dxil.dll`
  - `tools/dxc/inc/dxcapi.h`

Run all commands from the repository root. Wherever a command contains `<game>`, use your game folder.

## 1. The DLL (`FinalFoliage.dll`)
```
dll\build.bat
```
This produces `dll\FinalFoliage.dll`. `build.bat` compiles `dll\version.rc` (the file's name/author/version metadata) and links it in. `build.bat` expects Visual Studio 2022 Community in its default folder; edit the `vcvars64.bat` path if yours is elsewhere.

### Sign the DLL (recommended)
```
powershell -ExecutionPolicy Bypass -File tools\sign_dll.ps1
```
This generates a free, self-signed code-signing certificate in your user certificate store the first time (reused afterwards), then signs and timestamps `dll\FinalFoliage.dll`. The signature is real; because it is self-signed rather than issued by a certificate authority, Windows lists the publisher as unverified. It proves the file has not been altered since you built it and gives it a named publisher, which reduces antivirus false positives on an otherwise unsigned DLL. The public certificate is written to `tools\FinalFoliage-codesign.cer` (safe to share); the private key never leaves your certificate store. For a verified publisher instead, buy a certificate from a certificate authority and sign with `signtool` — not required for a mod.

## 2. The shader pak (`zz_FinalFoliage_P.pak`)
The pak holds the game's global shader archive, with the foliage mesh shaders patched to bend plants.

1. Build the DXIL assembler. Run this from an "x64 Native Tools Command Prompt for VS 2022":
   ```
   cl /nologo /O2 /EHsc /I tools\dxc\inc tools\dxasm\dxasm.cpp /Fe:tools\dxasm\dxasm.exe ole32.lib
   ```
2. Extract the shader archive from the game:
   ```
   tools\repak\repak.exe unpack -o extract\shaders -i End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode "<game>\End\Content\Paks\pakchunk0-WindowsNoEditor.pak"
   ```
3. Disassemble the foliage mesh shaders. This writes 752 shaders to `extract\shaders\ms`.
   ```
   python tools\extract_shaders.py
   ```
4. Patch them, then reassemble and validate them. This should report 320 `MainInBasePass` and 240 `MainInPrePass` shaders OK.
   ```
   python tools\build_bend.py
   ```
5. Rebuild the archive:
   ```
   python tools\make_archive.py
   ```
6. Pack it:
   ```
   cd build
   ..\tools\repak\repak.exe pack -q --version V11 -m "../../../" pak zz_FoliageBend_P.pak
   cd ..
   ```

## 3. The release zip
```
set FF7R_GAME=<game>
python tools\make_release.py v1.0
```
This writes `release\FinalFoliageReenabled_v1.0\` and the `.zip`.

The installer script (`FinalFoliage-Setup.ps1`, generated from `tools\setup_template.ps1`) contains only the bytes it changes in `ff7rebirth_.exe`. The exe itself is never shipped.
- The changes come from `patch_wind.py`:
  - a 26-byte entry stub that calls `LoadLibraryA("FinalFoliage.dll")`;
  - the wind-distance clamp;
  - two section-size fields.
- The changes are computed against an unmodified exe. Point `FF7R_ORIGINAL_EXE` at one if the exe in `<game>` is already modified.
- The default `FinalFoliage.ini` is generated from `DEFAULT_INI` in `dll/foliage.inc` (`tools/make_default_ini.py`).

## What each part does
| Part | Files | Purpose |
|---|---|---|
| Exe patch | `patch_wind.py`, `tools/setup_template.ps1` | Loads the DLL at startup. Clamps the per-plant wind fade distance (`AnimationDecayRatio`) when plant records are uploaded to the GPU. |
| DLL | `dll/*.cpp`, `dll/*.inc` | Hooks `RHICreateUniformBuffer` and writes nearby character positions, attack gusts, per-material plant classes, wind settings and the trail into unused registers of the View uniform buffer. Detects characters touching plants (CPU copy of the plant positions), plays procedural rustles (XAudio2) and spawns the game's own Niagara effects through `UObject::ProcessEvent`. |
| Shader patch | `tools/bend_patch.py`, `tools/build_bend.py` | Inserts the bend into the DXIL of the MassiveEnvironment foliage mesh shaders. The bend reads those View registers, leans each plant away from characters and adds procedural wind to plants that have none. |
