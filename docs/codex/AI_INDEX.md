# Generated repository navigation

`.ai/` is local navigation for coding assistants and developers. The generator
reads the current working tree, including existing tracked files and non-ignored
untracked source. It writes paths, component groups, file hashes and C++ include
relationships. It does not change trading code or deployment configuration.

Read `AGENTS.md`, the maintained handoff/architecture, the relevant README and
actual source in their established order. Generated navigation helps find those
files; current source and validation output remain authoritative.

## Refresh and check

From the repository root under WSL, using Python 3.9 or newer and Git:

```bash
python3 tools/generate_ai_index.py
python3 tools/generate_ai_index.py --check
```

Generation works without a configured build, installed application dependencies
or network access. It uses Git only to list working-tree paths and evaluate
ignore rules; it does not stage files, commit or inspect credentials.

`--check` compares all three expected outputs with the current inputs. Exit status
is zero when current, one when missing/stale and two on generation errors. It
does not modify output. Regenerate after source, build-definition or guide changes.
An unchanged refresh writes nothing. Existing unrelated local files under `.ai/`
are preserved.

## Outputs

| File | Use |
| --- | --- |
| `.ai/README.md` | Starting page, maintained guide links and component counts. |
| `.ai/components.md` | Complete included file list, grouped by component directory. |
| `.ai/index.json` | Searchable paths, language, component, normalized SHA256 hashes, literal Meson target declarations and C++ include navigation. |

For example:

```bash
rg 'execution_engine|replay_runtime|risk|accounting' .ai/components.md
rg 'research_html|included_by|include_candidates' .ai/index.json
```

The JSON schema is version 1. `source_fingerprint` hashes the sorted included
paths and per-file hashes. Hashes normalize CRLF to LF, and output contains no
absolute machine paths, commit IDs or generation timestamps. The same working
tree therefore produces the same bytes on Windows and WSL. Editing, adding or
deleting an included file invalidates the output; ignored runtime changes do not.

## Scope and limits

The maintained generator is [tools/generate_ai_index.py](../../tools/generate_ai_index.py).
Its explicit source roots, supported file types and exclusions define the scope.
Ignored files are excluded even if tracked. Deleted files, all environment files
(including example templates), credential/key filenames, symlinks, build output,
datasets, logs, dependency caches, on-hold strategies and the retired runtime are
excluded. Frozen `docs/venue/` documents, captured replay output and historical
diagnostic tools are also excluded from current navigation. Their maintained
guides still describe where to find retained evidence.

Source/configuration bodies are not copied into output. C++ includes resolve to
a same-directory file or a unique matching repository path suffix. Ambiguous
headers appear in `include_candidates`, without reverse links claiming a definite
dependency. This lightweight scan does not interpret macros, compiler include
search order or conditional compilation; it is not a compiler dependency graph.
Other languages currently receive file/component navigation rather than import
graphs. Meson target entries cover literal declarations; configured build truth
comes from Meson introspection. The index makes no strategy/readiness claims.

All generated `.ai/` files are ignored by Git. Only the generator, its focused
validation and this maintained guide belong in source control.

Run the safety/freshness fixtures under WSL:

```bash
python3 validation/ai_index_test.py
```
