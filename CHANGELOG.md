# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Apache-2.0 license, `NOTICE`, `SECURITY.md`, `CODE_OF_CONDUCT.md` and
  `CONTRIBUTING.md`.
- GitHub Actions CI: firmware build in `espressif/idf:v5.5.5`, the vendored
  `tcp_transport` drift check, and the two host harnesses (`host/run.sh`,
  `host/prompt.sh`) as gates.
- A release workflow that publishes a single flashable `deepgram_agent-merged.bin`
  on a `v*` tag.
- `.editorconfig`, `.clang-format`, issue and pull-request templates.

### Changed

- `README.md` is now a landing page; the engineering detail moved into `docs/`
  unchanged. Investigation notes moved to `docs/notes/`.
- `components/tcp_transport/check-patch.sh` decides pass/fail on SHA-256 digests
  (`baseline.sha256`) instead of on committed diff text, which was not portable
  between Apple/FreeBSD `diff` and GNU `diffutils` and so could not run in CI.
  `local.patch` is unchanged and remains the human-readable record.

## [0.1.0] - unreleased

First tagged release. See the git history for everything before this file existed.
