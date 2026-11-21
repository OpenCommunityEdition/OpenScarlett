# User patches

The layer can apply byte patches that **you** keep next to a title's executable. It loads them
while `d3d12_xs.dll` initialises, before the title's own code runs, so the executable on disk is
never modified. The project ships no patches for any title; patch files are user data, like
save games, and stay out of this repository.

## Where

In the directory of the title executable, the first file found of:

1. `<executable name without .exe>.patches.txt` (e.g. `Game.patches.txt`)
2. `patches.txt`

## Format

```text
# Comments start with '#', anywhere on a line.

[Widescreen fix]                              # a named group
0x14012AB30: 39 8E E3 3F -> 26 B4 17 40        # virtual address: original bytes -> new bytes
0x14012AB40: 74 0C -> EB 0C

[Skip intro] off                              # 'off' (or 'disabled') keeps a group inactive

[Using RVAs]
rva 0x12AB50: 0F 84 -> 90 E9                   # 'rva' addresses are relative to the image base
```

- **Addresses** are hexadecimal virtual addresses as a disassembler shows them (relative to the
  executable's preferred image base, read from the file), or RVAs with the `rva` prefix.
- **Bytes** are hexadecimal, spaces optional. The original and new byte strings must have the
  same length.
- **Groups** apply all-or-nothing: every line's original bytes must match the loaded image, or
  the whole group is skipped with a warning naming the first mismatching line. A patch written
  for a different build of a title therefore cannot land on the wrong code.
- Lines before the first `[group]` form an unnamed group.
- Code and data can both be patched (pages are made writable for the write and restored).

The log reports every group (`[xsl:patches:...]`): applied, skipped (mismatch) or disabled, and
a parse error stops all patching for that file.

## Notes

- Patches are applied once at start-up, to the main executable only.
- Keep patches small and documented; the original bytes double as a version check.
- `tests/synthetic/patch_smoke.cpp` exercises the format (data, code, RVA, mismatch, disabled).
