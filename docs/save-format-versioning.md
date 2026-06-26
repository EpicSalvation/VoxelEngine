# Save-Format Versioning Contract (`.vxc`)

This document is the versioning contract for the engine's **own** save format, the
per-chunk `.vxc` file written by `src/io/ChunkPersistence.cpp`. It exists so that
consumers (games, tools, save-migration utilities) can reason about what a bump of
the version number means, what the engine does when it meets a file it does not
recognise, and what compatibility they may and may not rely on as the engine
evolves past 1.0.

It does **not** govern the `.vox` / `.qb` interop formats (`src/io/VoxImporter`,
`QbImporter`, …). Those are third-party interchange formats with their own external
specifications; the engine does not version them. See ARCHITECTURE §9.

---

## 1. What `.vxc` is

`.vxc` is the internal, engine-owned format for **player-edited (dirty) terminal-layer
chunks** (introduced M5; see ARCHITECTURE §11). Only dirty chunks are written —
clean, generator-produced chunks regenerate deterministically on cache miss and are
never persisted. One file per `ChunkCoord`, named `c_<x>_<y>_<z>.vxc`, under a
per-world save directory managed by `persistence::WorldSave`.

It is deliberately **not a portable interchange format**: it is little-endian, uses
raw IEEE-754 floats, and targets the (all little-endian) platforms the engine runs
on. Cross-architecture portability is a non-goal — that role belongs to the
`.vox`/`.qb` path.

## 2. On-disk layout

Every `.vxc` file begins with a fixed header. All multi-byte integers are
little-endian; floats are IEEE-754.

| Field | Type | Notes |
|-------|------|-------|
| Magic | `char[4]` | `'V','X','C','K'` — rejected if it does not match |
| **Version** | `uint32` | The format version this document governs |
| `voxel_size_m` | `float64` | World identity (see below) |
| `chunk_size_voxels` | `uint32` | World identity |
| `coord.x/y/z` | `int32 × 3` | The chunk's coordinate |
| palette count | `uint32` | Number of distinct material records |
| palette records | (see §3) | `count` material records |
| run count | `uint32` | Number of RLE runs |
| runs | `uint32 × 2 × n` | `(length, palette_index)` pairs, x-fastest flat order |

The body is a deduplicated **material palette** plus a **run-length encoding** of the
per-voxel palette-index stream (most chunks hold a few materials in long runs, so
RLE over a palette is compact).

**World identity.** The `voxel_size_m` + `chunk_size_voxels` pair (`persistence::WorldIdentity`)
ties a file to the layer config that produced it. It is *not* the version, but it is
a second compatibility gate: on the strict load path a file whose identity differs
from the live world is rejected rather than silently misinterpreted (a save from a
differently-configured world is meaningless against the current one). The permissive
load path (network, §6) skips this check.

## 3. The palette record — the part that changes

Each palette record is the serialized subset of `MaterialProperties`:

```
float32  density
float32  structural_strength
float32  thermal_conductivity
float32  porosity
float32  hardness
float32  light_emission    (added in v2)
uint8    palette_index
```

The palette record is where format evolution actually happens: as the engine grows
new per-voxel material properties, they are appended here. The header, identity, coord,
and RLE framing have been **stable since M5** and are not expected to change; a change
to *them* would be a far larger version event than appending a palette field.

## 4. Version history

| Version | Milestone | Change |
|---------|-----------|--------|
| 1 | M5 | Initial format. Palette record: `density`, `structural_strength`, `thermal_conductivity`, `porosity`, `hardness` + `palette_index`. (M11 added the `decodeChunkFilePermissive` network read path, but **no layout change** — same v1 bytes.) |
| 2 | M17 (A1) | `light_emission` (`float32`) appended to the palette record, for the voxel lighting model. Version bumped 1 → 2. |

To date the format has changed exactly once, and that change was versioned correctly:
the only layout edit since M5 is the M17 `light_emission` append, which bumped the
version 1 → 2. The rule in §5.1 — **a layout change must bump the version** — is what
keeps that record clean as more fields are added.

## 5. The contract

### 5.1 What bumps the version

Bump `kVersion` (in `ChunkPersistence.cpp`) for **any change to the on-disk byte
layout** that an existing reader would parse incorrectly. In practice:

- **Appending a field to the palette record** (the common case — every new persisted
  `MaterialProperties` field). This is a layout change even though it only adds bytes,
  because the reader computes record boundaries from the field count.
- Changing the size, type, order, or meaning of any existing field.
- Changing the header, identity block, coord encoding, or RLE framing.
- Changing endianness or float representation (not anticipated).

