# Core patches

Changes made to the AzerothCore tree itself, rather than to a module.

There is one, and it is cosmetic. Everything else Sanctuary does lives in `../modules/`
against an otherwise unmodified core — which is why this repository does not need to be a
full fork of AzerothCore.

## `0001-branding-companyname.patch`

`src/cmake/revision.h.in.cmake` — sets `AC_COMPANYNAME_STR` to `"Sanctuary"` so the built
`worldserver.exe` and `authserver.exe` carry the realm name in their Windows file
properties. `AC_LEGALCOPYRIGHT_STR` is left pointing at AzerothCore, unchanged.

Apply against a checkout of <https://github.com/azerothcore/azerothcore-wotlk>:

```bash
git apply /path/to/sanctuary-modules/core-patches/0001-branding-companyname.patch
```

The patch was generated against upstream `master` at commit `2fed8b96e`. It touches one
line, so it will almost certainly still apply cleanly to a newer master; if it does not,
the change is small enough to make by hand.
