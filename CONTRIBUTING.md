# Contributing to PhonePostMix

Thanks for taking the time to contribute.

## Ground rules

- **Language:** all code, comments, commit messages, issues and documentation are in English.
- **Licence:** this project is AGPL-3.0-or-later (see [LICENSE](LICENSE) and
  [`docs/adr/`](docs/adr/) for why). By contributing you agree your contribution is
  released under that licence.

## Workflow

1. Branch from `main`. Use a descriptive prefix:
   - `feat/…` new functionality
   - `fix/…` bug fixes
   - `docs/…` documentation only
   - `chore/…` build, CI, tooling
   - `refactor/…` no behaviour change
2. Commit using [Conventional Commits](https://www.conventionalcommits.org/):
   `type(scope): short imperative summary`, then a blank line and a body explaining
   *why*. Example: `feat(net): add Opus encoder on the streaming thread`.
3. Open a pull request against `main`. CI must be green.

## Building and testing

See [README.md](README.md) for the per-platform quickstart, and
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for the full development guide.

## The one rule that matters most

**Never block the audio thread.** No locks, no allocations, no system calls, no
logging, no network I/O inside `processBlock`. Everything that can block happens on a
worker thread and communicates with the audio thread through a lock-free FIFO. A pull
request that violates this will not be merged, however convenient it looks.
