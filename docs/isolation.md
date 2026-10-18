# Isolation

Generation agents must not read other submissions, evaluation fixtures or
reference implementations. They should receive only the task specification,
context document, prompt and their own writable submission directory.

## What the launchers do

`scripts/launch-round.sh` runs each agent in its own submission directory inside
the checkout. It does not hide other repository files. A 2026-10-18 correction
fixed its previously missing change of working directory.

`scripts/launch-sandboxed.sh` creates separate scratch directory trees containing
only the relevant spec, PRD, prompt and an empty submission directory, then
copies produced files back. Despite the script's name, this is **working-copy
separation, not a security sandbox**. A normal process can still access the
original repository, sibling scratch directories and anything else allowed by
its OS user. Sparse checkouts and Git worktrees also do not enforce read access.

Neither launcher alone proves isolation. Historical usage aggregates contain no
file-access audit, and the raw tool-call sessions are not published here.

## Future published rounds

Use an actual filesystem boundary, such as an isolated container or restricted
OS account with only the supplied inputs mounted. Do not mount the evaluation
checkout, sibling submissions, home directory or provider session archive into
the generation environment. Provide model credentials through the agent's
supported mechanism and keep them out of archived tool output.

Record the agent version, allowed tools, generation environment, model settings,
input Git revision and isolation mechanism in the round metadata. Preserve a
sanitized tool-access trace and original usage records if independent auditing
is a goal. Record any deviations rather than inferring compliance from an
instruction in the prompt.

Committed holdout cases support reproducible evaluation. They are public
material, not a permanently secret test set, and future model training may
include them. New rounds can use fresh, independently reviewed cases before
publishing their evaluation set.
