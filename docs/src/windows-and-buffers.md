# Windows and buffers

## Windows

The frame holds up to 8 windows, side by side or one above the other, each showing a buffer with its own point and
scroll position; two windows can show the same buffer. A click selects a window; drag the line between two windows, or
a mode line, to resize.

| keys | command | what it does |
|---|---|---|
| `C-x 2` `C-x 3` | `split-window-below` `split-window-right` | split the selected window below / right |
| `C-x o` | `other-window` | select the next window |
| `C-x 0` `C-x 1` | `delete-window` `delete-other-windows` | delete the selected window / every other one |
| `C-x +` | `balance-windows` | make the windows equal |
| `C-x ^` `C-x }` `C-x {` | `enlarge-window` `enlarge-window-horizontally` `shrink-window-horizontally` | change the selected window's size |
| `C-M-v` `C-M-S-v` | `scroll-other-window` `scroll-other-window-down` | scroll the next window forward / back |
| `C-x 4 f` `C-x 4 b` | `find-file-other-window` `switch-to-buffer-other-window` | a file / a buffer in another window |
| `C-x 4 0` | `kill-buffer-and-window` | kill the buffer and delete its window |

A command that shows a buffer in another window (`C-x 4 b`, `C-x 4 f`, `C-h e`) splits a single window side by side
when the frame is at least `split_width_threshold` columns wide, otherwise one above the other. `startup_windows`
set to 2 starts with two windows side by side.

## Buffers and files

Every open file is a buffer; `*scratch*` has no file, and `*Messages*` (`C-h e`) keeps what the echo area said.

| keys | command | what it does |
|---|---|---|
| `C-x C-f` | `find-file` | open a file, with completion |
| `C-x C-s` `C-x s` | `save-buffer` `save-some-buffers` | save the buffer / the modified buffers, asking about each |
| `C-x C-w` | `write-file` | write the buffer to another file and visit that |
| `C-x b` | `switch-to-buffer` | switch the selected window to another buffer |
| `C-x k` | `kill-buffer` | kill a buffer, asking to save it if modified |
| `C-x <left>` `C-x <right>` | `previous-buffer` `next-buffer` | the previous / next buffer of the buffer list |
| `C-x C-c` | `save-buffers-kill-terminal` | quit, asking about unsaved files |

Files are UTF-8 or UTF-16 with a byte order mark; LF, CRLF and mixed line endings are kept byte for byte, and a save
goes through a temporary file.

## Files changed on disk

With `auto_revert` (the default), a buffer without unsaved changes whose file changed on disk is reloaded, when ronin is
activated and at once for the buffers on screen. A buffer with unsaved changes is never touched, only flagged in the
mode line. `M-x revert-buffer` reloads a buffer from its file by hand.

## Crash recovery

When ronin itself crashes, it first writes each modified buffer that visits a file to `%LOCALAPPDATA%\ronin\recovery`,
when the recovery file can be created; the next start says so, and `M-x recover-file` recovers a file from its copy.
Buffers without a file, such as `*scratch*`, are not written. It is a best effort: nothing is written when the process is
ended from outside or the power fails.