You do **not** bump the version for changes that leave the bytes identical
(refactors, comments, equivalent encodings that produce byte-identical output). The
test suite's round-trip assertions are the guard: if encode→decode still passes and
the bytes are unchanged, no bump is needed.

When you bump, add a row to the §4 table describing the delta.

### 5.2 What the engine does on an unrecognised version

The current reader requires an **exact version match**. `decodeChunkFile` and
`decodeChunkFilePermissive` both return `nullptr` if the stored version is anything
other than `kVersion` — there is **no** built-in up-conversion of an older file and
no down-conversion of a newer one.

The consequences differ by load path:

- **Local load (`WorldSave::tryLoadChunk` → `decodeChunkFile`).** A version mismatch
  (or any malformed/foreign file) decodes to `nullptr`, which the world treats as a
  **cache miss**. The chunk is then **regenerated deterministically from the recipe**
  — the clean, generator-produced state. **The player's edits stored in that file are
  silently dropped.** The file on disk is not deleted, but it is no longer
  authoritative; the next save of that chunk overwrites it in the current version.
- **Network load (`decodeChunkFilePermissive`, §6).** A version mismatch decodes to
  `nullptr`; `NetworkManager::handleDirtyChunkData` logs `"could not decode chunk"`
  and drops the packet. The client simply does not receive that dirty chunk (it sees
  generator state). A server and client built from the same engine revision always
  agree on the version, so this only bites across mismatched builds.

This "reject → regenerate" behaviour is **safe** (no corruption, no crash, fully
deterministic) but **lossy** (edits in unreadable chunks are lost). That trade-off is
acceptable pre-1.0, where no save compatibility is promised. It is **not** an
acceptable end state for a 1.0 engine that asks players to keep worlds across
updates — see §5.3.

### 5.3 Forward-migration path (the 1.0+ commitment to plan around)

The contract a consumer should plan around as the engine matures:

1. **Newer engine, older file → migrate, don't discard.** When the reader meets a
   file whose version is *older* than `kVersion`, it should up-convert it to the
   current layout rather than rejecting it. Because the change to date has been an
   **append-only** addition of a palette field with a well-defined default
   (`light_emission` defaults sensibly on `MaterialProperties`), migration is
   mechanical: parse the file under its declared version's layout, then
   fill newly-added fields with their defaults. The natural implementation is a small
   per-version decode ladder (`decodeV1`, `decodeV2`, …) selected on the header
   version, each producing the current in-memory `Chunk`; `kVersion` files are then
   re-saved at the current version on next write. This is the work the B3 task flags
   as currently undocumented and unimplemented.
2. **Older engine, newer file → reject cleanly (current behaviour is correct).** An
   old engine cannot be expected to understand a field that did not exist when it was
   built. It must refuse the file with a clear diagnostic rather than guess — which is
   what the exact-match check already does. The only improvement wanted here is a
   *louder* signal: a logged "save is from a newer engine version (file vN > engine
   vM)" rather than a silent cache-miss/regenerate, so a downgrade does not look like
   data loss with no explanation.
3. **Version is monotonic and never reused.** A given version number describes exactly
   one byte layout for all time. Never repurpose an old number for a new layout.

Until that migration ladder is implemented, **consumers must treat an engine-version
upgrade that bumps `kVersion` as potentially edit-dropping** for any chunk not
re-saved under the new version, and should not rely on `.vxc` files surviving a
format bump. After it is implemented, the guarantee becomes: *older saves migrate
forward automatically; newer saves are refused with a clear message, never
mis-parsed.*

## 6. The network path shares the format

The multiplayer join handshake serves dirty chunks as **raw `.vxc` bytes** over the
wire (ARCHITECTURE §15): the server sends `encodeChunkFile` output and the client
decodes it with `decodeChunkFilePermissive` (which trusts the embedded identity since
the client may not yet have configured its `World`). The version gate is identical to
the local path, so everything in §5.2/§5.3 about version handling applies to the wire
as well. Practically: **a client and server must run the same `.vxc` version**, which
they do whenever they are built from the same engine revision.

## 7. Quick reference for implementers

- The version constant lives at `kVersion` in `src/io/ChunkPersistence.cpp`.
- Bump it in the same commit that changes the layout, and add a §4 row.
- Add/extend a `tests/ChunkPersistenceTest.cpp` round-trip case for the new field.
- Until the migration ladder (§5.3) lands, a bump means old `.vxc` files are
  treated as cache misses (local) or dropped (network) — never mis-parsed.
