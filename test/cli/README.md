# CLI tests

Catch2 cases under `src/` cover the option layer, `formats`, platform
helpers, and end-to-end `slp` invocations. CTest labels every case
`cli`.

## Fixtures

Hand-written logs live in `fixtures/`. Keep them small and pin
timestamps so goldens stay stable across machines.

`fixtures/empty_templates/` is an empty `--template-dir` so user
templates on the developer machine cannot perturb CI.

## Golden files

Expected stdout bytes live in `golden/`. Cases tagged `[golden]`
compare against those files byte-for-byte. On failure the assertion
names the first differing offset.

Regenerate every golden after an intentional output change:

```powershell
$env:SLP_UPDATE_GOLDEN = "1"
ctest --preset release -L cli -R golden --output-on-failure
Remove-Item Env:SLP_UPDATE_GOLDEN
```

```sh
SLP_UPDATE_GOLDEN=1 ctest --preset release -L cli -R golden --output-on-failure
unset SLP_UPDATE_GOLDEN
```

Or build the helper target:

```sh
cmake --build --preset release --target slp-update-goldens
```
