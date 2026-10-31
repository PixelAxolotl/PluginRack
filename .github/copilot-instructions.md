# PluginRack development instructions

## Required build artifact step

After every source or configuration change, build the application and copy the
fresh executable to the repository root before considering the work complete.

Run this from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1
```

The expected artifact is `PluginRack.exe` in the repository root. Report build
or copy failures explicitly; do not claim completion with a stale executable.

Do not commit with co-auther
