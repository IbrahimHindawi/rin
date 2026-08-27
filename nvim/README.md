# Neovim support for rin

Filetype detection, syntax highlighting, and LSP attachment for `.i` files.

## Install

```
python nvim/install.py
```

Options:

| flag | effect |
| --- | --- |
| `--link` | symlink instead of copying, so edits in this repo apply without reinstalling |
| `--dest DIR` | install into a specific config directory |
| `--dry-run` | print what would happen, write nothing |
| `--uninstall` | remove the installed files |

The target defaults to Neovim's own config location: `%LOCALAPPDATA%\nvim` on
Windows, `$XDG_CONFIG_HOME/nvim` or `~/.config/nvim` elsewhere. `NVIM_APPNAME` is
honoured, so it installs alongside a named config rather than over your main one.

## Verify

Open any `.i` file and check:

```vim
:set filetype?     " filetype=rin
:checkhealth lsp   " an rin-lsp client, rooted at the project
```

## What gets installed

| file | purpose |
| --- | --- |
| `ftdetect/rin.lua` | maps `*.rin` to filetype `rin` |
| `syntax/rin.vim` | syntax highlighting |
| `ftplugin/rin.lua` | loads the LSP attachment |
| `after/ftplugin/rin.lua` | starts and attaches `rin-lsp` |

**Why `ftdetect` is Lua rather than Vim script:** Neovim's built-in filetype table
already claims `*.rin` for Progress, so a plain autocmd races against it. This uses
`vim.filetype.add()`, which is consulted first and makes the mapping deterministic.

## LSP entry point

`after/ftplugin/rin.lua` resolves the language server in this order:

1. `vim.g.rin_lsp_command`, if you set it
2. `$RIN_HOME/scripts/rin_lsp.py`, the packaged toolchain layout
3. the repo path baked in by `install.py` at install time

To override it, set the command before the filetype loads:

```lua
vim.g.rin_lsp_command = { "python", "-u", "C:/path/to/i/scripts/rin_lsp.py" }
```

## Highlighting notes

`//` and `/* */` are comments. `#` is C preprocessor passthrough, so `#` lines are
highlighted as directives — and a `#` line that is not a recognized directive is
shown as an **error**, matching the compiler, which rejects it.
