#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k)&0x1f)
#define MAX_UNDO 200
#define TAB_STOP 8

enum EditorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef enum {
  MODE_NORMAL,
  MODE_INSERT,
  MODE_COMMAND,
  MODE_SEARCH
} EditorMode;

typedef struct {
  int idx;
  int size;
  char *chars;
} erow;

typedef struct {
  char **lines;
  int line_count;
  int cx;
  int cy;
} Snapshot;

typedef struct {
  int size;
  char *b;
} abuf;

typedef struct {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[160];
  time_t statusmsg_time;
  struct termios orig_termios;
  EditorMode mode;
  char cmdbuf[256];
  int cmdlen;
  char yank[4096];
  int pending_g;
  Snapshot undo[MAX_UNDO];
  int undo_len;
  Snapshot redo[MAX_UNDO];
  int redo_len;
} EditorConfig;

EditorConfig E;

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);

void die(const char *s) {
  ssize_t ignored = write(STDOUT_FILENO, "\x1b[2J", 4);
  (void)ignored;
  ignored = write(STDOUT_FILENO, "\x1b[H", 3);
  (void)ignored;
  perror(s);
  exit(1);
}

void disableRawMode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1':
            case '7':
              return HOME_KEY;
            case '3':
              return DEL_KEY;
            case '4':
            case '8':
              return END_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
      }
    }
    return '\x1b';
  }

  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  }
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }
  return rx;
}

void editorFreeSnapshot(Snapshot *s) {
  for (int i = 0; i < s->line_count; i++) free(s->lines[i]);
  free(s->lines);
  s->lines = NULL;
  s->line_count = 0;
}

Snapshot editorMakeSnapshot(void) {
  Snapshot s = {0};
  s.line_count = E.numrows;
  s.cx = E.cx;
  s.cy = E.cy;
  s.lines = calloc((size_t)s.line_count, sizeof(char *));
  if (!s.lines) die("calloc");
  for (int i = 0; i < s.line_count; i++) {
    s.lines[i] = strdup(E.row[i].chars);
    if (!s.lines[i]) die("strdup");
  }
  return s;
}

void editorRestoreSnapshot(Snapshot *s) {
  for (int i = 0; i < E.numrows; i++) free(E.row[i].chars);
  free(E.row);
  E.row = NULL;
  E.numrows = 0;

  if (s->line_count > 0) {
    E.row = calloc((size_t)s->line_count, sizeof(erow));
    if (!E.row) die("calloc");
    for (int i = 0; i < s->line_count; i++) {
      E.row[i].idx = i;
      E.row[i].size = (int)strlen(s->lines[i]);
      E.row[i].chars = strdup(s->lines[i]);
      if (!E.row[i].chars) die("strdup");
    }
  }
  E.numrows = s->line_count;
  E.cx = s->cx;
  E.cy = s->cy;
  if (E.cy > E.numrows) E.cy = E.numrows;
  if (E.cy == E.numrows && E.numrows > 0) E.cy = E.numrows - 1;
  if (E.cy >= 0 && E.cy < E.numrows && E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
}

void editorPushUndoState(void) {
  if (E.undo_len == MAX_UNDO) {
    editorFreeSnapshot(&E.undo[0]);
    memmove(&E.undo[0], &E.undo[1], sizeof(Snapshot) * (MAX_UNDO - 1));
    E.undo_len--;
  }
  E.undo[E.undo_len++] = editorMakeSnapshot();

  for (int i = 0; i < E.redo_len; i++) editorFreeSnapshot(&E.redo[i]);
  E.redo_len = 0;
}

void editorUndo(void) {
  if (E.undo_len == 0) {
    editorSetStatusMessage("Already at oldest change");
    return;
  }
  if (E.redo_len == MAX_UNDO) {
    editorFreeSnapshot(&E.redo[0]);
    memmove(&E.redo[0], &E.redo[1], sizeof(Snapshot) * (MAX_UNDO - 1));
    E.redo_len--;
  }
  E.redo[E.redo_len++] = editorMakeSnapshot();
  Snapshot s = E.undo[--E.undo_len];
  editorRestoreSnapshot(&s);
  editorFreeSnapshot(&s);
  E.dirty = 1;
  editorSetStatusMessage("Undo");
}

void editorRedo(void) {
  if (E.redo_len == 0) {
    editorSetStatusMessage("Already at newest change");
    return;
  }
  if (E.undo_len == MAX_UNDO) {
    editorFreeSnapshot(&E.undo[0]);
    memmove(&E.undo[0], &E.undo[1], sizeof(Snapshot) * (MAX_UNDO - 1));
    E.undo_len--;
  }
  E.undo[E.undo_len++] = editorMakeSnapshot();
  Snapshot s = E.redo[--E.redo_len];
  editorRestoreSnapshot(&s);
  editorFreeSnapshot(&s);
  E.dirty = 1;
  editorSetStatusMessage("Redo");
}

void abAppend(abuf *ab, const char *s, int len) {
  char *new_buf = realloc(ab->b, (size_t)(ab->size + len));
  if (!new_buf) return;
  memcpy(&new_buf[ab->size], s, (size_t)len);
  ab->b = new_buf;
  ab->size += len;
}

void abFree(abuf *ab) { free(ab->b); }

void editorUpdateRowIndexFrom(int start) {
  for (int i = start; i < E.numrows; i++) E.row[i].idx = i;
}

void editorInsertRow(int at, const char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
  if (!E.row) die("realloc");
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (size_t)(E.numrows - at));
  E.row[at].size = (int)len;
  E.row[at].chars = malloc(len + 1);
  if (!E.row[at].chars) die("malloc");
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
  editorUpdateRowIndexFrom(at);
  E.dirty++;
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  free(E.row[at].chars);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (size_t)(E.numrows - at - 1));
  E.numrows--;
  E.row = realloc(E.row, sizeof(erow) * (size_t)(E.numrows > 0 ? E.numrows : 1));
  editorUpdateRowIndexFrom(at);
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, (size_t)(row->size + 2));
  if (!row->chars) die("realloc");
  memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
  row->size++;
  row->chars[at] = (char)c;
  E.dirty++;
}

