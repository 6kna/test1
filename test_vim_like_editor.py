import os
import tempfile
import unittest

from vim_like_editor import EditorCore


class EditorCoreTests(unittest.TestCase):
    def test_insert_and_escape(self) -> None:
        editor = EditorCore()
        editor.process_key(ord("i"))
        editor.process_key(ord("h"))
        editor.process_key(ord("i"))
        editor.process_key(27)  # ESC
        self.assertEqual(editor.mode, "NORMAL")
        self.assertEqual(editor.lines, ["hi"])

    def test_newline_and_backspace_merge(self) -> None:
        editor = EditorCore(lines=["ab"], row=0, col=2, mode="INSERT")
        editor.process_key(10)  # enter
        editor.process_key(ord("c"))
        self.assertEqual(editor.lines, ["ab", "c"])
        editor.process_key(backspace_key())
        editor.process_key(backspace_key())
        self.assertEqual(editor.lines, ["ab"])

    def test_delete_char_joins_lines_when_current_line_empty(self) -> None:
        editor = EditorCore(lines=["", "cd"], row=0, col=0, mode="NORMAL")
        editor.process_key(ord("x"))
        self.assertEqual(editor.lines, ["cd"])

    def test_command_write_and_quit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "sample.txt")
            editor = EditorCore(filename=path, lines=["hello"])
            editor.enter_command_mode()
            editor.command_buffer = "wq"
            should_quit = editor.execute_command()
            self.assertTrue(should_quit)
            with open(path, "r", encoding="utf-8") as f:
                self.assertEqual(f.read(), "hello\n")

    def test_q_refuses_with_unsaved_changes(self) -> None:
        editor = EditorCore(lines=["x"], dirty=True)
        editor.enter_command_mode()
        editor.command_buffer = "q"
        should_quit = editor.execute_command()
        self.assertFalse(should_quit)
        self.assertIn("Unsaved changes", editor.status_message)


def backspace_key() -> int:
    return 127


if __name__ == "__main__":
    unittest.main()
