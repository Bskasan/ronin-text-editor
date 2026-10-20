# Terminals

| keys | command | what it does |
|---|---|---|
| `C-c t` | `open-terminal` | a terminal in the current file's directory |
| `C-c a` | `open-ai-terminal` | a terminal in the project's root running an AI assistant's command line tool |

`C-c a` runs `claude` (Claude Code) by default, `codex` with `ai_provider = openai`, or any command with
`ai_provider = custom` and `ai_command` (which, when set, is run for any provider). Windows Terminal is used when
`wt.exe` is on PATH, the classic console otherwise; the `terminal` setting (`auto`, `wt` or `cmd`) chooses. ronin
starts the terminal and lets it go: it stays open after ronin ends.
