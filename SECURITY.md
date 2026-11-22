# Security and leaks

## Reporting
Report security problems privately to the maintainers (GitHub private vulnerability reporting on
the repository), not in public issues. This includes:

- parser bugs triggered by untrusted input (shader containers, packages, save data, network data
  handled by the layer);
- anything in the repository that should not be there: SDK files or text, `GameOS.Xvd`, title
  binaries or assets, keys, dumps.

## If something that must not be public was committed
Do not push a "fix" commit that deletes it (the data stays in history). Instead:

1. Tell the maintainers privately with the commit and path.
2. Maintainers rewrite history with `git filter-repo --sensitive-data-removal`, force-push every
   branch and tag of the canonical repository and its mirrors.
3. Ask GitHub Support to remove cached views and pull-request refs that still reference the data.
4. Ask the owners of forks made since the commit to delete or rewrite them.

## Scope
OpenScarlett runs code from titles the user owns; the titles are trusted to the same degree as on
the console. Sandbox escapes from title code are out of scope; memory-safety bugs in the layer's
own parsers are in scope.
