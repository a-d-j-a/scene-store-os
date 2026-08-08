/*
 * scene_fmt.h — wire-format binding of scene-store-spec.md v0.
 *
 * This file is the C contract derived from the locked specification
 * (docs/scene-store-spec.md). Byte layout is authoritative: little-endian,
 * 16-byte record header, 8-byte aligned records. Nothing here is optional.
 *
 * Binding decisions (spec is authoritative; where the table is terse, the
 * binding below fixes the reading and is frozen with v0):
 *
 *  - Every client->server op payload STARTS with a monotonic seq u64.
 *  - Server records list their exact payload fields per section 5; they do
 *    NOT carry an implicit leading seq unless their listed layout has one.
 *    SNAPSHOT/CAPTURE embed the commit seq as the snapshot-header seq.
 *  - Ack: seq = the InputPointer.seq the client consumed; token = the last
 *    Present token it rendered (correlates the ack to a frame).
 *  - TextIndex payload: count u32, then `count` entries of
 *    {TextId u32, NodeId u32, UTF-8 text}.
 *  - PONG carries only the nonce (no seq).
 */
#ifndef SCENE_FMT_H
#define SCENE_FMT_H

#include <stdint.h>
#include <stddef.h>

/* ---- Framing ---------------------------------------------------------- */

#define SCENE_MAGIC          UINT32_C(0x5343454e)   /* "SCEN" LE           */
#define SCENE_PROTOCOL_V0    UINT16_C(0x0000)
#define SCENE_HEADER_SIZE    16u                     /* magic..checksum     */

/* Wire geometry is 4 x i32: (x, y, w, h) absolute in session space.     */
typedef struct scene_rect {
    int32_t  x, y, w, h;
} scene_rect;

/* Per-session node identifier; client-issued, u32. */
typedef uint32_t scene_node_id;

/* Stable, per-session identifier for a text/value slot. */
typedef uint32_t scene_text_id;

/* Server-issued opaque handles. */
typedef uint32_t scene_texture_ref;
typedef uint32_t scene_style_ref;
typedef uint32_t scene_effect_ref;

#define SCENE_NO_PARENT     UINT32_MAX
#define SCENE_NO_TEXTURE    UINT32_MAX

/* Node flags: enabled=1, focusable=2, visible=4. */
#define SCENE_FLAG_ENABLED  0x01u
#define SCENE_FLAG_FOCUSABLE 0x02u
#define SCENE_FLAG_VISIBLE  0x04u

/* ---- Roles (section 4): 0x00..0x1F, mandatory for every node --------- */
typedef enum scene_role {
    SCENE_ROLE_GENERIC   = 0x00, SCENE_ROLE_WINDOW  = 0x01,
    SCENE_ROLE_PANEL     = 0x02, SCENE_ROLE_BUTTON  = 0x03,
    SCENE_ROLE_CHECKBOX  = 0x04, SCENE_ROLE_TEXTFIELD = 0x05,
    SCENE_ROLE_LABEL     = 0x06, SCENE_ROLE_LIST     = 0x07,
    SCENE_ROLE_TREE      = 0x08, SCENE_ROLE_TABLE    = 0x09,
    SCENE_ROLE_MENU      = 0x0A, SCENE_ROLE_DIALOG   = 0x0B,
    SCENE_ROLE_SCROLLBAR = 0x0C, SCENE_ROLE_TABBAR   = 0x0D,
    SCENE_ROLE_SLIDER    = 0x0E, SCENE_ROLE_IMAGE    = 0x0F,
    SCENE_ROLE_SPINNER   = 0x10, SCENE_ROLE_TOOLBAR  = 0x11,
    SCENE_ROLE_STATUSBAR = 0x12, SCENE_ROLE_TITLEBAR = 0x13,
    SCENE_ROLE_TERMINAL  = 0x14, SCENE_ROLE_EDITOR   = 0x15,
    SCENE_ROLE_COMBO     = 0x16, SCENE_ROLE_PROGRESS = 0x17,
    SCENE_ROLE_TOOLTIP   = 0x18, SCENE_ROLE_POPUP    = 0x19,
    SCENE_ROLE_GROUP     = 0x1A, SCENE_ROLE_CANVAS   = 0x1B,
    SCENE_ROLE_TEXTBLOCK = 0x1C, SCENE_ROLE_SELECTION = 0x1D,
    SCENE_ROLE_CURSOR    = 0x1E, SCENE_ROLE_LINK     = 0x1F
} scene_role;