void editorRowAppendString(erow *row, const char *s, size_t len) {
  row->chars = realloc(row->chars, (size_t)(row->size + len + 1));
  if (!row->chars) die("realloc");
  memcpy(&row->chars[row->size], s, len);
  row->size += (int)len;
  row->chars[row->size] = '\0';
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
  row->size--;
  E.dirty++;
}

void editorInsertChar(int c) {
  if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline(void) {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], (size_t)(row->size - E.cx));
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
  }
  E.cy++;
  E.cx = 0;
}

void editorDeleteChar(void) {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, (size_t)row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

void editorDeleteAtCursor(void) {
  if (E.cy >= E.numrows) return;
  erow *row = &E.row[E.cy];
  if (E.cx < row->size) {
    editorRowDelChar(row, E.cx);
  } else if (E.cy + 1 < E.numrows) {
    editorRowAppendString(row, E.row[E.cy + 1].chars, (size_t)E.row[E.cy + 1].size);
    editorDelRow(E.cy + 1);
  }
}

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numrows; j++) totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc((size_t)totlen);
  if (!buf) die("malloc");
  char *p = buf;
  for (int j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, (size_t)E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(const char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  if (!E.filename) die("strdup");

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    if (errno == ENOENT) {
      editorSetStatusMessage("New file: %s", filename);
      return;
    }
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
    editorInsertRow(E.numrows, line, (size_t)linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

int editorSave(void) {
  if (!E.filename) {
    editorSetStatusMessage("No file name. Use :w <name>");
    return -1;
  }

  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd != -1) {
    if (write(fd, buf, (size_t)len) == len) {
      close(fd);
      free(buf);
      E.dirty = 0;
      editorSetStatusMessage("%d bytes written to disk", len);
      return 0;
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
  return -1;
}

void editorScroll(void) {
  E.rx = 0;
  if (E.cy < E.numrows) E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

  if (E.cy < E.rowoff) E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
  if (E.rx < E.coloff) E.coloff = E.rx;
  if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

const char *editorModeName(void) {
  switch (E.mode) {
    case MODE_INSERT:
      return "INSERT";
    case MODE_COMMAND:
      return "COMMAND";
    case MODE_SEARCH:
      return "SEARCH";
    default:
      return "NORMAL";
  }
}

void editorDrawRows(abuf *ab) {
  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[120];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "vimish -- C Vim-like editor (%s mode)", editorModeName());
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      char lineno[16];
      int ln_len = snprintf(lineno, sizeof(lineno), "%4d ", filerow + 1);
      abAppend(ab, "\x1b[90m", 5);
      abAppend(ab, lineno, ln_len);
      abAppend(ab, "\x1b[39m", 5);

      int len = E.row[filerow].size - E.coloff;
      if (len < 0) len = 0;
      int avail = E.screencols - 5;
      if (len > avail) len = avail;
      if (len > 0) abAppend(ab, &E.row[filerow].chars[E.coloff], len);
    }
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[160], rstatus[64];
  int len = snprintf(status, sizeof(status), "%.24s - %d lines %s | %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "", editorModeName());
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d,%d", E.cy + 1, E.cx + 1);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    }
    abAppend(ab, " ", 1);
    len++;
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = (int)strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 7) abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(void) {
  editorScroll();

  abuf ab = {0, NULL};
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  int cx = (E.rx - E.coloff) + 6;
  int cy = (E.cy - E.rowoff) + 1;
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy, cx);
  abAppend(&ab, buf, (int)strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);
  ssize_t ignored = write(STDOUT_FILENO, ab.b, (size_t)ab.size);
  (void)ignored;
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size && E.cy < E.numrows - 1) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) E.cy--;
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows - 1) E.cy++;
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) E.cx = rowlen;
}

