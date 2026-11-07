# Attic — superseded localisation scripts

These predate the catalogue-driven pipeline (`generate-from-shared.py` +
`check-po-content.py`, CI-gated by `strings-freshness.yml`):

- `gen-translations.py` — a second, STALE po generator (Apple-xcstrings-seeded,
  different header format). Running it clobbers the catalogue-driven po/ output.
- `translate-missing.py` / `translate-missing.js` / `extract-to-shared.py` —
  wrote cross-repo into ../Dasher-Android, which now owns its generator
  (Dasher-Android#82).

Do not run. Kept for archaeology; delete when comfortable.
