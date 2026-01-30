# Claude Conversation (b2940671)

## 👤 Human

can we implement IOSurface (for scanout?) similar to dmabuf on linux?

---

## 🤖 Assistant

This is an interesting question for your macOS Venus/Vulkan work. IOSurface is indeed macOS's equivalent to dmabuf for zero-copy buffer sharing.

Let me explore the codebase to understand the current dmabuf scanout implementation and assess IOSurface feasibility.

---

## 🤖 Assistant

→ `Task` (Explore)

---

## 🤖 Assistant

→ `Task` (Explore)

---

## 🤖 Assistant

Excellent findings! There's already IOSurface support in QEMU's `apple-gfx` device that we can reference. Let me read the existing notes and key reference implementations to design a concrete plan.

---

## 🤖 Assistant

→ `Read`: `/opt/other/qemu/notes/macos-venus-vulkan-metal.md`

---

## 🤖 Assistant

→ `Read`: `/opt/other/qemu/hw/display/apple-gfx-mmio.m`

---

## 🤖 Assistant

Now I have a clear picture. The notes document the exact problem (lines 130-132, 447-456), and apple-gfx-mmio.m shows how IOSurface integration works in QEMU. Let me write the plan.

---

## 🤖 Assistant

→ `Write`: `/Users/me/.claude/plans/fluffy-toasting-iverson.md`

---

## 🤖 Assistant

I've analyzed the codebase and written a plan. Here's the summary:

**Key Finding:** IOSurface scanout is feasible. QEMU already has IOSurface integration in `apple-gfx-mmio.m` that we can reference.

**Two approaches:**
- **Phase 1 (Readback):** After Venus renders, read pixels from Metal→CPU and display via existing Cocoa path. Works now, but has performance cost.
- **Phase 2 (Zero-copy):** Extend virglrenderer to export IOSurface handles instead of dmabuf fds. Optimal, but requires virglrenderer changes.

---

## 🤖 Assistant

→ `AskUserQuestion`

---

