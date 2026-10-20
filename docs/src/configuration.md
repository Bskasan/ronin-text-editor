# Configuration

## Where it is

ronin reads `ronin.conf` next to `ronin.exe` if that file exists (a portable install), otherwise
`%APPDATA%\ronin\ronin.conf`. `C-c ,` opens it, creating it the first time with every setting, color and binding
explained in comments: that text is the built-in configuration, the same one the [settings](settings.html) and
[keys](keys.html) pages are made from.

## Applied as soon as it is saved

ronin applies the file as soon as it is saved, from ronin or any other program; `C-c r` reloads it by hand. A file
needs only what it changes. A bad line is reported in the echo area and in `*Messages*` with its line number, and the
rest still applies.

```ini
[settings]
font      = Cascadia Mono
font_size = 13
theme     = moonfly

[keys]
<f5>        recompile
C-c g       goto-line
```

## The sections

The file has three kinds of sections: `[settings]`, `[colors]` and `[keys]`. Lines starting with `#` are comments
(only at the start of a line).

The settings most often changed:

| setting | default | what it does |
|---|---|---|
| `font` | `Consolas` | a monospace font (Consolas when it is missing) |
| `font_size` | `12` | points, 4 to 96; `C-x C-+` and `C-x C--` change it for the session |
| `theme` | `ronin` | the color theme (see [Themes](themes.html)) |
| `tab_width` | `4` | columns per tab stop |
| `indent_width` | `4` | columns per indentation level |
| `indent_with_tabs` | `false` | indent with tabs instead of spaces (a file's own style is detected when it loads) |
| `render_mode` | `symmetric` | ClearType: `symmetric`, `natural` or `classic` (crisper, narrower) |
| `delete_selection_mode` | `false` | typing replaces an active region |
| `startup_windows` | `1` | 2 starts with two windows side by side |
| `early_window` | `false` | show the window in its background color before the graphics device is ready |
| `ai_provider` | `claude` | the AI tool `C-c a` runs: `claude`, `openai` (codex) or `custom` (`ai_command`) |
| `terminal` | `auto` | the terminal `C-c t` opens: `wt` (Windows Terminal), `cmd` or `auto` |

Every setting, with its default and what it does, is on the [settings page](settings.html). `[colors]` pins single
colors over the theme: see [Themes](themes.html#your-own-theme).

## Rebinding keys

In `[keys]` a line is a key sequence and a command, the command last, so keys such as `=` need no quoting. The command
`none` removes a binding. Later lines win: binding a sequence removes the bindings it conflicts with (binding the prefix
C-x itself would remove every binding that starts with it), with a warning. Plain characters can be bound too; they then stop inserting
themselves. Because `#` starts a comment, a lone `#` key cannot be bound.

`[keys minibuffer]`, `[keys isearch]`, `[keys search-results]`, `[keys compilation]`, `[keys command-list]` and
`[keys git-changes]` hold the keys that come first in those places; every other key falls through to `[keys]`. The
[keys page](keys.html) lists every section with its default bindings.

When several keyboard layouts are installed, Windows' Left Alt+Shift hotkey can switch the layout while you type a Meta
key with Shift (`M-<`, `M-%`); turn that hotkey off in the Windows settings if it gets in the way.
