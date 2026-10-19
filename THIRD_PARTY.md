# Third-party material

ronin contains no third-party code or libraries. Four of its built-in color themes take their color values
(and the names of those colors) from other projects' Neovim color schemes; nothing else was taken from them. Each
theme file in [themes/](themes) records its source, the commit it was ported from, its license and its author in
its `[theme]` block. How each Neovim highlight group maps to a ronin color is written down in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (Decisions, Phase 21).

| theme | source | commit | license | copyright |
|---|---|---|---|---|
| moonfly | [bluz71/vim-moonfly-colors](https://github.com/bluz71/vim-moonfly-colors) | `2c883835f5b7b380a0a9e80695c420f7efebdfe4` | MIT | Copyright (c) 2017-present vim-moonfly-colors authors |
| oxide | [oxidescheme/nvim](https://github.com/oxidescheme/nvim) | `838760c820821cce6a79706430883439c80dad4c` | MIT | Copyright (c) 2025 oxidescheme |
| vague | [vague-theme/vague.nvim](https://github.com/vague-theme/vague.nvim) | `73f8aaa866bf3d14dd0b0ab9e48c11794c1cb7ab` | MIT | Copyright (c) 2024 Alberto Hernandez |
| mellifluous | [ramojus/mellifluous.nvim](https://github.com/ramojus/mellifluous.nvim) | `ec0575bdb63594c5e7fba6eb9607b009aa1fb74e` | MIT | Copyright (c) 2024 Ramojus Lapinskas |

moonfly, oxide and vague write their colors as hex values in their Lua sources, which were read as text.
mellifluous computes its colors from OkHSL values when it loads; they were computed by its own code in Neovim
(v0.11.5) at the commit above, with the command recorded in [themes/mellifluous.conf](themes/mellifluous.conf).

Each of the four projects is under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
> to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions
> of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
> CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.
