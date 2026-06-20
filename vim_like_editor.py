from __future__ import annotations

import curses
import os
import sys
from dataclasses import dataclass, field


@dataclass
class EditorCore:
    filename: str | None = None
    lines: list[str] = field(default_factory=lambda: [""])
    row: int = 0
    col: int = 0
    mode: str = "NORMAL"
    command_buffer: str = ""
    status_message: str = "NORMAL"
    dirty: bool = False

    @classmethod
    def from_file(cls, filename: str | None = None) -> "EditorCore":
        editor = cls(filename=filename)
        if filename and os.path.exists(filename):
            with open(filename, "r", encoding="utf-8") as f:
                editor.lines = f.read().splitlines() or [""]
            editor.status_message = f"Opened {filename}"
        return editor

    def clamp_cursor(self) -> None:
        if not self.lines:
            self.lines = [""]
            self.row = 0
            self.col = 0
            return
        self.row = max(0, min(self.row, len(self.lines) - 1))
        line_len = len(self.lines[self.row])
        if self.mode == "NORMAL":
            max_col = line_len - 1 if line_len > 0 else 0
        else:
            max_col = line_len
        self.col = max(0, min(self.col, max_col))

    @staticmethod
    def _printable_char_for_key(key: int) -> str | None:
        if key < 0 or key > 255:
            return None
        try:
            ch = chr(key)
        except ValueError:
            return None
        return ch if ch.isprintable() else None

    def move_left(self) -> None:
        self.col -= 1
        self.clamp_cursor()

    def move_right(self) -> None:
        self.col += 1
        self.clamp_cursor()

    def move_up(self) -> None:
        self.row -= 1
        self.clamp_cursor()

    def move_down(self) -> None:
        self.row += 1
        self.clamp_cursor()

    def enter_insert_mode(self) -> None:
        self.mode = "INSERT"
        self.status_message = "-- INSERT --"

    def enter_normal_mode(self) -> None:
        self.mode = "NORMAL"
        self.status_message = "NORMAL"

    def enter_command_mode(self) -> None:
        self.mode = "COMMAND"
        self.command_buffer = ""
        self.status_message = ":"

    def insert_char(self, ch: str) -> None:
        line = self.lines[self.row]
        self.lines[self.row] = line[: self.col] + ch + line[self.col :]
        self.col += len(ch)
        self.dirty = True

    def backspace(self) -> None:
        if self.col > 0:
            line = self.lines[self.row]
            self.lines[self.row] = line[: self.col - 1] + line[self.col :]
            self.col -= 1
            self.dirty = True
            return
        if self.row > 0:
            prev = self.lines[self.row - 1]
            current = self.lines.pop(self.row)
            self.row -= 1
            self.col = len(prev)
            self.lines[self.row] = prev + current
            self.dirty = True

    def newline(self) -> None:
        line = self.lines[self.row]
        left, right = line[: self.col], line[self.col :]
        self.lines[self.row] = left
        self.lines.insert(self.row + 1, right)
        self.row += 1
        self.col = 0
        self.dirty = True

    def delete_char(self) -> None:
        line = self.lines[self.row]
        if self.col < len(line):
            self.lines[self.row] = line[: self.col] + line[self.col + 1 :]
            self.dirty = True
            return
        if self.row < len(self.lines) - 1:
            self.lines[self.row] += self.lines.pop(self.row + 1)
            self.dirty = True
        self.clamp_cursor()

    def save(self) -> bool:
        if not self.filename:
            self.status_message = "Cannot save: no filename provided"
            return False
        content = "\n".join(self.lines)
        if self.lines != [""]:
            content += "\n"
        with open(self.filename, "w", encoding="utf-8") as f:
            f.write(content)
        self.dirty = False
        self.status_message = f"Wrote {self.filename}"
        return True

    def execute_command(self) -> bool:
        cmd = self.command_buffer.strip()
        self.command_buffer = ""
        self.mode = "NORMAL"
        if cmd == "w":
            self.save()
            return False
        if cmd == "q":
            if self.dirty:
                self.status_message = "Unsaved changes (use :q! to quit)"
                return False
            self.status_message = "Quit"
            return True
        if cmd == "q!":
            self.status_message = "Force quit"
            return True
        if cmd == "wq":
            if self.save():
                self.status_message = "Saved and quit"
                return True
            return False
        self.status_message = f"Unknown command: {cmd}"
        return False

    def process_key(self, key: int) -> bool:
        if self.mode == "NORMAL":
            return self._process_normal(key)
        if self.mode == "INSERT":
            return self._process_insert(key)
        return self._process_command(key)

    def _process_normal(self, key: int) -> bool:
        if key in (ord("h"), curses.KEY_LEFT):
            self.move_left()
        elif key in (ord("j"), curses.KEY_DOWN):
            self.move_down()
        elif key in (ord("k"), curses.KEY_UP):
            self.move_up()
        elif key in (ord("l"), curses.KEY_RIGHT):
            self.move_right()
        elif key == ord("i"):
            self.enter_insert_mode()
        elif key == ord("x"):
            self.delete_char()
        elif key == ord(":"):
            self.enter_command_mode()
        return False

    def _process_insert(self, key: int) -> bool:
        if key == 27:  # ESC
            self.enter_normal_mode()
        elif key in (curses.KEY_BACKSPACE, 127, 8):
            self.backspace()
        elif key in (10, 13):
            self.newline()
        else:
            ch = self._printable_char_for_key(key)
            if ch is not None:
                self.insert_char(ch)
        return False

    def _process_command(self, key: int) -> bool:
        if key == 27:  # ESC
            self.enter_normal_mode()
            return False
        if key in (curses.KEY_BACKSPACE, 127, 8):
            self.command_buffer = self.command_buffer[:-1]
            self.status_message = f":{self.command_buffer}"
            return False
        if key in (10, 13):
            return self.execute_command()
        ch = self._printable_char_for_key(key)
        if ch is not None:
            self.command_buffer += ch
            self.status_message = f":{self.command_buffer}"
        return False


def _draw(stdscr: "curses._CursesWindow", editor: EditorCore) -> None:
    stdscr.erase()
    height, width = stdscr.getmaxyx()
    max_rows = max(1, height - 2)
    for i, line in enumerate(editor.lines[:max_rows]):
        stdscr.addnstr(i, 0, line, width - 1)

    status = f"{editor.filename or '[No Name]'} | {editor.status_message}"
    stdscr.attron(curses.A_REVERSE)
    stdscr.addnstr(height - 1, 0, status.ljust(width), width - 1)
    stdscr.attroff(curses.A_REVERSE)

    editor.clamp_cursor()
    cursor_y = min(editor.row, max_rows - 1)
    cursor_x = min(editor.col, max(0, width - 2))
    stdscr.move(cursor_y, cursor_x)
    stdscr.refresh()


def _run(stdscr: "curses._CursesWindow", filename: str | None) -> None:
    curses.curs_set(1)
    stdscr.keypad(True)
    editor = EditorCore.from_file(filename)
    while True:
        _draw(stdscr, editor)
        key = stdscr.getch()
        should_quit = editor.process_key(key)
        if should_quit:
            return


def main(argv: list[str] | None = None) -> int:
    argv = argv if argv is not None else sys.argv
    filename = argv[1] if len(argv) > 1 else None
    curses.wrapper(lambda scr: _run(scr, filename))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
