# Install

## Download

1. Download the latest `ronin-<version>-windows-x64.zip` from
   [Releases](https://github.com/Bskasan/ronin-text-editor/releases).
2. Unzip it anywhere and run `ronin.exe`. Windows 10 or later, x64; there is no installer and no runtime to install.

The zip holds `ronin.exe`, `README.md`, `LICENSE` and `THIRD_PARTY.md`.

## The first start

The exe is not signed, so the first start may show "Windows protected your PC" (SmartScreen): choose **More info**,
then **Run anyway**.

## The command line

```text
ronin file.c                 open a file (as many as you like; the first is shown)
ronin +120:8 file.c          open it at line 120, column 8 (1-based, as in Emacs)
ronin C:\code\project        open a directory as the project
ronin --version              print the version and exit
```

Every file is opened, each at the `+LINE:COLUMN` before it; the first is shown, and with `startup_windows = 2` the
second window shows the second. A file that does not exist is created when you save it. A directory instead of a file
is opened as the project: nothing is visited for it, `*scratch*` and `C-x C-f` start there, and `C-x p f` lists it.

## Portable mode

ronin reads `ronin.conf` next to `ronin.exe` if that file exists (a portable install), otherwise
`%APPDATA%\ronin\ronin.conf`. With a `ronin.conf` beside the exe, the configuration and the theme files of
`themes\` beside it travel with the folder. See [Configuration](configuration.html).

## Samples

`samples\` in the repository has one small file per language to try the highlighting and the indentation on.

## Building it yourself

You need Visual Studio 2022 or later, or the Build Tools for Visual Studio, with the "Desktop development with C++"
workload; `build.bat release` makes `build\ronin.exe`. [Contributing](contributing.html#building) has the details.
