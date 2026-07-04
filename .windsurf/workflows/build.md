---
description: Build the Black Stride engine and sandbox from the repo root.
---

# Build Workflow

1. Compile any changed HLSL shaders with `tools/compile_shaders.sh` (Git Bash) or `dxc` directly.
2. Build engine, sandbox, and stage assets using the canonical command:
   ```batch
   cmd /c "cd /d C:\dev\blackstride && build-all.bat"
   ```
3. Run the game from `bin/`:
   ```batch
   cd /d C:\dev\blackstride\bin
   sandbox.exe
   ```

The `cd /d` is required because `build-all.bat` uses relative `PUSHD`/`POPD` paths.
