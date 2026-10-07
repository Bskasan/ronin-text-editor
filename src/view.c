// view.c — see view.h.

// ---------------------------------------------------------------------------
// Visual columns

b32 view_is_control(u8 b) {
    return (b < 0x20 && b != '\t' && b != '\n') || b == 0x7F;
}

i64 view_char_width(u8 first_byte, i64 col) {
    if (first_byte == '\t') return VIEW_TAB_WIDTH - col % VIEW_TAB_WIDTH;
    return view_is_control(first_byte) ? 2 : 1;
}

i64 view_walk(Buffer *buf, i64 pos, i64 end, i64 *io_col, i64 stop_col) {
    i64 col = *io_col;
    i64 gap = buf->gap_end - buf->gap_start;
    while (pos < end) {
        // A contiguous run on one side of the gap; p[pos] is the byte at pos.
        u8 *p = pos < buf->gap_start ? buf->text : buf->text + gap;
        i64 run_end = pos < buf->gap_start ? MIN(end, buf->gap_start) : end;
        while (pos < run_end) {
            u8 b = p[pos];
            if (b >= 0x20 && b < 0x7F) { // printable ASCII: the common case
                if (col + 1 > stop_col) goto done;
                col++;
                pos++;
                continue;
            }
            if (b >= 0x80) {
                if (col + 1 > stop_col) goto done;
                i64 advance;
                i64 avail = run_end - pos;
                if (avail >= 4 || run_end == end) utf8_decode(p + pos, avail, &advance); // cannot straddle anything
                else advance = buffer_next_char(buf, pos) - pos;                         // may straddle the gap
                col++;
                pos += advance;
                continue;
            }
            i64 w = view_char_width(b, col);
            if (col + w > stop_col) goto done;
            col += w;
            pos++;
        }
    }
done:
    *io_col = col;
    return pos;
}

i64 view_column_of(Buffer *buf, i64 offset) {
    i64 col = 0;
    view_walk(buf, buffer_line_start(buf, buffer_line_of(buf, offset)), offset, &col, I64_MAX);
    return col;
}

i64 view_offset_at_column(Buffer *buf, i64 line, i64 col) {
    i64 c = 0;
    i64 end = buffer_line_end(buf, line);
    i64 pos = view_walk(buf, buffer_line_start(buf, line), end, &c, col);
    if (pos == end) return end;
    // The character at pos covers [c, c + w) and col is inside it.
    i64 w = view_char_width(buffer_byte(buf, pos), c);
    return 2 * (col - c) <= w ? pos : buffer_next_char(buf, pos);
}