void editorJumpWordForward(void) {
  while (E.cy < E.numrows) {
    erow *row = &E.row[E.cy];
    while (E.cx < row->size && !isalnum((unsigned char)row->chars[E.cx])) E.cx++;
    if (E.cx < row->size) return;
    if (E.cy + 1 >= E.numrows) return;
    E.cy++;
    E.cx = 0;
  }
}

void editorJumpWordBackward(void) {
  if (E.cy >= E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  while (1) {
    if (E.cx == 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    E.cx--;
    if (isalnum((unsigned char)E.row[E.cy].chars[E.cx])) {
      while (E.cx > 0 && isalnum((unsigned char)E.row[E.cy].chars[E.cx - 1])) E.cx--;
      return;
    }
    if (E.cy == 0 && E.cx == 0) return;
  }
}

int editorFindNext(const char *needle) {
  if (!needle || !*needle) return -1;
  int start_row = E.cy;
  int start_col = E.cx + 1;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = start_row; i < E.numrows; i++) {
      erow *row = &E.row[i];
      char *match = strstr(&row->chars[(i == start_row) ? start_col : 0], needle);
      if (match) {
        E.cy = i;
        E.cx = (int)(match - row->chars);
        return 0;
      }
    }
    start_row = 0;
    start_col = 0;
  }
  return -1;
}

void editorYankLine(void) {
  if (E.cy >= E.numrows) return;
  snprintf(E.yank, sizeof(E.yank), "%s", E.row[E.cy].chars);
  editorSetStatusMessage("Yanked 1 line");
}

void editorPasteLineBelow(void) {
  if (E.yank[0] == '\0') {
    editorSetStatusMessage("Yank buffer is empty");
    return;
  }
  editorPushUndoState();
  int at = (E.cy < E.numrows) ? E.cy + 1 : E.numrows;
  editorInsertRow(at, E.yank, strlen(E.yank));
  E.cy = at;
  E.cx = 0;
  editorSetStatusMessage("Pasted line");
}

void editorDeleteLine(void) {
  if (E.cy >= E.numrows) return;
  editorPushUndoState();
  snprintf(E.yank, sizeof(E.yank), "%s", E.row[E.cy].chars);
  editorDelRow(E.cy);
  if (E.cy >= E.numrows && E.cy > 0) E.cy--;
  E.cx = 0;
  editorSetStatusMessage("Deleted line");
}