#define SCENE_ROLE_MAX UINT16_C(0x1F)

/* ---- Ops, client -> server (section 5) -------------------------------- */
enum {
    SCENE_OP_CREATE_NODE   = 0x0001,
    SCENE_OP_DESTROY_NODE  = 0x0002,
    SCENE_OP_SET_TEXT      = 0x0003,
    SCENE_OP_SET_VALUE     = 0x0004,
    SCENE_OP_SET_RECT      = 0x0005,
    SCENE_OP_SET_FLAGS     = 0x0006,
    SCENE_OP_SET_STYLE     = 0x0007,
    SCENE_OP_SET_TEXTURE   = 0x0008,
    SCENE_OP_SET_EFFECT    = 0x0009,
    SCENE_OP_FOCUS         = 0x000A,
    SCENE_OP_FOCUS_NEXT    = 0x000B,
    SCENE_OP_PRESENT       = 0x000C,
    SCENE_OP_SNAPSHOT      = 0x000D,
    SCENE_OP_SEARCH        = 0x000E,
    SCENE_OP_MACRO_BEGIN   = 0x000F,
    SCENE_OP_MACRO_END     = 0x0010,
    SCENE_OP_EXEC_MACRO    = 0x0011,
    SCENE_OP_CAPTURE       = 0x0012,
    SCENE_OP_ACK           = 0x0013,
    SCENE_OP_PING          = 0x0014,
    SCENE_OP_SET_INPUT_MODE = 0x0015,
    SCENE_OP_SEEK          = 0x0016,
}; /* constants only; no storage */

/* ---- Server -> client (section 5) ----------------------------------- */
enum scene_srv_op {
    SCENE_SRV_WELCOME        = 0x8001,
    SCENE_SRV_ERROR          = 0x8002,
    SCENE_SRV_SNAPSHOT       = 0x8003,
    SCENE_SRV_SEARCH_RESULT  = 0x8004,
    SCENE_SRV_CAPTURE        = 0x8005,
    SCENE_SRV_PONG           = 0x8006,
    SCENE_SRV_INPUT_POINTER  = 0x8007,
    SCENE_SRV_INPUT_ACTIVATE = 0x8008,
    SCENE_SRV_INPUT_FOCUS    = 0x8009,
    SCENE_SRV_PRESENT_DONE   = 0x800A,
    SCENE_SRV_TEXT_INDEX     = 0x800B,
    SCENE_SRV_INPUT_KEY      = 0x800C,
};

/* ---- Error codes (0x8002) -------------------------------------------- */
enum scene_errno {
    SCENE_ERR_PROTOCOL      = 0x0001,  /* reserved role/opcode/field     */
    SCENE_ERR_SEQ           = 0x0002,  /* non-monotonic / out-of-order   */
    SCENE_ERR_BAD_NODE      = 0x0003,  /* unknown node id                */
    SCENE_ERR_BAD_PARENT    = 0x0004,  /* parent does not exist          */
    SCENE_ERR_BAD_ROLE      = 0x0005,  /* role value out of range        */
    SCENE_ERR_LIMIT         = 0x0006,  /* negotiated limit exceeded      */
    SCENE_ERR_STATE         = 0x0007,  /* op invalid in current mode     */
    SCENE_ERR_CKSUM         = 0x0008,  /* frame checksum mismatch        */
    SCENE_ERR_MACRO         = 0x0009,  /* macro id unknown / state bad   */
    SCENE_ERR_INPUT_MODE    = 0x000A,  /* input in non-live mode         */
};

