#!/usr/bin/env bash
#
# sync-from-core.sh — copy the Sanctuary modules out of the AzerothCore working tree
# into this repository, check them, and optionally commit and push.
#
# Why this exists
# ---------------
# The modules are developed in azerothcore-wotlk/modules. That tree's .gitignore
# contains "/modules/*", so a module edit never shows up in its `git status`. This
# repository is a *copy*, so it only knows what was last synced into it. Both trees
# can therefore report "clean" while the published source — the source the site
# offers under the AGPL — is weeks out of date, with nothing anywhere warning you.
#
# Usage
# -----
#   tools/sync-from-core.sh                  sync, show what changed, stop
#   tools/sync-from-core.sh -m "message"     sync, then commit and push
#   tools/sync-from-core.sh -m "msg" -P      sync and commit, but do not push
#   tools/sync-from-core.sh -n               report drift only, change nothing
#
# The core checkout defaults to E:/WoWServer/azerothcore-wotlk and can be overridden:
#   CORE=/some/other/azerothcore-wotlk tools/sync-from-core.sh
#
# Exit codes: 0 nothing to do or success, 1 a check failed, 2 drift found under -n.

set -euo pipefail

CORE="${CORE:-E:/WoWServer/azerothcore-wotlk}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MESSAGE=""
DRY_RUN=0
PUSH=1

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--message) MESSAGE="${2:-}"; shift 2 ;;
        -n|--dry-run) DRY_RUN=1; shift ;;
        -P|--no-push) PUSH=0; shift ;;
        -h|--help)    sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\033[31mFAILED: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Checks before anything is touched
# ---------------------------------------------------------------------------

[ -d "$CORE/modules" ] || fail "no modules directory at $CORE/modules (set CORE=)"
[ -d "$REPO/.git" ]    || fail "$REPO is not a git repository"

# A dirty repo means an earlier sync was left uncommitted. Continuing would fold
# those changes into this run's commit under a message that does not describe them.
if [ -n "$(git -C "$REPO" status --porcelain)" ] && [ "$DRY_RUN" -eq 0 ]; then
    git -C "$REPO" status --short
    fail "repository has uncommitted changes — commit or discard them first"
fi

# ---------------------------------------------------------------------------
# What is about to change
# ---------------------------------------------------------------------------

# `Only in $CORE/modules/: X` lines are upstream AzerothCore's own files — CMakeLists.txt,
# ModulesPCH.h, create_module.sh and friends. They are deliberately not republished
# here, so they are filtered out rather than treated as drift — unless X is a mod-*
# folder, which is a new module and exactly the drift this exists to catch. The first
# version filtered every such line, so a new module never registered at all.
#
# A module cloned from its own repository carries a .git, excluded here and in the copy
# below: git would otherwise stage the folder as an embedded repository — a bare pointer
# with none of the module's files in it, published as if it were the source.
DRIFT="$(diff -rq --strip-trailing-cr -x .git "$CORE/modules/" "$REPO/modules/" 2>/dev/null \
         | awk -v top="Only in $CORE/modules/: " \
               'index($0, top) == 1 && substr($0, length(top) + 1, 4) != "mod-" { next } { print }' \
         || true)"

if [ -z "$DRIFT" ]; then
    say "In sync — the published modules match the working tree."
    exit 0
fi

say "Drift found:"
printf '%s\n' "$DRIFT"

if [ "$DRY_RUN" -eq 1 ]; then
    say "Dry run — nothing was changed."
    exit 2
fi

# ---------------------------------------------------------------------------
# Sync
# ---------------------------------------------------------------------------

# Delete first so a module removed upstream is removed here too; a plain copy would
# leave it behind and quietly republish something that no longer exists.
say "Syncing modules/"
rm -rf "${REPO:?}/modules"/mod-*
( cd "$CORE/modules" && for d in mod-*/; do
      cp -r "$d" "$REPO/modules/"
      rm -rf "$REPO/modules/${d%/}/.git"
  done )

git -C "$REPO" add -A modules

# ---------------------------------------------------------------------------
# Refuse to publish secrets
# ---------------------------------------------------------------------------
#
# This repository is public and is the source offer the website advertises. A
# credential reaching it is not a mistake that can be taken back by a later commit,
# so the scan gates the commit rather than reporting afterwards. Module configs are
# meant to ship as .conf.dist templates holding placeholders only.

say "Scanning for credentials"
SECRETS="$(git -C "$REPO" diff --cached --no-color \
    | grep -inE 'Gengu|BotAdmin|MTU0Mzkw|ZaJuFPj8|Uid=root|Pwd=[A-Za-z0-9!]|BotToken|ClientSecret|-----BEGIN|[0-9]{1,3}(\.[0-9]{1,3}){3}' \
    | grep -viE '127\.0\.0\.1|0\.0\.0\.0|255\.255|change-me' || true)"

if [ -n "$SECRETS" ]; then
    printf '%s\n' "$SECRETS" | head -20
    git -C "$REPO" reset -q
    fail "possible credential or private address in the changes — nothing was committed"
fi

BADFILES="$(git -C "$REPO" diff --cached --name-only | grep -iE '\.(pem|key|env|pfx|p12|exe|dll|zip)$' || true)"
if [ -n "$BADFILES" ]; then
    printf '%s\n' "$BADFILES"
    git -C "$REPO" reset -q
    fail "files of a type that must not be published — nothing was committed"
fi

LIVECONF="$(git -C "$REPO" diff --cached --name-only | grep -E '\.conf$' || true)"
if [ -n "$LIVECONF" ]; then
    printf '%s\n' "$LIVECONF"
    git -C "$REPO" reset -q
    fail "a live .conf is staged — only .conf.dist templates belong here"
fi

echo "clean"

# ---------------------------------------------------------------------------
# The core patch, which is easy to forget
# ---------------------------------------------------------------------------
#
# core-patches/ describes the changes made to the AzerothCore tree itself. It is not
# produced by the module sync, so it goes stale silently if the core is ever touched
# again. Warn rather than rewrite: regenerating it is a judgement about what belongs
# in the published patch set, not a mechanical step.

if git -C "$CORE" rev-parse --verify -q sanctuary >/dev/null 2>&1; then
    CORE_NOW="$(git -C "$CORE" diff origin/master...sanctuary -- . 2>/dev/null | grep -v '^index ' || true)"
    CORE_WAS="$(cat "$REPO"/core-patches/*.patch 2>/dev/null | grep -v '^index ' || true)"
    if [ "$CORE_NOW" != "$CORE_WAS" ]; then
        say "Warning: core-patches/ no longer matches the core's sanctuary branch."
        echo "  Regenerate with:"
        echo "    git -C \"$CORE\" diff origin/master...sanctuary > \"$REPO/core-patches/0001-branding-companyname.patch\""
        echo "  Then review it and include it in this commit."
    fi
fi

# ---------------------------------------------------------------------------
# Commit and push
# ---------------------------------------------------------------------------

say "Staged:"
git -C "$REPO" diff --cached --stat | tail -20

if [ -z "$MESSAGE" ]; then
    say "Not committed — no message given."
    echo "  The changes are staged. Commit them with:"
    echo "    git -C \"$REPO\" commit -m \"...\" && git -C \"$REPO\" push origin main"
    echo "  or re-run this script with -m \"...\"."
    exit 0
fi

git -C "$REPO" commit -q -m "$MESSAGE"
say "Committed: $(git -C "$REPO" log --oneline -1)"

if [ "$PUSH" -eq 1 ]; then
    say "Pushing"
    git -C "$REPO" push origin main
    say "Published: https://github.com/play-sanctuary/sanctuary-modules"
else
    say "Not pushed (-P). Push with: git -C \"$REPO\" push origin main"
fi
