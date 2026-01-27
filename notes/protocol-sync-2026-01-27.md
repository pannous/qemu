# Venus Protocol Sync - 2026-01-27

## Summary

Cherry-picked 4 Venus protocol sync commits from upstream to venus-stable branch to match Mesa 25.2.7 protocol.

## Commits Cherry-Picked

From upstream/main to venus-stable:

1. **acaf0be7** - vkr: sync to latest protocol for v1.4.334 (Nov 26, 2025)
2. **1002c4f7** - vkr: sync protocol for sorted VkCommandTypeEXT enum defines (Nov 28, 2025)
3. **cd978d97** - vkr: sync latest protocol for more shader extensions support (Nov 28, 2025)
4. **c1c52329** - vkr: sync protocol for VK_EXT_mesh_shader support (Dec 1, 2025)

## Resolution Method

Protocol header conflicts were resolved by accepting upstream versions (--theirs):
```bash
for f in src/venus/venus-protocol/*.h; do
    if git status "$f" | grep -q "both modified"; then
        git show :3:"$f" > "$f"  # Accept upstream
    fi
done
```

## Results

### ✅ Fixed
- Protocol version now matches Mesa 25.2.7 timeframe
- No compilation errors
- Full rebuild successful

### ❌ Still Broken
- Pipeline object lookup still fails: `vkr: failed to look up object 14 of type 19`
- This indicates the issue is NOT in protocol headers
- Problem is in object tracking/registration logic

## Current virglrenderer State

```
venus-stable branch (local):
44b23c5e vkr: sync protocol for VK_EXT_mesh_shader support
99894033 vkr: sync latest protocol for more shader extensions support
d0184354 vkr: sync protocol for sorted VkCommandTypeEXT enum defines
cbe31e0d vkr: sync to latest protocol for v1.4.334
baf75ab6 Revert "codex wip"
```

## Next Investigation Steps

The pipeline lookup failure suggests:

1. **Pipeline creation may be failing silently** on the host
   - Guest creates pipeline with ID 14
   - Host creation fails but doesn't report error to guest
   - Guest tries to bind pipeline 14, host can't find it

2. **Object ID mismatch** between guest and host
   - Guest assigns ID 14 to pipeline
   - Host registers it under different ID
   - Lookup fails when guest references ID 14

3. **Venus ring protocol issue**
   - Pipeline creation command not being properly transmitted
   - Or response not being properly received

Need to:
- Enable more verbose VKR_DEBUG logging for pipeline creation
- Check if vkCreateGraphicsPipelines is being called on host
- Verify object ID assignment matches between guest/host