/* ---- Input modes (0x0015) -------------------------------------------- */
#define SCENE_MODE_LIVE   0u
#define SCENE_MODE_REPLAY 1u
#define SCENE_MODE_RECORD 2u

/* ---- Key modifiers (bitmask in INPUT_KEY payload) ------------------- */
#define SCENE_MOD_SHIFT   0x01u
#define SCENE_MOD_CTRL    0x02u
#define SCENE_MOD_ALT     0x04u
#define SCENE_MOD_SUPER   0x08u

/* ---- Key codes (Linux evdev KEY_* values) --------------------------- */
#define SCENE_KEY_ESC       1u
#define SCENE_KEY_BACKSPACE 14u
#define SCENE_KEY_TAB       15u
#define SCENE_KEY_ENTER     28u
#define SCENE_KEY_LEFT      105u
#define SCENE_KEY_RIGHT     106u
#define SCENE_KEY_UP        103u
#define SCENE_KEY_DOWN      108u

/* ---- Limits (section 8), defaulted in WELCOME ------------------------ */
typedef struct scene_limits {
    uint32_t max_nodes_per_session;       /* default 262144                */
    uint32_t max_text_bytes_per_slot;     /* default 1 MiB                 */
    uint32_t max_text_slots_per_node;     /* default 16                    */
    uint32_t max_record_length;           /* default 16 MiB                */
    uint64_t input_latency_budget_us;     /* default 16667 (one frame 60Hz)*/
} scene_limits;

#define SCENE_DEFAULT_NODES          UINT32_C(262144)
#define SCENE_DEFAULT_TEXT_BYTES     (UINT32_C(1024) * UINT32_C(1024))
#define SCENE_DEFAULT_TEXT_SLOTS     UINT32_C(16)
#define SCENE_DEFAULT_RECORD_LENGTH  (UINT32_C(16) * UINT32_C(1024) * UINT32_C(1024))
#define SCENE_DEFAULT_LATENCY_US     UINT64_C(16667)

/* ---- Record header (section 2) -------------------------------------- */
typedef struct scene_frame_header {
    uint32_t magic;        /* SCENE_MAGIC                                   */
    uint16_t version;      /* SCENE_PROTOCOL_V0                             */
    uint16_t opcode;       /* SCENE_OP_* / SCENE_SRV_*                     */
    uint32_t length;       /* payload bytes following this header           */
    uint32_t checksum;     /* FNV-1a 32 over bytes [0, 16+length); the
                              checksum field (12..16) is zeroed at compute
                              time (spec amendment 2026-08-03)            */
} scene_frame_header;

/* FNV-1a 32 over a byte range. Implemented per spec (not verified prior). */
uint32_t scene_fnv1a32(const uint8_t *data, size_t len);

/* Whole-frame validation (magic/version/length/checksum). Returns 0 if
 * the frame is byte-valid, or a negative SCENE_ERR_* code. The checksum
 * field (12..16) is treated as zero at verification, matching emit.     */
int scene_frame_check(const scene_frame_header *h, const uint8_t *frame,
                      uint32_t frame_len);

/* Encode/decode primitive helpers. All little-endian. */
void    scene_put_u16(uint8_t *p, uint16_t v);
void    scene_put_u32(uint8_t *p, uint32_t v);
void    scene_put_u64(uint8_t *p, uint64_t v);
void    scene_put_i32(uint8_t *p, int32_t v);
uint16_t scene_get_u16(const uint8_t *p);
uint32_t scene_get_u32(const uint8_t *p);
uint64_t scene_get_u64(const uint8_t *p);
int32_t scene_get_i32(const uint8_t *p);

/* UTF-8: u32 length + bytes (length excludes NUL). Returns bytes written. */
size_t  scene_put_utf8(uint8_t *p, const char *s, uint32_t len);
/* Returns payload size of a UTF-8 field (4 + len). */
size_t  scene_utf8_field_len(uint32_t len);

#endif /* SCENE_FMT_H */