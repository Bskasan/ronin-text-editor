// buffer.h — a text buffer: gap buffer of raw bytes (treated as UTF-8) with an incremental
// line index, file load / save. Plain C; files go through the os_* functions of platform.h.
//
// Positions are byte offsets into the logical text. Every modification goes through
// buffer_replace; nothing else writes buffer memory (loading fills a fresh buffer).

#ifndef BUFFER_H
#define BUFFER_H

#define BUFFER_TEXT_RESERVE GB(2) // text capacity; the line index reserves 4 bytes per text byte
#define BUFFER_META_RESERVE MB(1) // the Buffer struct, path and name
#define BUFFER_MAX_FILE_SIZE GB(1)

typedef enum BufferEncoding {
    BUFFER_UTF8,
    BUFFER_UTF8_BOM,
    BUFFER_UTF16LE,
    BUFFER_UTF16BE,
} BufferEncoding;

typedef enum BufferEol {
    BUFFER_EOL_LF,
    BUFFER_EOL_CRLF,  // stored as LF, written as CRLF
    BUFFER_EOL_MIXED, // stored and written byte for byte
} BufferEol;

typedef struct Buffer {
    Arena meta; // holds this struct, path and name

    // Text: [0, gap_start) and [gap_end, text_cap) of `text`. text_cap is the committed size.
    u8 *text;
    i64 text_reserved;
    i64 text_cap;
    i64 gap_start, gap_end;

    // Line index: the positions of the '\n' bytes, in their own gap array paired with the text
    // gap. [0, nl_front) are absolute positions of the newlines before the text gap;
    // [nl_back, nl_cap) are distances from the end of the text (size - pos) of the newlines
    // after it, in increasing position order. An edit at the gap leaves the back entries valid.
    u32 *nl;
    i64 nl_reserved; // entries
    i64 nl_cap;      // committed entries
    i64 nl_front, nl_back;

    String8 path; // full, normalized; empty when not visiting a file
    String8 name;
    BufferEncoding encoding;
    BufferEol eol;
    b32 modified;
    b32 read_only;
    b32 inhibit_read_only; // set around program-made edits of read-only buffers (build output)
    u64 edit_count;
    i64 file_size;  // as of the last load or save
    u64 file_time;  // last write time, same
} Buffer;

Buffer *buffer_create(String8 name); // NULL if the address space cannot be reserved
Buffer *buffer_create_reserve(String8 name, i64 text_reserve);
b32     buffer_destroy(Buffer *buf); // false if some memory could not be released

i64 buffer_size(Buffer *buf);
i64 buffer_line_count(Buffer *buf); // newlines + 1

// Replaces [start, end) with `text`. Insert = empty range, delete = empty text. Returns false,
// leaving the buffer unchanged, for a read-only buffer (unless inhibit_read_only), an invalid
// range, or when the result would not fit (capacity or commit failure).
b32 buffer_replace(Buffer *buf, i64 start, i64 end, String8 text);

i64 buffer_line_start(Buffer *buf, i64 line); // line clamped to [0, line_count)
i64 buffer_line_end(Buffer *buf, i64 line);   // excluding the '\n'
i64 buffer_line_of(Buffer *buf, i64 offset);  // the line containing offset

u8      buffer_byte(Buffer *buf, i64 offset);
void    buffer_copy(Buffer *buf, i64 start, i64 end, u8 *dst);
// Points into buffer memory unless the range straddles the gap; then it is copied to `scratch`.
String8 buffer_text(Buffer *buf, Arena *scratch, i64 start, i64 end);
String8 buffer_line(Buffer *buf, Arena *scratch, i64 line);
void    buffer_segments(Buffer *buf, String8 *before, String8 *after); // the text before and after the gap

// Character boundaries: a valid UTF-8 sequence or a single invalid byte is one unit.
i64 buffer_next_char(Buffer *buf, i64 offset);
i64 buffer_prev_char(Buffer *buf, i64 offset);

// Files. Loading needs an empty buffer; on failure it stays empty and the status says why.
// Opening a missing file gives OS_FILE_NOT_FOUND; the caller may then visit the path as a new file.
OsFileStatus buffer_load_file(Buffer *buf, String8 path);
void         buffer_set_path(Buffer *buf, String8 full_path); // also sets the name
// Saving writes a temp file next to the target, flushes it to disk and swaps it in with
// ReplaceFileW, or writes in place when a swap is not possible. Encoding and line-ending mode
// are the buffer's. A read-only target is refused untouched. Save-as visits the new path.
OsFileStatus buffer_save(Buffer *buf);
OsFileStatus buffer_save_as(Buffer *buf, String8 path);
OsFileStatus buffer_save_as_opt(Buffer *buf, String8 path, b32 flush); // flush = 0 only for benchmarks (setting in Phase 5)
const char  *buffer_status_text(OsFileStatus status);           // "access denied", ...

#endif // BUFFER_H
