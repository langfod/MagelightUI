---
description: "Multi-agent code review for the Magelight host, GPU driver, public ABI, views and packaging"
---

# Magelight Code Review

Run a parallel review of the changed files with specialized reviewer rubrics.

## Step 1: Scope

Check $ARGUMENTS for:
- A PR number → `gh pr diff <number>` (fetch the head: `git fetch origin <headRef>`; read
  post-change files with `git show FETCH_HEAD:<path>`)
- A branch name → `git diff <branch>...HEAD`
- File paths → review those files
- Nothing → all uncommitted changes (`git diff HEAD`, plus `git diff --cached`)

Save the diff to a file in the scratchpad and hand every agent its path.

## Step 2: Classify changed files

| Pattern | Category | Lenses |
|---------|----------|--------|
| `src/Magelight.cpp`, `src/main.cpp`, `src/include/magelight/Magelight.h` | Host | threading, bugs, simplicity |
| `src/MagelightApi4.*`, `src/MagelightApiExport.cpp`, `api/*.h` | Public ABI | abi (mandatory), threading, bugs |
| `gpu/*` | GPU driver | gpu-driver (mandatory), bugs |
| `extern/ultralight-free-sdk-*/`, `extern/appcore-ref/*`, `extern/README.md` | SDK bump | gpu-driver, packaging, docs |
| `frontend/**`, `views/**`, `assets/**`, `interface/**` | Pages / assets | frontend, packaging |
| `build.ps1`, `build2.ps1`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/**` | Build / packaging | packaging (mandatory) |
| `tools/**` | Tooling | bugs, docs |
| `README.md`, `CLAUDE.md`, `docs/**` | Docs | docs |

Skip a lens entirely when nothing in scope matches it. The **docs** lens runs whenever code
changed (it checks whether README/API comments need updating), and is skipped only for
docs-only diffs.

## Step 3: Automated checks first

Run and report before dispatching agents (failures are Critical, confidence 100):

- `powershell -ExecutionPolicy Bypass -File tools/check_stage.ps1` — namespaced runtime,
  no stock DLL names in the stage, API header in sync with SA's vendored copy (pass
  `-SaHeader <path>` when the SA checkout is known), CMake version present.
- If `api/MagelightUI_API.h` or `src/MagelightApiExport.cpp` changed: diff the struct member
  order against the export initializer order by eye AND list it in the report — this is the
  highest-value check in the repo.
- If `gpu/` changed: `git diff --stat` against the last commit and a `diff` of the touched
  function against `extern/appcore-ref/` (the reviewer needs the upstream side).

## Step 4: Dispatch reviewers in parallel

The rubrics are NOT registered subagent types. Spawn each as a **`general-purpose`** agent whose
prompt says: read the rubric file first, then review. Launch all relevant lenses in one message.

| Lens | Rubric | Reviews |
|------|--------|---------|
| threading | `.claude/agents/review-threading.md` | host, ABI, driver |
| bugs | `.claude/agents/review-bugs.md` | all code |
| abi | `.claude/agents/review-abi.md` | public header + export table + Api4 |
| gpu-driver | `.claude/agents/review-gpu-driver.md` | gpu/, SDK bumps |
| packaging | `.claude/agents/review-packaging.md` | build/stage/version/assets |
| frontend | `.claude/agents/review-frontend.md` | pages, views, frontend |
| docs | `.claude/agents/review-docs.md` | whether docs/comments must change |
| simplicity | `.claude/agents/review-simplicity.md` | all changed files |

Each agent prompt MUST be self-contained (subagents see none of this conversation):
1. The exact scope: changed-file list + the diff file path (or the `git diff`/`gh pr diff` command).
2. Full-file access: the agent MAY read whole files (and `extern/appcore-ref/` for driver diffs)
   so a guard just outside the hunk isn't reported as missing.
3. Read-only: no edits.
4. Self-verify: confirm the cited line exists and the claim holds; report confidence >= 60 only.

Each returns a JSON array then a 2-3 sentence summary:
```json
[{"agent":"threading","file":"src/Magelight.cpp","line":812,"severity":"high","confidence":88,"category":"...","description":"...","suggestion":"..."}]
```

## Step 5: Verify and synthesize

1. Collect all findings; dedupe by file+line (keep the highest confidence).
2. **Verify every Medium+ finding yourself** against the real file: the line exists, the claim
   holds in full-file context, no guard/lock/tombstone nearby already covers it. Drop what fails.
   A false Critical is worse than a missed Medium.
3. Bands by confidence (wins over the agent's own severity): Critical >= 90 (must fix before
   commit), High >= 75, Medium >= 60, below 60 skip.
4. Add a **verified-clean** note listing load-bearing things you confirmed correct (thread
   ownership of a new call, export-table order, a lock scope, a tombstone path).

## Step 6: Report

```
## Magelight Code Review

### Automated checks
| Check | Result |
|-------|--------|
| check_stage.ps1 | PASS |
| export table vs struct order | PASS (37 members) |

### Lens summary
| Lens | Findings | Critical | High | Medium |
| ... |

### Critical
**[abi] src/MagelightApiExport.cpp:96** (confidence 98)
`&ApiRaiseView` and `&ApiGetViewInfo` are swapped relative to MagelightApi4 — every consumer
calling GetViewInfo would invoke RaiseView.
→ Reorder to match the struct; add the static_assert-style ordering comment.

### High
...
### Verified clean
- ...
```

For a PR number, offer to post the report with `gh pr comment` — **ask before posting**.

Now execute the review.
