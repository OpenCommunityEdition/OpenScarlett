# Contributing

Read `AGENTS.md` first: its rules apply to every change, written by people or by AI assistants.

- **Facts, not copies.** Layouts and behaviour come from the user's own SDK headers, public
  documentation or measurement, each recorded in `docs/provenance.md`. Code from other emulators
  and recompilation projects is not copied (their findings can be re-derived from upstream sources
  and recorded).
- **Licences.** `src/`, build files and `tools/xsl-run` are AGPL-3.0-or-later; tools and tests
  are MIT. Every file carries an SPDX tag.
- **No per-title code.** Fix the general case; per-title workarounds are user data
  (`docs/patches.md`), never skip lists in code.
- **Evidence.** Quote the file and offset, or call it a hypothesis. Changes to the shader
  translator come with a tier-1 kernel (`tests/gpucode/kernels`, `tools/gpucode/tier1`) where one
  can exercise them; changes that affect samples report before/after results
  (`tools/run-samples`, `tools/compare-reference`).
- **Before sending:** `tools/check-files` passes (install it as a pre-commit hook:
  `ln -s ../../tools/check-files .git/hooks/pre-commit` with `--staged` semantics via a wrapper).
  Say which GPU, driver and OS you tested on, and whether the change was AI-assisted.
