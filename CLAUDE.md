# Working agreement

## Commit every significant change

Commit each significant change as you make it. Do not let uncommitted work
accrue — unstaged changes risk being lost.

- Make a separate commit per logical change, right after completing it.
- Use a single-line commit message of at most 80 characters
- Don't add co-authored by lines: just a single line commit message
- Build/verify before committing when practical.

For incremental updates to a change that has not yet been pushed to GitHub,
prefer amending the existing commit over stacking new commits, so a single unit
of work stays a single commit. Intermediate states remain recoverable via
`git reflog`. Once a commit is pushed, do not amend it — add a new commit.

## Always pass CI before pushing to origin

Never push to `origin` until the exact commit being pushed has passed the CI
checks locally. GitHub's `main` allows the push through, so a red build is only
caught after the fact — reproduce it first.

CI (`.github/workflows/ci.yml`) runs `make setup-clang-format` then `make check`
on `ubuntu-latest`. Reproduce it faithfully — the pinned clang-format and GCC
have caught breakage that Apple's toolchain did not:

- Preferred: on the Ubuntu VM reachable via `ssh orb`, do a fresh `git clone` of
  the local repo, `git checkout` the commit to be pushed, then run
  `make setup-clang-format && make check` (with `LANG=C.UTF-8 LC_ALL=C.UTF-8`).
  A clone rather than the shared working tree keeps the check honest and off the
  local `build/`.
- At minimum, run `make check` locally with the pinned clang-format.

Push only when it is green. After pushing, confirm the real GitHub run also
passed (`gh run watch`), since the local reproduction can still diverge from the
runner.

## Keep memory in sync with substantial commits

After each substantial commit, check whether the change makes any existing
memory inaccurate or incomplete, and whether it introduces something worth
remembering. Update the affected memories or save new ones accordingly.

## Leave stray files alone

For stray/untracked files you didn't create (scratch output, temp files), just
leave them be: don't commit them, don't delete them, and don't add them to
`.gitignore`.

## Demo every significant user-facing change in the README

For any significant change to user-facing functionality (a new rendered element,
a new flag, a new program behavior), add a demonstration of it to `README.md`.
The README doubles as a live sample document — rendering it with `mdcat` should
show the feature in action — so a new capability isn't done until it has a demo
there.

## Keep the terminal-graphics field guide current

`docs/TERMINAL-GRAPHICS.md` is the nerd-focused companion to the README: the
measured terminal behaviour, the dead ends, and the ground truth behind image
placement, paging, table alignment, and remote (SSH/Kitty) rendering.

When a significant architectural change shifts that ground truth — a new graphics
protocol, a different placement/paging strategy, a new terminal added to the
support matrix, or a probe that overturns a documented belief — update the
relevant section of that doc in the same change. New probe scripts under `tools/`
that establish a durable fact belong in its probe table too.

## Canonical definition of MarkDown

Refer to https://github.github.com/gfm for the specification of the MarkDown format.
