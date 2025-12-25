// #include <ctype.h>
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

// DEFINES//
#define VERSION "1.O"
#define CTRL_KEY(k) ((k & 0x1f))

// DATA//

// Editor row
typedef struct EditorRow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

// Editor configuration
typedef struct EditorConfig {
  int cx, cy;
  int rowoff;
  int row_off;
  int col_off;
  struct termios orig_termios;
  int screen_rows;
  int screen_cols;
  int num_rows;
  erow *row;
} econfig;

econfig E;

// TERMINAL//

// Error handling, resets terminal state and kills Quill
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(0);
}

// Use to restore original terminal settings after closing Quill
void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

// Obtain original terminal settings
void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
  // Wether exiting through exit() or main(), ensures that terminal is restored
  atexit(disable_raw_mode);
  struct termios raw = E.orig_termios;
  // Setting flags for editor
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cflag &= (CS8);
  raw.c_oflag &= ~(OPOST);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  // Uptading terminal to match new settings
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

// Reads in keystrokes
char editor_read_key(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if ((nread == -1 && errno != EAGAIN)) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
        read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }

    if (seq[0] == '[') {
      switch (seq[1]) {
      case 'A':
        return 'k';
      case 'B':
        return 'j';
      case 'C':
        return 'l';
      case 'D':
        return 'h';
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  uint32_t i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1 || buf[i] == 'R') {
      break;
    }
    i++;
  }

  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') {
    return 1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return 1;
  }

  return 0;
}

int get_window_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return get_cursor_position(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

// ROW OPERATIONS//
void editor_append_row(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

  int at = E.num_rows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.num_rows++;
}

// FILE I/O//
void editor_open(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;
  while ((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 &&
           (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
      line_len--;
    }
    editor_append_row(line, line_len);
  }
  free(line);
  fclose(fp);
}

// APPEND BUFFER//
typedef struct AppendBuffer {
  char *b;
  int len;
} append_buffer;

#define ABUF_INIT                                                              \
  { NULL, 0 }

void abuf_append(append_buffer *abuf, const char *s, int len) {
  char *new = realloc(abuf->b, abuf->len + len);
  if (new == NULL) {
    return;
  }
  memcpy(&new[abuf->len], s, len);
  abuf->b = new;
  abuf->len += len;
}

void abuf_free(append_buffer *abuf) { free(abuf->b); }

// OUTPUT//

// Scrolling
void editor_scroll() {
  if (E.cy < E.row_off) {
    E.row_off = E.cy;
  }
  if (E.cy >= E.row_off + E.screen_rows) {
    E.row_off = E.cy - E.screen_rows + 1;
  }
  if (E.cx < E.col_off) {
    E.col_off = E.cx;
  }
  if (E.cx >= E.col_off + E.screen_cols) {
    E.col_off = E.cx - E.screen_cols + 1;
  }
}

// Drawing welcome
void editor_draw_welcome(append_buffer *abuf) {
  char welcome[80];
  int welcome_len =
      snprintf(welcome, sizeof(welcome), "Quill Editor %s", VERSION);
  if (welcome_len > E.screen_cols) {
    welcome_len = E.screen_cols;
  }
  abuf_append(abuf, "~", 1);
  int padding =
      (E.screen_cols - welcome_len) / 2 - 1; // Finding center of the screen
  while (padding) {
    abuf_append(abuf, " ", 1);
    padding--;
  }
  abuf_append(abuf, welcome, welcome_len);
}

// Drawing ~ to mark all rows
void editor_draw_rows(append_buffer *abuf) {
  int y;
  for (y = 0; y < E.screen_rows - 1; y++) {
    if (y < E.num_rows) {
      int len = E.row[y].size;
      if (len > E.screen_cols) {
        len = E.screen_cols;
      }
      abuf_append(abuf, E.row[y].chars, len);
      int filerow = y + E.row_off;
      if (filerow < E.num_rows) {
        int len = E.row[filerow].size - E.col_off;
        if (len < 0) {
          len = 0;
        }
        abuf_append(abuf, &E.row[filerow].chars[E.col_off], len);
      } else {
        if (E.num_rows == 0 && y == E.screen_rows / 2) {
          editor_draw_welcome(abuf);
        } else {
          abuf_append(abuf, "~", 1);
        }
      }
      abuf_append(abuf, "\x1b[K", 3); // Clearing lines as they are redrawn
      if (y < E.screen_rows - 1) {
        abuf_append(abuf, "\r\n", 2);
      }
    }
  }
}

// Clears the screen
void editor_refresh_screen() {
  editor_scroll();

  append_buffer abuf = ABUF_INIT;

  abuf_append(&abuf, "\x1b[?25l", 6); // Hiding cursor
  abuf_append(&abuf, "\x1b[H", 3);    // Position the cursor at top left
  editor_draw_rows(&abuf);

  // Moving cursor to last stored position before screen refresh
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1,
           (E.cx - E.col_off) + 1);
  abuf_append(&abuf, buf, strlen(buf));

  abuf_append(&abuf, "\x1b[?25h", 6); // Reshowing cursor
  write(STDOUT_FILENO, abuf.b, abuf.len);
  abuf_free(&abuf);
}

// INPUT//

// Movinng the cursor
void editor_move_cursor(char key) {
  erow *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
  switch (key) {
  case 'h':
    if (E.cx != 0) {
      E.cx--;
    } else if (E.cy > 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case 'l':
    if (E.cx != E.screen_cols - 1) {
      E.cx++;
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (E.cy < E.num_rows) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case 'k':
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case 'j':
      if (E.cy != E.screen_rows - 1) {
        if (E.cy < E.num_rows) {
          E.cy++;
        }
        break;
      }

      row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
      int len = row ? row->size : 0;
      if (E.cx > len) {
        E.cx = len;
      }
    }
  }
}
// Takes in keystrokes and handles any specific keystroke cases
void editor_process_keypress(void) {
  char c = editor_read_key();
  switch (c) {
  // Keystroke to close program
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4); // Clear the screen
    write(STDOUT_FILENO, "\x1b[H", 3);  // Position the cursor at the top
                                        // left
    exit(0);
    break;
  case 'h':
  case 'j':
  case 'k':
  case 'l':
    editor_move_cursor(c);
    break;
  }
}

// INIT//

void initEditor(void) {
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.row_off = 0;
  E.col_off = 0;
  E.num_rows = 0;
  E.row = NULL;
  if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
    die("get_window_size");
  }
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  initEditor();
  if (argc >= 2) {
    editor_open(argv[1]);
  }
  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }

  return 0;
}
