// #include <ctype.h>
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

// DEFINES//
#define VERSION "1.O"
#define TAB_STOP 8
#define CTRL_KEY(k) ((k & 0x1f))

// DATA//

// Editor row
typedef struct EditorRow {
  int size;     // 4 bytes
  int rsize;    // 4 bytes
  char *chars;  // 8 bytes
  char *render; // 8 bytes
} erow;

// Editor configuration
typedef struct EditorConfig {
  int cx, cy;  // 8 bytes, gives cursor location
  int rx;      // 4 bytes, for knowing the amount of tabs
  int row_off; // 4 bytes, gives the row offset (offset from beginning of given
               // row)
  int col_off; // 4 bytes, gives the col offset (offset from beginning of given
               // column)
  int screen_rows; // 4 bytes, gives the amount
  int screen_cols; // 4 bytes
  int num_rows;    // 4 bytes
  erow *row;       // 8 bytes
  char *file;      // 8 bytes for a file name
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios; // This is a low-level struct which gives us
                               // access to the terminal state
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
    die("tcgetattr");
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
      // Keystroke handling for movement
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

int editor_row_conversion(erow *row, int cx) {
  int rx = 0, j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] != '\t') {
      rx += (TAB_STOP + 1) - (rx % TAB_STOP);
    }
    rx++;
  }
  return rx;
}
void editor_update_row(erow *row) {
  int tabs = 0;
  int j, idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      tabs++;
    }
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % TAB_STOP == 0) {
        row->render[idx++] = ' ';
      }
    }
    row->render[idx++] = row->chars[j];
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}
void editor_append_row(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

  int at = E.num_rows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editor_update_row(&E.row[at]);

  E.num_rows++;
}

// FILE I/O//
void editor_open(char *filename) {
  FILE *fp = fopen(filename, "r");
  free(E.file);
  E.file = strdup(filename);
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
  E.rx = 0;
  if (E.cy < E.num_rows) {
    E.rx = editor_row_conversion(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.row_off) {
    E.row_off = E.cy;
  }
  if (E.cy >= E.row_off + E.screen_rows) {
    E.row_off = E.cy - E.screen_rows + 1;
  }
  if (E.rx < E.col_off) {
    E.col_off = E.rx;
  }
  if (E.rx >= E.col_off + E.screen_cols) {
    E.col_off = E.rx - E.screen_cols + 1;
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
    int filerow = y + E.row_off;
    if (filerow < E.num_rows) {
      int len = E.row[filerow].rsize - E.col_off;
      if (len < 0) {
        len = 0;
      }
      abuf_append(abuf, &E.row[filerow].render[E.col_off], len);
    } else {
      if (E.num_rows == 0 && y == E.screen_rows / 2) {
        editor_draw_welcome(abuf);
      } else {
        abuf_append(abuf, "~", 1);
      }
    }
    abuf_append(abuf, "\x1b[K", 3); // Clearing lines as they are redrawn
    abuf_append(abuf, "\r\n", 2);
  }
}

// Drawing Status Bar
void editor_draw_status_bar(append_buffer *ab) {
  abuf_append(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                     E.file ? E.file : "[No Name]", E.num_rows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.num_rows);
  if(len > E.screen_cols) {
    len = E.screen_cols;
  }

  abuf_append(ab, status, len);

  while (len < E.screen_cols) {
    if(E.screen_cols - len == rlen) {
      abuf_append(ab, rstatus, rlen);
      break;
    }
    abuf_append(ab, " ", 1);
    len++;
  }
  abuf_append(ab, "\x1b[m", 3);
  abuf_append(ab, "\r\n", 2);
}

//Draws the message bar on the screen
void editor_draw_message_bar(append_buffer *ab) {
  abuf_append(ab, "\x1b[K", 3);
  int msg_len = strlen(E.statusmsg);
  if(msg_len > E.screen_cols) {
    msg_len = E.screen_cols;
  }
  if(msg_len && time(NULL) - E.statusmsg_time < 5) {
    abuf_append(ab, E.statusmsg, msg_len);
  }
}
// Clears the screen
void editor_refresh_screen() {
  editor_scroll();

  append_buffer abuf = ABUF_INIT;

  abuf_append(&abuf, "\x1b[?25l", 6); // Hiding cursor
  abuf_append(&abuf, "\x1b[H", 3);    // Position the cursor at top left
                                   //
  editor_draw_rows(&abuf);
  editor_draw_status_bar(&abuf);
  editor_draw_message_bar(&abuf);
  // Keystroke handling for movement

  // Moving cursor to last stored position before screen refresh
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1,
           (E.rx - E.col_off) + 1);
  abuf_append(&abuf, buf, strlen(buf));

  abuf_append(&abuf, "\x1b[?25h", 6); // Reshowing cursor
  write(STDOUT_FILENO, abuf.b, abuf.len);
  abuf_free(&abuf);
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
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

// Takes in keystrokes and handles any specific keystroke cases
void editor_process_keypress(void) {
  char c = editor_read_key();
  switch (c) {
  // Keystroke to close program
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4); // Clear the screen
    write(STDOUT_FILENO, "\x1b[H", 3);  // Position the cursor at the top left
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
  E.rx = 0;
  E.row_off = 0;
  E.col_off = 0;
  E.num_rows = 0;
  E.row = NULL;
  E.file = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
    die("get_window_size");
  }
  E.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  initEditor();
  if (argc >= 2) {
    editor_open(argv[1]);
  }

  editor_set_status_message("HELP: Ctrl-Q to quit");
  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }

  return 0;
}
