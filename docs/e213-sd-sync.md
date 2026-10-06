# E213 SD sync manifest

Plot & Jot-managed writing is identified explicitly by a `.sync` file in the
project directory. Ordinary folders and files are local-only.

## Safety rule

**No valid `.sync` manifest means no automatic upload.**

The firmware must not infer a Plot & Jot relationship from a directory name,
file name, or manuscript title. A malformed, unsupported, or incomplete
manifest is treated as unmanaged/local-only until the user repairs or
reconnects it.

## Suggested SD layout

```text
/novels/
  My Novel/
    .sync
    scenes/
      Opening.txt
      The Arrival.txt

/local/
  drafts/
    random-story.txt
  notes/
    ideas.txt
```

The human-readable project folder and scene filenames may change. Stable Plot &
Jot IDs in `.sync` are authoritative for synchronization.

## Version 1

```json
{
  "version": 1,
  "provider": "plot-and-jot",
  "project_id": 42,
  "project_name": "My Novel",
  "scenes": {
    "scenes/Opening.txt": {
      "scene_id": 101,
      "chapter_id": 7,
      "revision": "8e94c1..."
    }
  }
}
```

- `project_id` identifies the Plot & Jot project.
- `scene_id` identifies the Plot & Jot scene.
- `chapter_id` preserves the scene's Plot & Jot chapter relationship.
- `revision` is the last synchronized content revision used for conflict checks.
- `project_name` and filenames are display information, not sync identity.

## Future behavior

When SD support is wired in, discovery should classify directories into:
1. managed Plot & Jot projects with a valid `.sync`;
2. local-only writing without a valid manifest.

A local file becomes sync-managed only through an explicit import/link action
that creates the corresponding Plot & Jot record and records its returned IDs.
