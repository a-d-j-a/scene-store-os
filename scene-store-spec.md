# Scene Store Wire Format — v0

The scene store is the core of the semantic-scene engine. It owns the meaning of every native
app's visible UI. This document is the strict byte-level wire format. Everything else
(compositor, effects, search, automation, a11y, rewind) is a consumer of this protocol.
Locked before code. Nothing in this file is optional.

## 1. Goals

- Retained, versioned, semantic scene per client session.
- Deterministic op log: every record is replayed to identical scenes.
- Textures are opaque imports (DMA-BUF/shm equivalents). No pixel semantics.
- Byte-level format, little-endian, 8-byte aligned records.

## 2. Framing

Every record on the stream:

```
offset  size  field
0       4     magic           0x5343454E ("SCEN")
4       2     protocol_version 0x0000
6       2     record_type      u16 opcode (table below)
8       4     record_length   bytes of payload following header
12      4     checksum        FNV-1a 32 over bytes [0, 16+record_length);
                              the 4 checksum bytes (12..16) are zeroed at
                              compute time. Amendment 2026-08-03: coverage
                              was [0, 12+record_length), which left the
                              last 4 payload bytes unsigned; the whole
                              frame is now covered.
16      ...   payload         opcode-specific, length = record_length
```

- Connection: client connects → server sends `WELCOME` (scene_id u32, negotiated version,
  limits from section 8).
- Every record carries a monotonic `seq u64` as its first payload field. Server rejects
  non-monotonic seq. Seq is the version of the scene after that record.
- All records are appended to the session op log (server-side, append-only, checkpointed
  every 4096 records with a full scene snapshot).

## 3. Primitive types

| type       | encoding                            |
|------------|-------------------------------------|
| NodeId     | u32, per-session                    |
| TextId     | u32, stable per text/value slot     |
| Rect       | 4 x i32, absolute in session space (x, y, w, h) |
| Color      | u32 ARGB, premultiplied             |
| Role       | u16 (enum below)                    |
| Flags      | u8 bitmask (enabled=1, focusable=2, visible=4) |
| StyleRef   | u32, index into server style table  |
| EffectRef  | u32, index into server effect table |
| TextureRef | u32, server-issued opaque handle    |
| UTF-8      | u32 length + bytes, length includes no NUL |

## 4. Roles (semantic, mandatory for every node)

```
0x00 Generic    0x01 Window      0x02 Panel      0x03 Button
0x04 Checkbox   0x05 TextField   0x06 Label      0x07 List
0x08 Tree       0x09 Table       0x0A Menu       0x0B Dialog
0x0C Scrollbar  0x0D TabBar      0x0E Slider     0x0F Image
0x10 Spinner    0x11 Toolbar     0x12 StatusBar  0x13 TitleBar
0x14 Terminal   0x15 Editor      0x16 Combo      0x17 Progress
0x18 Tooltip    0x19 Popup       0x1A Group      0x1B Canvas
0x1C TextBlock  0x1D Selection   0x1E Cursor     0x1F Link
```

Unassigned opcode values: reserved. Receiving a reserved value is a protocol error:
server sends `ERROR`, closes the session. No tolerance.

## 5. Ops (record_type values)

Client → server:

```
0x0001 CreateNode    parent NodeId, id NodeId, role u16, rect Rect, flags u8
0x0002 DestroyNode   id NodeId
0x0003 SetText       id NodeId, slot TextId, utf8 UTF-8
0x0004 SetValue      id NodeId, slot TextId, utf8 UTF-8
0x0005 SetRect       id NodeId, rect Rect
0x0006 SetFlags      id NodeId, flags u8
0x0007 SetStyle      id NodeId, style StyleRef
0x0008 SetTexture    id NodeId, tex TextureRef, src Rect, blend u8, opacity u8
0x0009 SetEffect     id NodeId, effect EffectRef
0x000A Focus         id NodeId
0x000B FocusNext     step i8 (non-zero)
0x000C Present       token u64 (commit point; rendering begins on Present)
0x000D Snapshot      req_id u32  → server replies SNAPSHOT (see 6)
0x000E Search        req_id u32, term UTF-8 → server replies SEARCH_RESULT (see 6)
0x000F MacroBegin    macro_id u32 (opened for recording)
0x0010 MacroEnd      macro_id u32 (closes; replayable subsequence)
0x0011 ExecMacro     macro_id u32 (engine executes region-resolved activations)
0x0012 Capture       req_id u32 → server replies CAPTURE (scene read, not pixels)
0x0013 Ack           seq u64, token u64 (flow control: client consumed input seq)
0x0014 Ping          nonce u64 → server replies PONG
0x0015 SetInputMode  mode u8 (0=live, 1=replay, 2=record)
0x0016 Seek          target_seq u64 (rewind; valid only in replay/record mode)
```

Server → client:

