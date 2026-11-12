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

## Commit policy

- Never add `Co-authored-by`, `Generated-by`, or any other AI-attribution
  trailer to commit messages. Each commit is authored solely by the human.
- Use Conventional Commits: `feat(scope):`, `fix(scope):`, `chore:`,
  `docs:`, `refactor:`, `test:`, etc.
- Keep commits focused; do not bundle unrelated changes into one commit.
- Do not commit AI artifacts or temporary files (see `.gitignore`).