void editorCommandExecute(void) {
  E.cmdbuf[E.cmdlen] = '\0';
  char *cmd = E.cmdbuf;
  if (cmd[0] == ':') cmd++;

  while (*cmd == ' ') cmd++;
  if (strcmp(cmd, "q") == 0) {
    if (E.dirty) {
      editorSetStatusMessage("Unsaved changes, use :q! to force");
    } else {
      ssize_t ignored = write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
      (void)ignored;
      exit(0);
    }
  } else if (strcmp(cmd, "q!") == 0) {
    ssize_t ignored = write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
    (void)ignored;
    exit(0);
  } else if (strcmp(cmd, "w") == 0) {
    editorSave();
  } else if (strncmp(cmd, "w ", 2) == 0) {
    char *name = cmd + 2;
    while (*name == ' ') name++;
    if (*name == '\0') {
      editorSetStatusMessage("Missing file name");
    } else {
      free(E.filename);
      E.filename = strdup(name);
      if (!E.filename) die("strdup");
      editorSave();
    }
  } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
    if (editorSave() == 0) {
      ssize_t ignored = write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
      (void)ignored;
      exit(0);
    }
  } else if (strncmp(cmd, "e ", 2) == 0) {
    char *name = cmd + 2;
    while (*name == ' ') name++;
    if (*name == '\0') {
      editorSetStatusMessage("Missing file name");
    } else if (E.dirty) {
      editorSetStatusMessage("Unsaved changes, save or use :q! first");
    } else {
      for (int i = 0; i < E.numrows; i++) free(E.row[i].chars);
      free(E.row);
      E.row = NULL;
      E.numrows = 0;
      E.cx = E.cy = 0;
      editorOpen(name);
      editorSetStatusMessage("Opened %s", name);
    }
  } else if (strcmp(cmd, "help") == 0) {
    editorSetStatusMessage("Normal: hjkl,x,dd,yy,p,u,Ctrl-r,i,a,o,/,:(w,wq,q,q!,e)");
  } else {
    editorSetStatusMessage("Unknown command: %s", cmd);
  }
}

void editorProcessInsertMode(int c) {
  switch (c) {
    case '\r':
      editorPushUndoState();
      editorInsertNewline();
      break;
    case 127:
    case CTRL_KEY('h'):
    case DEL_KEY:
      editorPushUndoState();
      editorDeleteChar();
      break;
    case '\x1b':
      E.mode = MODE_NORMAL;
      editorSetStatusMessage("NORMAL mode");
      break;
    default:
      if (!iscntrl(c) && c < 128) {
        editorPushUndoState();
        editorInsertChar(c);
      }
      break;
  }
}