```
0x8001 WELCOME       scene_id u32, version u16, limits (section 8)
0x8002 ERROR         code u16, message UTF-8
0x8003 SNAPSHOT      req_id u32, snapshot payload (6)
0x8004 SEARCH_RESULT req_id u32, count u32, then per hit: NodeId, rect Rect, role u16, TextId u32
0x8005 CAPTURE       req_id u32, seq u64, full scene dump (6)
0x8006 PONG          nonce u64
0x8007 InputPointer  seq u64, device u8, x i32, y i32, buttons u8
0x8008 InputActivate seq u64, id NodeId (engine resolved the semantic target)
0x8009 InputFocus    seq u64, id NodeId, state u8 (1=gained, 0=lost)
0x800A PresentDone   seq u64, token u64, latency_us u64
0x800B TextIndex     delta u32 count, then per entry: TextId u32, id NodeId, utf8 UTF-8
0x800C InputKey      seq u64, key_code u32, state u8, modifiers u8
0x800D ImportResult  texture_ref u32, ok u8
0x800E InputText     seq u64, utf8 UTF-8 field (OS text insertion, paste)
```

- `buttons` in InputPointer is a bitmask: 0x01 LEFT (held), 0x02 RIGHT (held),
  0x10 MIDDLE (held); 0x04 WHEEL_UP and 0x08 WHEEL_DOWN are TRANSIENT — they
  appear only on the record carrying the wheel tick and are cleared on the next
  event. Only LEFT (0x01) triggers InputActivate; wheel bits never resolve to a
  semantic node.
- InputText carries a UTF-8 field, not keycodes: the compositor's clipboard
  service (OS layer) delivers paste content to the focused session. Flow-
  controlled on the same un-acked gate as InputPointer/InputKey (dropped while
  a previous input is unacked); not region-resolved.

## 6. Snapshots and dumps (shared payload layout)

```
snapshot header:  seq u64, node_count u32, texture_count u32
per node:         id NodeId, parent NodeId, role u16, flags u8, rect Rect,
                  style StyleRef, effect EffectRef, tex TextureRef|0xFFFFFFFF,
                  text_slots u32, then per slot: TextId u32, utf8 UTF-8
per texture:      tex TextureRef, w u32, h u32, fmt u16, opaque u8
```

- SNAPSHOT = current committed scene. CAPTURE = same payload, served from the op log,
  usable at any seq after Seek.
- Search operates on committed text slots and returns ids + rects. Results are
  region-resolved: the engine, not the app, decides what the user can activate.

## 7. Semantics of the engine's ownership

- The engine resolves all pointer input to a semantic node (`InputActivate`). Clients
  never receive raw pointer events for activation, only the resolved id — for native
  semantic nodes. (Raw coordinates go to non-semantic regions only.)
- Re-theme: SetStyle stores a StyleRef into the server style table. Server-side style
  change re-lays-out and re-renders every referencing node on next Present. Style is
  not data in the client. Nothing ships this.
- Automation: MacroBegin/MacroEnd capture the op subsequence. ExecMacro replays it with
  activation resolved by the engine's region map, so it works across every native app
  with zero per-app support. This is the cross-app UI automation OS service.
- Replay: SetInputMode(1) + Seek stop the live pipeline; the server replays the op log
  from the nearest checkpoint to target_seq. Same ops, same seq → same scene. This is
  the deterministic whole-desktop test/rewind primitive.
- Ghost-crash: an app that dies is detached without destroying its scene; its nodes
  remain rendered, marked stale. On reconnect the client re-issues its ops; the engine
  diffs against the retained scene and only applies deltas. State survives the app.
- Commit vs in-flight: the committed scene is server truth; **in-flight input values are
  client truth.** A client must not re-render committed server state that would overwrite
  values the user typed but has not yet committed (cursor text, unconfirmed edits,
  mid-gesture state). Client applies a received Present's diffs only after its own
  in-flight input events for that node are acked (see Ack, section 5). This mirrors the
  rule that production server-rendered UI stacks are forced into (LiveView-class):
  surface server state only when in-flight client input is resolved. Source:
  enactment-proven behavior of that class; without it, round-trip updates roll back the
  field the user is editing.

## 8. Limits (negotiated in WELCOME)

```
max_nodes_per_session     u32   default 262144
max_text_bytes_per_slot   u32   default 1 MiB
max_text_slots_per_node   u32   default 48
max_record_length         u32   default 16 MiB
input_latency_budget_us   u64   default 16667 (one frame at 60 Hz)
```

- The server may raise limits; it never silently lowers them.
- Flow control: client must Ack each input seq before the server will deliver the
  next input record (`InputPointer`, `InputKey`, `InputText` all share one
  un-acked gate). Latency budget is a deadline, not a suggestion.

## 9. Boundaries (honest, stated, not hidden)

- TextureRef content is opaque: composited, effectable, never semantically owned.
  Browser/video/WebGL live here. There is no semantic access into texture pixels.
- The semantic scene covers native apps. That is the line, and it is the same line every
  OS draws — the difference is everything inside it.

## 10. Versioning

- v0 as specified. Next version bumps `protocol_version`; WELCOME negotiates.
- Breaking changes are new protocol versions, never silent field reinterpretations.
