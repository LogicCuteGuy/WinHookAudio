# Issue tracker: GitHub

Issues and specs live in GitHub Issues. Use gh for operations.

Resolve the repository from git remote -v (origin: https://github.com/LogicCuteGuy/WinHookAudio.git).
Do not infer a destination solely from the workspace name.

## Conventions

- Create: gh issue create --title "..." --body-file <file>
- Read: gh issue view <number> --comments
- List: gh issue list --state open --json number,title,body,labels
- Comment: gh issue comment <number> --body-file <file>
- Labels: gh issue edit <number> --add-label or --remove-label
- Close: gh issue close <number>

Use UTF-8 files for multiline bodies and preserve actual newlines.
Publishing means creating a GitHub issue.
Fetching a ticket means reading its issue, labels, and comments.

## Pull requests as a triage surface

PRs as a request surface: no.

## Wayfinding

A map is a GitHub issue labeled wayfinder:map.
Link child issues using native sub-issues, or a map task list
with a reciprocal Part of #<map> reference.

Use wayfinder:research, wayfinder:prototype, wayfinder:grilling,
or wayfinder:task for child types.

Use native issue dependencies for blockers where available.
Otherwise record Blocked by: #<number> references.
A child is available when open, unassigned, and without open blockers.
Select available children in map order.

Claim by assigning the implementing developer.
Resolve by recording the answer, closing the child, and adding
a decision summary and link to the map.
