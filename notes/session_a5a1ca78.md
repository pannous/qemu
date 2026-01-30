# Claude Conversation (a5a1ca78)

## 👤 Human

Git track-only master from upstream.

---

## 🤖 Assistant

I'll help you configure git to only track the master branch from upstream. Let me first check the current remote configuration.

---

## 🤖 Assistant

→ `Bash`: `git remote -v`

---

## 🤖 Assistant

→ `Bash`: `git branch -r`

---

## 🤖 Assistant

I can see upstream is tracking many branches. I'll configure it to only track the master branch.

---

## 🤖 Assistant

→ `Bash`: `git config remote.upstream.fetch '+refs/heads/master:refs/remotes/upstream/master'`

---

## 🤖 Assistant

→ `Bash`: `git remote prune upstream`

---

## 🤖 Assistant

→ `Bash`: `git branch -r | grep upstream`

---

## 🤖 Assistant

The branches are still cached locally. Let me remove them manually:

---

## 🤖 Assistant

→ `Bash`: `git branch -r | grep 'upstream/' | grep -v 'upstream/master' | grep -v 'upstream/HEAD' | xargs -r gi...`

---

## 🤖 Assistant

→ `Bash`: `git branch -r | grep upstream`

---

## 🤖 Assistant

Perfect! Git is now configured to only track the master branch from upstream. Let me verify the configuration:

---

## 🤖 Assistant

→ `Bash`: `git config --get remote.upstream.fetch`

---

## 🤖 Assistant

Done! Git is now configured to only track the master branch from upstream:

✓ Configured fetch refspec: `+refs/heads/master:refs/remotes/upstream/master`
✓ Removed all other upstream remote-tracking branches  
✓ Only `upstream/master` and `upstream/HEAD` remain

Future fetches from upstream will only update the master branch, keeping your remote branch list clean.

---

