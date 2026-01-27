# Venus Stable Branch Policy

## Purpose

The `venus-stable` branch maintains a stable, working version of QEMU with Venus/Vulkan support before the breaking zero-copy changes. It serves as a reliable development base.

## Branch Protection Rules

✅ **Enabled protections:**
- Require pull request reviews (1 approval minimum)
- Dismiss stale pull request approvals when new commits are pushed
- Enforce restrictions for administrators
- Prevent force pushes
- Prevent branch deletion

## Merge Policy

### ✅ ALLOWED: Merges from upstream

You may merge commits from the official QEMU upstream:
```bash
git checkout venus-stable
git fetch upstream
git merge upstream/master  # Or specific upstream commits
```

### ❌ FORBIDDEN: Merges from origin/main

**DO NOT merge from your fork's main branch** (`origin/main`), as it contains the breaking zero-copy code:
```bash
# ❌ NEVER DO THIS:
git merge origin/main
git merge main
```

### ✅ ALLOWED: Cherry-picks from main

You may cherry-pick specific safe commits from main:
```bash
git cherry-pick <commit-hash>  # Only for doc updates, safe fixes
```

## Why This Policy?

The main branch contains breaking commits from Jan 23, 2026:
- `37f2c7c205` - milestone zero-copy triangle (+322 lines breaking code)
- All subsequent zero-copy related changes

These broke Venus rendering and caused `vn_ring_submit fatal` errors. The venus-stable branch excludes this code to maintain working 273 FPS performance.

## When to Merge Back

When venus-stable is ready to merge back into main:
1. Create a pull request from `venus-stable` → `main`
2. Review thoroughly to ensure no regressions
3. Update main branch to use stable codebase
4. Archive or delete broken zero-copy code from main

## Monitoring Compliance

A GitHub Action checks pull requests to venus-stable:
- ✅ Accepts merges from upstream
- ❌ Rejects merges that include origin/main commits
- ✅ Accepts individual cherry-picks
- ✅ Accepts new development commits

See `.github/workflows/venus-stable-guard.yml` for implementation.

## Questions?

See `notes/SOLUTION-venus-stable-branches.md` for the complete story of why venus-stable exists and what it excludes.
