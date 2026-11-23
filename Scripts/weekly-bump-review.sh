#!/usr/bin/env bash
# Weekly shared-deps bump review — the human half of the approval step.
#
# The scheduled Shared-deps bump workflow (Mondays 07:00 UTC) opens PRs
# from github-actions. PRs authored by the bot don't fire pull_request
# workflows on their own: on some repos the check runs queue as
# "action_required" awaiting an admin approval; on others they never
# appear at all. This script is the one command a maintainer runs as
# their weekly pass:
#
#   Scripts/weekly-bump-review.sh            approve pending runs, report
#   Scripts/weekly-bump-review.sh --recreate recreate bot PRs from your
#                                           account (CI always fires; use
#                                           where checks never appear)
#
# In every case the human reviews the diff and clicks merge — nothing
# here merges for you.
#
# Requires: gh authenticated as a repo admin, run from the repo root.
set -euo pipefail

REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)
MODE=approve
[ "${1:-}" = "--recreate" ] && MODE=recreate

echo "== $REPO — weekly bump review ($MODE) =="
BOT_PRS=$(gh pr list --repo "$REPO" --author "app/github-actions" --state open --json number,title,headRefName --jq '.[] | "\(.number)\t\(.title)\t\(.headRefName)"')
if [ -z "$BOT_PRS" ]; then
    echo "No open automation PRs. Nothing to review."
    exit 0
fi

while IFS=$'\t' read -r NUMBER TITLE HEAD; do
    echo
    echo "-- #$NUMBER $TITLE [$HEAD]"
    gh pr diff "$NUMBER" --name-only | sed 's/^/     /' | head -8

    if [ "$MODE" = "recreate" ]; then
        git fetch origin "$HEAD" --quiet
        NEWBRANCH="chore/review-$(date +%Y%m%d)-$HEAD"
        git push origin "FETCH_HEAD:refs/heads/$NEWBRANCH" --quiet
        gh pr create --repo "$REPO" --base main --head "$NEWBRANCH" \
            --title "$TITLE" \
            --body "$(gh pr view "$NUMBER" --json body -q .body)

---
Recreated from #$NUMBER by the weekly bump review (Scripts/weekly-bump-review.sh --recreate): CI fires on human-authored PRs; the bot's never triggered workflows." \
            >/dev/null && echo "     recreated as a human PR; closing #$NUMBER"
        gh pr close "$NUMBER" --comment "Recreated from the weekly bump review so CI runs (bot PRs do not trigger workflows)." >/dev/null
        continue
    fi

    # Approve every action_required run on the PR's head branch.
    APPROVED=0
    for RUN in $(gh run list --repo "$REPO" --branch "$HEAD" --status action_required --json databaseId -q '.[].databaseId'); do
        if gh api --repo "$REPO" -X POST "actions/runs/$RUN/approve" >/dev/null 2>&1; then
            APPROVED=$((APPROVED + 1))
        fi
    done
    sleep 20
    CHECKS=$(gh pr checks "$NUMBER" --repo "$REPO" 2>&1 || true)
    if echo "$CHECKS" | grep -qE "pending|pass|fail"; then
        echo "     approved $APPROVED run(s) — checks are running."
        echo "     review and merge: $(gh pr view "$NUMBER" --json url -q .url)"
    else
        echo "     no checks appeared ($APPROVED run(s) approved). This repo's"
        echo "     workflows skip bot PRs entirely — re-run with --recreate."
    fi
done <<< "$BOT_PRS"

echo
echo "Done. Merge what CI approves; adapt frontends on the PR if a bump needs it."
