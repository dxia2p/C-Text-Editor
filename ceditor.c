#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;  // Stores the original terminal settings so we can restore the user's terminal!

void disableRawMode(){
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {  // Enables "raw" mode, which allows us to read the user's inputs right away instead of after they press enter
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disableRawMode);

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);  // Turns off both echo and canonical mode
  // Keep in mind that ICANON is not an input flag for some reason

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(){
  enableRawMode();

  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q'){
    if (iscntrl(c)){  // Tests whether a character is a control character (nonprintable character)
      printf("%d\n", c);
    } else {
      printf("%d ('%c')\n", c, c);
    }
  }
  return 0;
}
