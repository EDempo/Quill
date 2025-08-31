//#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <stdint.h>

//DEFINES//
#define CTRL_KEY(k) ((k & 0x1f))

//DATA//

//Editor configuration
typedef struct EditorConfig {
  struct termios orig_termios;
  int screenrows;
  int screencols;
} Editor_Config;

Editor_Config E;

//TERMINAL//

//Error handling, resets terminal state and kills Quill
void die(const char* s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(0);
}

//Use to restore original terminal settings after closing Quill
void disable_raw_mode() {
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH , &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

//Obtain original terminal settings
void enable_raw_mode() {
  if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) { 
    die("tcsetattr");
  }
  //Wether exiting through exit() or main(), ensures that terminal is restored 
  atexit(disable_raw_mode);
  struct termios raw = E.orig_termios;
  //Setting flags for editor
  raw.c_iflag &= ~( BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);  
  raw.c_cflag &= (CS8);
  raw.c_oflag &= ~(OPOST);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  //Uptading terminal to match new settings
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}


//Reads in keystrokes
char editor_read_key() {
  int nread;
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if((nread == -1 && errno != EAGAIN)) { 
        die("read");
      }
  }
  return c;
}

int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  uint32_t i = 0;
  if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) { return -1; }
  
  while(i < sizeof(buf) - 1) {
    if(read(STDIN_FILENO, &buf[i], 1) != 1 || buf[i] == 'R') { break; }
    i++;
  }

  buf[i] = '\0';
  if(buf[0] != '\x1b' || buf[1] != '[') { return 1; } 
  return 0;
}
int get_window_size(int *rows, int *cols) {
  struct winsize ws;
  if(1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B",12) != 12) { return -1; }
    return get_cursor_position(rows,cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

//OUTPUT//

//Drawing ~ to mark all rows
void editor_draw_rows() {
  int y;
  for(y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

//Clears the screen
void editor_refresh_screen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  editor_draw_rows();
  write(STDOUT_FILENO, "\x1b[H", 3);
}

//INPUT//

//Takes in keystrokes and handles any specific keystroke cases
void editor_process_keypress() {
  char c = editor_read_key();
  switch(c) {
  //Keystroke to close program
  case CTRL_KEY('q'): 
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

//INIT// 

void initEditor() {
  if (get_window_size(&E.screenrows, &E.screencols) == -1) {
    die("get_window_size");
  }
}

int main() {
  enable_raw_mode();
  initEditor();

  while(1) {
    editor_refresh_screen();
    editor_process_keypress();
  }

  return 0;
}
