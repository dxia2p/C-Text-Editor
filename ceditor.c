/* includes */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* defines */
#define CTRL_KEY(k) ((k) & 0x1f)  // A macro to turn alphabet key codes into their CTRL counterparts

/* data */
struct editorConfig {
  int screenrows;
  int screencols;
  struct termios orig_termios;  // Stores the original terminal settings so we can restore the user's terminal!
};

struct editorConfig E;
/* terminal */

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);  // Clears screen
  write(STDOUT_FILENO, "\x1b[H", 3);  // Puts cursor at top left

  perror(s);
  exit(1);
}

void disableRawMode(){
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {  // Enables "raw" mode, which allows us to read the user's inputs right away instead of after they press enter
  if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON );  // Disables Ctrl-S and Ctrl-Q, and also carriage returns (Ctrl-m)
  raw.c_oflag &= ~(OPOST);  // Turns off translating "\n" to "\r\n" in the terminal's output
  raw.c_cflag |= (CS8);  // Sets character size to 8 bits
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);  // Turns off echo, canonical mode, Ctrl-V and signals
  // Keep in mind that ICANON and ISIG are not input flags for some reason
  raw.c_cc[VMIN] = 0;  // Sets minimum number of bytes of input before read() can return
  raw.c_cc[VTIME] = 1;  // maximum amount of time to wait before read() returns

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey(){  // Waits for a keypress then returns it
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  return c;
}

int getWindowSize(int *rows, int *cols){  // Use pointers in the arguments to "return" multiple values (this also lets us use the return for error codes
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;  // Report error
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/* output */

void editorDrawRows(){
  int y;
  for (y = 0; y < E.screenrows; y++){
    write(STDOUT_FILENO, "~\r\n", 3);  // Write tildes for lines after end of file
  }
}

void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);  // writes an escape sequence that clears the screen
  write(STDOUT_FILENO, "\x1b[H", 3);  // Positions cursor at top left of screen using the H command

  editorDrawRows();

  write(STDOUT_FILENO, "\x1b[H", 3);
}

/* input */

void editorProcessKeypress(){  // Waits for a keypress, then handles it.
  char c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      // We don't use atexit() to clear the screen when program exits so that the error message printed by die() doesn't get erased
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
  }
}

/* init */

void initEditor() { // Initializes all fields in the E struct except termios
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(){
  enableRawMode();
  initEditor();

  char c;
  while (1){
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
