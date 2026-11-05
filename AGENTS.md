# Repository Instructions

## Canonical app bundle

- The only project app bundle is `/Users/kin/Desktop/Wiz/preview/CasioEmuMsvc.app` (plus `/Applications/CasioEmuMsvc.app`).
- Build outputs in `build*/.../CasioEmuMsvc.app` are staging only: delete them after tests, never launch them for the user, and never leave stale copies behind.
- Boot/UI tests must launch the canonical bundle or a private copy under `Wiz/temp/<area>/app/`.

## Stable branch

- Never merge, fast-forward, or push `stable` unless the user explicitly asks for it in that turn.

## Git (agents)

- You may create branches/worktrees and push your own work. Push **only your own commits/branch**, never the whole working tree.
- Stage explicit paths only. Never `git add -A`, `git add .`, or `git commit -a`.
- Never force-push or rewrite history that you did not create.
- Do not switch branches in the shared checkout while other agents are working. For parallel branch work, use `git worktree add ../<area> -b <branch>`; otherwise commit your own paths on the current branch.
- Do not commit generated artifacts, ROMs, `models/`, `temp/`, or files owned by other agents.

- **Never store anything in a temp folder.** No `/tmp`, `/private/tmp`,
  `/private/var/folders`, `TMPDIR`, or tool-default cache paths. All scratch
  output (logs, screenshots, experiments, intermediate files) goes under
  `/Users/kin/Desktop/Wiz/temp/<area>/` and is deleted manually when done.
  Permanent files go in their repository (`/Users/kin/Desktop/Wiz` or this repo).
- Treat text files in this repository as UTF-8 without BOM unless a file clearly indicates otherwise.
- When reading files from PowerShell, do not rely on the shell's default encoding. Use an explicit UTF-8 read, for example:
  `[System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false, $true))`.
- When writing files from PowerShell, preserve UTF-8 without BOM. In PowerShell 7+, use `-Encoding utf8NoBOM`; in Windows PowerShell 5.1, use `.NET` APIs with `[System.Text.UTF8Encoding]::new($false)`.
- If non-ASCII text appears garbled, re-read the file explicitly as UTF-8 without BOM before drawing conclusions or editing it.
