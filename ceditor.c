/* includes */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

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

/* main */

int main(){
  enableRawMode();

  char c;
  while (1){
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");  // This is for cygwin because when read() times out it returns -1 with an errno of EAGAIN instead of returning 0
    if (iscntrl(c)){  // Tests whether a character is a control character (nonprintable character)
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q') break;
  }
  return 0;
}
