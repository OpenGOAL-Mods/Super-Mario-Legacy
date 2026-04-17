# Super Mario Legacy — Build Workflow

## Building C++ changes (when editing .cpp/.h files)

Rebuild `gk` and `goalc` with the `Release-windows-clang-static` preset
(clang/llvm, static link).  The preset's build dir is `out/build/Release` —
do **not** pass `-B build`, it would create a second broken build tree.

Steps:
1. Kill any running `gk.exe` / `goalc.exe` — the binary is locked while running:
   ```bash
   tasklist | grep -iE "gk\\.exe|goalc\\.exe"
   taskkill //F //IM gk.exe
   taskkill //F //IM goalc.exe
   ```
2. Regenerate the CMake cache (picks up CMakeLists changes; no-op if up to date):
   ```bash
   cmake --preset=Release-windows-clang-static
   ```
3. Build the targets (the preset uses Ninja under the hood — that's fine, it's
   the stale-manifest case we avoid, not Ninja itself):
   ```bash
   cmake --build out/build/Release --target gk --parallel 8
   cmake --build out/build/Release --target goalc --parallel 8
   ```
4. Binaries land at `out/build/Release/bin/gk.exe` and `.../goalc.exe`.

### If Ninja errors with "manifest 'build.ninja' still dirty after 100 tries"

The cache got into a bad state (happens after some CMakeLists/preset edits).
Wipe it and reconfigure from scratch:
```bash
rm -rf out/build/Release
cmake --preset=Release-windows-clang-static
cmake --build out/build/Release --target gk --parallel 8
```
A fresh configure takes ~2 minutes; the rebuild after is full (all ~1100
objects).  If `rm -rf` complains about busy files, wait a couple seconds and
retry — VS Code's CMake Tools extension briefly locks `.cmake/api/v1/`.

## Building GOAL changes (when editing .gc files only)

No C++ rebuild needed. Use the REPL:
1. Launch `task repl` to start `goalc.exe`.
2. In the REPL, run `(mi)` then `(e)` and capture the output.
   - `(mi)` — make iso (recompile the GOAL project)
   - `(e)` — exit the REPL

To run non-interactively and capture output in one shot:
```bash
printf '(mi)\n(e)\n' | task repl 2>&1 | tail -80
```
Look for `Successfully built all N targets` at the end — that means the GOAL
recompile succeeded.  `goalc.exe` lives at
`./out/build/Release/bin/goalc.exe` (not `./build/bin/`).
