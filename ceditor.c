/* includes */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/* defines */
#define CTRL_KEY(k) ((k) & 0x1f)  // A macro to turn alphabet key codes into their CTRL counterparts

/* data */

struct termios orig_termios;  // Stores the original terminal settings so we can restore the user's terminal!

/* terminal */

void die(const char *s) {
  perror(s);
  exit(1);
}

void disableRawMode(){
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {  // Enables "raw" mode, which allows us to read the user's inputs right away instead of after they press enter
  if(tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;
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

/* output */

void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
}

/* input */

void editorProcessKeypress(){  // Waits for a keypress, then handles it.
  char c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      exit(0);
      break;
  }
}

/* init */

int main(){
  enableRawMode();

  char c;
  while (1){
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
