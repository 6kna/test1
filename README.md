# vimish (C Vim-like editor)

This repository now contains a C terminal editor with a Vim-like workflow.

## Build

```sh
make
```

## Run

```sh
./vimish [optional-file]
```

## Implemented features

- Modal editing:
  - **NORMAL** mode (navigation and commands)
  - **INSERT** mode (text entry)
  - **COMMAND** mode (`:` commands)
  - **SEARCH** mode (`/pattern`)
- Navigation:
  - `h j k l`, arrow keys
  - `0`, `$`, `gg`, `G`, `w`, `b`
  - `PageUp`, `PageDown`
- Editing:
  - Insert/append/open line: `i`, `a`, `o`
  - Character delete: `x`
  - Backspace/delete in insert mode
  - Newline insertion
- Line operations:
  - `dd` delete line
  - `yy` yank line
  - `p` paste yanked line below
- Undo/redo:
  - `u` undo
  - `Ctrl-r` redo
- File commands:
  - `:w`, `:w <file>`
  - `:q`, `:q!`
  - `:wq`, `:x`
  - `:e <file>`
- Search:
  - `/pattern` jumps to next match
- Interface:
  - Status line with mode and cursor position
  - Line numbers

## Notes

Implementing *all* features of full Vim is a very large multi-year project.  
This editor includes a substantial Vim-like feature set in C and is much larger than a tiny starter editor.