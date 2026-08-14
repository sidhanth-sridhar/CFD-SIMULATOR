#!/usr/bin/env bash
set -euo pipefail

# Verify this is a Git repository.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Error: not inside a Git repository." >&2
  echo "Run this script from the root of the CFD-SIMULATOR repository." >&2
  exit 1
fi

# A commit message is required.
if [ "$#" -ne 1 ] || [ -z "$1" ]; then
  echo "Usage: ./push.sh \"commit message\""
  exit 1
fi
MESSAGE="$1"

CURRENT_BRANCH="$(git branch --show-current)"
REMOTE_BRANCH="origin/$CURRENT_BRANCH"

echo "Checking repository status..."

# Reject force-push style situations: if the remote has commits we do not have,
# pushing would fail (or require a merge/rebase). Report and exit safely.
git fetch --quiet
if ! git merge-base --is-ancestor "$REMOTE_BRANCH" HEAD; then
  echo "Error: remote branch '$REMOTE_BRANCH' has commits you do not have." >&2
  echo "Run ./pull.sh first to integrate the remote changes, then try again." >&2
  exit 1
fi

git add -A
git commit -m "$MESSAGE"

echo "Pushing '$CURRENT_BRANCH' to origin..."
if git push origin "$CURRENT_BRANCH"; then
  echo ""
  echo "Push complete."
  echo "Commit: $(git rev-parse --short HEAD)"
else
  echo ""
  echo "Push failed." >&2
  exit 1
fi
