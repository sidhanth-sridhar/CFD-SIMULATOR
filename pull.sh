#!/usr/bin/env bash
set -euo pipefail

# Verify this is a Git repository.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Error: not inside a Git repository." >&2
  echo "Run this script from the root of the CFD-SIMULATOR repository." >&2
  exit 1
fi

echo "Pulling latest changes from GitHub..."

# If the working tree is dirty, rebasing could destroy local work.
if ! git diff --quiet && git diff --cached --quiet; then
  echo "Error: you have uncommitted changes." >&2
  echo "Stash or commit them first (e.g. git stash), then run ./pull.sh again." >&2
  exit 1
fi

if git pull --rebase; then
  echo ""
  echo "Pull complete."
else
  echo ""
  echo "Pull failed." >&2
  echo "If there are conflicts, resolve them manually, then run: git rebase --continue" >&2
  exit 1
fi
