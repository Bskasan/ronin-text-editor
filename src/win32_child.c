// win32_child.c — dev builds only: --child <script>, the scripted program the build tests run. It reads one
// command per line and writes to the stdout and stderr it was given (the build's pipe), with no window and
// no log (the run that started it writes build\ronin.log):
//
//   out TEXT, err TEXT        TEXT and CRLF to stdout / stderr
//   raw TEXT, rawerr TEXT     exactly TEXT (escapes \r \n \t \e \\ \xHH)
//   sleep MS                  waits
//   exit N                    ends with exit code N (the end of the script: 0)
//   hang                      waits; gives up after 120 s (exit 99), so a broken test leaves nothing for long
//   spawn FILE                a grandchild on another script, inheriting stdout and stderr; prints "grandchild <pid>"
//   flood BYTES               numbered lines in 64 KB writes; every 64th an MSVC warning on flood.c
//   cwd                       the working directory and CRLF
//   write PATH TEXT           PATH rewritten with TEXT (escapes), as a build that regenerates a file
//
// An unknown line ends it with exit code 98.

#if RONIN_DEV

static void win32_child_write(DWORD which, const u8 *data, i64 n) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    while (n > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, data, (DWORD)MIN(n, (i64)MB(1)), &wrote, NULL) || !wrote) return;
        data += wrote;
        n -= wrote;
    }
}

static u32 win32_child_hex(u8 c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10u : c >= 'A' && c <= 'F' ? c - 'A' + 10u : 16u;
}

// The escapes of TEXT into `out` (as long as TEXT); its length.
static i64 win32_child_unescape(String8 in, u8 *out) {
    i64 n = 0;
    for (i64 i = 0; i < in.len; i++) {
        u8 c = in.data[i];
        if (c != '\\' || i + 1 >= in.len) {
            out[n++] = c;
            continue;
        }
        u8 e = in.data[++i];
        if (e == 'r') out[n++] = '\r';
        else if (e == 'n') out[n++] = '\n';
        else if (e == 't') out[n++] = '\t';
        else if (e == 'e') out[n++] = 0x1B;
        else if (e == 'x' && i + 2 < in.len && win32_child_hex(in.data[i + 1]) < 16 && win32_child_hex(in.data[i + 2]) < 16) {
            out[n++] = (u8)(win32_child_hex(in.data[i + 1]) * 16 + win32_child_hex(in.data[i + 2]));
            i += 2;
        } else {
            out[n++] = e;
        }
    }
    return n;
}

static i64 win32_child_number(String8 s) {
    i64 v = 0, i = 0, sign = 1;
    if (s.len && s.data[0] == '-') {
        sign = -1;
        i = 1;
    }
    for (; i < s.len && s.data[i] >= '0' && s.data[i] <= '9'; i++) v = v * 10 + (s.data[i] - '0');
    return v * sign;
}

static void win32_child_flood(Arena *arena, i64 bytes) {
    u8 *block = PUSH_ARRAY(arena, u8, KB(64) + 256);
    i64 used = 0, line = 1;
    while (bytes > 0) {
        i64 n = line % 64 == 0 ? fmt_buf(block + used, 256, "flood.c(%D,1): warning C4100: 'x': unreferenced formal parameter\r\n", line)
                               : fmt_buf(block + used, 256, "flood line %D: the quick brown fox jumps over the lazy dog 0123456789\r\n", line);
        used += n;
        line++;
        if (used >= KB(64) || used >= bytes) {
            i64 take = MIN(used, bytes);
            win32_child_write(STD_OUTPUT_HANDLE, block, take);
            bytes -= take;
            used = 0;
        }
    }
}

static i32 win32_dev_child(Platform *p, String8 script) {
    Arena *arena = &p->perm;
    String8 text;
    if (os_read_small_file(arena, script, MB(16), &text) != OS_FILE_OK) {
        static const u8 msg[] = "child: cannot read the script\r\n";
        win32_child_write(STD_ERROR_HANDLE, msg, sizeof(msg) - 1);
        return 97;
    }
    u8 *buf = PUSH_ARRAY(arena, u8, text.len + 4);
    for (i64 at = 0; at < text.len;) {
        i64 end = at;
        while (end < text.len && text.data[end] != '\n') end++;
        String8 line = str8(text.data + at, end - at);
        at = end + 1;
        if (line.len && line.data[line.len - 1] == '\r') line.len--;
        if (!line.len) continue;
        i64 sp = 0;
        while (sp < line.len && line.data[sp] != ' ') sp++;
        String8 cmd = str8(line.data, sp), arg = sp < line.len ? str8(line.data + sp + 1, line.len - sp - 1) : str8(NULL, 0);
        if (str8_equal(cmd, STR8_LIT("out")) || str8_equal(cmd, STR8_LIT("err"))) {
            i64 n = win32_child_unescape(arg, buf);
            buf[n++] = '\r';
            buf[n++] = '\n';
            win32_child_write(cmd.data[0] == 'o' ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, buf, n);
        } else if (str8_equal(cmd, STR8_LIT("raw")) || str8_equal(cmd, STR8_LIT("rawerr"))) {
            win32_child_write(cmd.len == 3 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, buf, win32_child_unescape(arg, buf));
        } else if (str8_equal(cmd, STR8_LIT("sleep"))) {
            Sleep((DWORD)win32_child_number(arg));
        } else if (str8_equal(cmd, STR8_LIT("exit"))) {
            return (i32)win32_child_number(arg);
        } else if (str8_equal(cmd, STR8_LIT("hang"))) {
            Sleep(120000);
            return 99;
        } else if (str8_equal(cmd, STR8_LIT("spawn"))) {
            String8 exe = os_dev_exe_path(arena);
            String16 line16 = str16_from_str8(arena, str8_fmt(arena, "\"%S\" --child \"%S\"", exe, arg));
            STARTUPINFOW si = { sizeof(si) };
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            PROCESS_INFORMATION pi;
            if (!CreateProcessW(NULL, (WCHAR *)line16.data, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 96;
            u8 msg[64];
            i64 n = fmt_buf(msg, sizeof(msg), "grandchild %u\r\n", (u32)pi.dwProcessId);
            win32_child_write(STD_OUTPUT_HANDLE, msg, n);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else if (str8_equal(cmd, STR8_LIT("flood"))) {
            win32_child_flood(arena, win32_child_number(arg));
        } else if (str8_equal(cmd, STR8_LIT("cwd"))) {
            WCHAR dir[MAX_PATH * 2];
            DWORD n = GetCurrentDirectoryW(ARRAY_COUNT(dir), dir);
            String8 s = str8_from_str16(arena, (u16 *)dir, n < ARRAY_COUNT(dir) ? n : 0);
            win32_child_write(STD_OUTPUT_HANDLE, s.data, s.len);
            win32_child_write(STD_OUTPUT_HANDLE, (const u8 *)"\r\n", 2);
        } else if (str8_equal(cmd, STR8_LIT("write"))) {
            i64 s2 = 0;
            while (s2 < arg.len && arg.data[s2] != ' ') s2++;
            String8 path = str8(arg.data, s2), body = s2 < arg.len ? str8(arg.data + s2 + 1, arg.len - s2 - 1) : str8(NULL, 0);
            if (!os_write_file(path, str8(buf, win32_child_unescape(body, buf)))) return 95;
        } else {
            static const u8 msg[] = "child: unknown line\r\n";
            win32_child_write(STD_ERROR_HANDLE, msg, sizeof(msg) - 1);
            return 98;
        }
    }
    return 0;
}

#endif