void editorProcessNormalMode(int c) {
  switch (c) {
    case 'h':
    case ARROW_LEFT:
      editorMoveCursor(ARROW_LEFT);
      E.pending_g = 0;
      break;
    case 'j':
    case ARROW_DOWN:
      editorMoveCursor(ARROW_DOWN);
      E.pending_g = 0;
      break;
    case 'k':
    case ARROW_UP:
      editorMoveCursor(ARROW_UP);
      E.pending_g = 0;
      break;
    case 'l':
    case ARROW_RIGHT:
      editorMoveCursor(ARROW_RIGHT);
      E.pending_g = 0;
      break;
    case '0':
    case HOME_KEY:
      E.cx = 0;
      E.pending_g = 0;
      break;
    case '$':
    case END_KEY:
      if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
      E.pending_g = 0;
      break;
    case 'G':
      if (E.numrows > 0) {
        E.cy = E.numrows - 1;
        if (E.cx > E.row[E.cy].size) E.cx = E.row[E.cy].size;
      }
      E.pending_g = 0;
      break;
    case 'g':
      if (E.pending_g) {
        E.cy = 0;
        E.cx = 0;
        E.pending_g = 0;
      } else {
        E.pending_g = 1;
      }
      break;
    case 'w':
      editorJumpWordForward();
      E.pending_g = 0;
      break;
    case 'b':
      editorJumpWordBackward();
      E.pending_g = 0;
      break;
    case 'i':
      E.mode = MODE_INSERT;
      E.pending_g = 0;
      editorSetStatusMessage("INSERT mode");
      break;
    case 'a':
      if (E.cy < E.numrows && E.cx < E.row[E.cy].size) E.cx++;
      E.mode = MODE_INSERT;
      E.pending_g = 0;
      editorSetStatusMessage("INSERT mode");
      break;
    case 'o':
      editorPushUndoState();
      if (E.cy < E.numrows) {
        E.cy++;
        editorInsertRow(E.cy, "", 0);
      } else {
        editorInsertRow(E.numrows, "", 0);
        E.cy = E.numrows - 1;
      }
      E.cx = 0;
      E.mode = MODE_INSERT;
      E.pending_g = 0;
      editorSetStatusMessage("INSERT mode");
      break;
    case 'x':
      editorPushUndoState();
      editorDeleteAtCursor();
      E.pending_g = 0;
      break;
    case 'd':
      if (E.pending_g == -1) {
        editorDeleteLine();
        E.pending_g = 0;
      } else {
        E.pending_g = -1;
      }
      break;
    case 'y':
      if (E.pending_g == -2) {
        editorYankLine();
        E.pending_g = 0;
      } else {
        E.pending_g = -2;
      }
      break;
    case 'p':
      editorPasteLineBelow();
      E.pending_g = 0;
      break;
    case 'u':
      editorUndo();
      E.pending_g = 0;
      break;
    case CTRL_KEY('r'):
      editorRedo();
      E.pending_g = 0;
      break;
    case ':':
      E.mode = MODE_COMMAND;
      E.cmdlen = 1;
      E.cmdbuf[0] = ':';
      E.cmdbuf[1] = '\0';
      E.pending_g = 0;
      editorSetStatusMessage(":");
      break;
    case '/':
      E.mode = MODE_SEARCH;
      E.cmdlen = 1;
      E.cmdbuf[0] = '/';
      E.cmdbuf[1] = '\0';
      E.pending_g = 0;
      editorSetStatusMessage("/");
      break;
    case PAGE_UP:
    case PAGE_DOWN: {
      int times = E.screenrows;
      while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      E.pending_g = 0;
    } break;
    case '\x1b':
      E.pending_g = 0;
      break;
    default:
      E.pending_g = 0;
      break;
  }
}

void editorProcessPromptMode(int c) {
  if (c == '\x1b') {
    E.mode = MODE_NORMAL;
    E.cmdlen = 0;
    E.cmdbuf[0] = '\0';
    editorSetStatusMessage("NORMAL mode");
    return;
  }
  if (c == '\r') {
    if (E.mode == MODE_COMMAND) {
      editorCommandExecute();
    } else if (E.mode == MODE_SEARCH) {
      if (editorFindNext(E.cmdbuf + 1) == 0) {
        editorSetStatusMessage("Found: %s", E.cmdbuf + 1);
      } else {
        editorSetStatusMessage("Pattern not found: %s", E.cmdbuf + 1);
      }
    }
    E.mode = MODE_NORMAL;
    E.cmdlen = 0;
    E.cmdbuf[0] = '\0';
    return;
  }
  if (c == 127 || c == CTRL_KEY('h') || c == DEL_KEY) {
    if (E.cmdlen > 1) E.cmdlen--;
    E.cmdbuf[E.cmdlen] = '\0';
    editorSetStatusMessage("%s", E.cmdbuf);
    return;
  }
  if (!iscntrl(c) && c < 128 && E.cmdlen < (int)sizeof(E.cmdbuf) - 1) {
    E.cmdbuf[E.cmdlen++] = (char)c;
    E.cmdbuf[E.cmdlen] = '\0';
    editorSetStatusMessage("%s", E.cmdbuf);
  }
}

void editorProcessKeypress(void) {
  int c = editorReadKey();
  if (E.mode == MODE_INSERT) {
    editorProcessInsertMode(c);
  } else if (E.mode == MODE_COMMAND || E.mode == MODE_SEARCH) {
    editorProcessPromptMode(c);
  } else {
    editorProcessNormalMode(c);
  }
}

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.mode = MODE_NORMAL;
  E.cmdlen = 0;
  E.yank[0] = '\0';
  E.pending_g = 0;
  E.undo_len = 0;
  E.redo_len = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) editorOpen(argv[1]);
  editorSetStatusMessage(
      "NORMAL mode | i/a/o insert | :w/:q commands | / search | dd/yy/p line ops | u/Ctrl-r undo/redo");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
