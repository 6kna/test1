# Vim-like terminal editor (bim-style)

This repository now contains a small terminal text editor inspired by vim/neovim and
`klange/bim`.

## Features

- Normal mode and insert mode
- Vim-style movement in normal mode: `h`, `j`, `k`, `l`
- `i` to enter insert mode, `Esc` to return to normal mode
- `x` to delete character under cursor in normal mode
- Command mode with `:` supporting:
  - `:w` (save)
  - `:q` (quit if no unsaved changes)
  - `:q!` (force quit)
  - `:wq` (save and quit)

## Run

```bash
python vim_like_editor.py [optional-file]
```

## Tests

```bash
python -m unittest test_vim_like_editor.py
```