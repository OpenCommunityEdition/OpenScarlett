# Working rules for humans and AI assistants

1. Never add SDK headers, libraries, PDBs, DLLs, documentation, `GameOS.Xvd`, or text copied from
   them. The SDK is a build-time dependency through the gitignored `sdk/` link.
2. Never add keys, decryption code, dumping tools or exploit procedures.
3. Every layout, constant or behaviour taken from a binary or header gets a provenance note in
   `docs/provenance.md` (source, method, date).
4. AI-generated code is reviewed and tested like any other; the author must be able to explain it.
5. SPDX tag in every file: AGPL-3.0-or-later for `src/`, build files and `tools/xsl-run`; MIT for
   tools and tests; patches to third-party code keep their upstream licence (`third_party/README.md`).
6. No title-specific offsets or patches in the code: per-title fixes are user data (`docs/patches.md`).
7. Never add anything that helps obtain games, SDKs, system software or keys, or that links to them.
