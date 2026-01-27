# Branch Protection Setup Complete ✅

## What Was Configured

### 1. GitHub Branch Protection Rules (Applied)

Both repositories now have active protection on `venus-stable`:

**QEMU: pannous/qemu/venus-stable**
- ✅ Require 1 pull request review before merging
- ✅ Dismiss stale reviews when new commits are pushed
- ✅ Enforce restrictions for administrators
- ✅ Block force pushes
- ✅ Block branch deletion

**virglrenderer: pannous/virglrenderer/venus-stable**
- ✅ Require 1 pull request review before merging
- ✅ Dismiss stale reviews when new commits are pushed
- ✅ Enforce restrictions for administrators
- ✅ Block force pushes
- ✅ Block branch deletion

### 2. Merge Policy Documentation (Created)

**Files created:**
- `.github/VENUS_STABLE_POLICY.md` (both repos)

Clearly documents:
- ✅ ALLOWED: Merges from upstream (official QEMU/virglrenderer)
- ❌ FORBIDDEN: Merges from origin/main (contains broken zero-copy)
- ✅ ALLOWED: Cherry-picks of safe commits
- Why this policy exists
- How to comply

### 3. GitHub Action Guards (Created)

**Files created:**
- `.github/workflows/venus-stable-guard.yml` (both repos)

Automatically checks every PR to venus-stable for:
- ❌ Rejects PRs containing origin/main commits
- ❌ Rejects PRs with known breaking commit hashes:
  - QEMU: `37f2c7c205`, `e3601ea0d0`, `ad7ec23a30`, `1352959d65`
  - virglrenderer: `f48b5b19`, `9b0a9ab2`, `19cf9e77`, `0018e310`
- ⚠️  Warns about merge commits mentioning "main"
- ✅ Allows clean development commits
- ✅ Allows upstream merges

## How It Works

### Before This Setup
```bash
# Nothing prevented this dangerous merge:
git checkout venus-stable
git merge main  # ❌ Would break everything!
git push
```

### After This Setup
```bash
# Direct push blocked by branch protection:
git checkout venus-stable
git merge main
git push  # ❌ ERROR: Branch protection requires PR

# Create PR instead:
gh pr create --base venus-stable --head my-feature

# GitHub Action runs:
# ❌ FAIL: "This PR contains commits from origin/main"
# PR is blocked until you fix it
```

### Proper Workflow
```bash
# ✅ Merge from upstream (official QEMU)
git checkout venus-stable
git fetch upstream
git merge upstream/master
gh pr create --base venus-stable

# GitHub Action runs:
# ✅ PASS: "No origin/main commits detected"
# PR can be merged after review

# ✅ Cherry-pick safe commits
git checkout venus-stable
git cherry-pick abc123def  # Some doc update from main
gh pr create --base venus-stable

# GitHub Action runs:
# ✅ PASS: "No breaking commits detected"
# PR can be merged after review
```

## Limitations

### What GitHub Branch Protection CAN'T Do

GitHub's branch protection rules **cannot** distinguish between:
- Merges from `upstream/master` (✅ good)
- Merges from `origin/main` (❌ bad)

They're both just "a merge commit" to GitHub.

### What Our GitHub Action DOES

The custom GitHub Action we created **can** detect:
- Whether a PR's commits are ancestors of `origin/main`
- Specific breaking commit hashes
- Merge commits that mention "main" in the message

This provides an additional layer of protection beyond basic branch rules.

## Files to Commit

The following files need to be committed to venus-stable branches:

**QEMU:**
```bash
cd /opt/other/qemu
git checkout venus-stable
git add .github/VENUS_STABLE_POLICY.md
git add .github/workflows/venus-stable-guard.yml
git commit -m "feat: Add branch protection policy and guard"
git push
```

**virglrenderer:**
```bash
cd /opt/other/virglrenderer
git checkout venus-stable
git add .github/VENUS_STABLE_POLICY.md
git add .github/workflows/venus-stable-guard.yml
git commit -m "feat: Add branch protection policy and guard"
git push
```

## Testing the Protection

### Test 1: Try to merge main (should fail)
```bash
git checkout -b test-bad-merge venus-stable
git merge main
gh pr create --base venus-stable --head test-bad-merge
# GitHub Action should fail with:
# "❌ ERROR: This PR contains commits from origin/main"
```

### Test 2: Try to merge upstream (should pass)
```bash
git checkout -b test-good-merge venus-stable
git merge upstream/master
gh pr create --base venus-stable --head test-good-merge
# GitHub Action should pass:
# "✅ No origin/main commits detected"
```

## Summary

✅ **Branch protection enabled** on GitHub
✅ **Policy documented** in .github/VENUS_STABLE_POLICY.md
✅ **Automated guards** via GitHub Actions
✅ **Both repositories configured** (QEMU and virglrenderer)

The venus-stable branches are now protected from accidentally merging the broken zero-copy code from main. They can only merge from upstream or accept cherry-picked commits that don't contain the breaking changes.

## Next Steps

1. Commit the .github files to venus-stable branches (requires git commit)
2. Test the protection by creating a test PR
3. Continue development on venus-stable with confidence
4. When ready, create PR from venus-stable → main to replace main with stable code
