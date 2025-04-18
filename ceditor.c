/* includes */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>

/* defines */
#define EDITOR_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)  // A macro to turn alphabet key codes into their CTRL counterparts

enum editorkey{
  ARROW_LEFT = 1000, // These hold large integers so they don't conflict with characters
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/* data */
typedef struct erow {  // erow
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy; // Cursor x and y
  int rowoff;  // Stores how much the user has scrolled
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
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

int editorReadKey(){  // Waits for a keypress then returns it
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b'){
    char seq[3];  // Stores the characters after the escape sequence

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '['){
      if (seq[1] >= '0' && seq[1] <= '9'){  // Checks if the sequence is page up or page down
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; // Checks if there is a third byte
        if (seq[2] == '~'){
          switch (seq[1]){
            case '1': return HOME_KEY;  // There are many different ways of representing home and end
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else{
        switch (seq[1]){  // Check if the sequence is an arrow key
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O'){  // Sometimes the keys are represented as <esc>OH and <esc>OF
      switch (seq[1]){
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {  // Reads cursor position (fallback for ioctl)
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

int getWindowSize(int *rows, int *cols){  // Use pointers in the arguments to "return" multiple values (this also lets us use the return for error codes
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {  // 1 || is temporary, only for testing
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;  // C command moves cursor to the right, B moves cursor down (they will stop at edge of screen so we write 999 to make sure it gets there)
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/* row operations */

void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

/* file i/o */
void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1){
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;

    editorAppendRow(line, linelen);
  }

  free(line);
  fclose(fp);
}

/* append buffer */

struct abuf {  // Append buffer, has a pointer to the buffer and a length
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}  // Empty buffer, acts like a constructor

void abAppend(struct abuf *ab, const char *s, int len){
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab){
  free(ab->b);
}

/* output */

void editorScroll() {
  if (E.cy < E.rowoff){
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows){
    E.rowoff = E.cy - E.screenrows + 1;
  }
}

void editorDrawRows(struct abuf *ab){
  int y;
  for (y = 0; y < E.screenrows; y++){
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3){
        char welcome[80];
        int welcomelen = snprintf(welcome,sizeof(welcome),
          "c editor -- version %s", EDITOR_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        // Adding padding to the welcome message
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);

        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);  // Write tildes for lines after end of file
      }
    } else {
      int len = E.row[filerow].size;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, E.row[filerow].chars, len);
    }


    abAppend(ab, "\x1b[K", 3);  // Erases the part of the current line to the right of the cursor
    if (y < E.screenrows - 1){ // Cannot write \r\n to last line or the terminal will scroll down and the last line will not have a tilde!
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // Hides cursor before we clear the screen

  abAppend(&ab, "\x1b[H", 3);  // Positions cursor at top left of screen using the H command

  editorDrawRows(&ab);  // Add stuff to buffer

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1); // Moves the cursor to its position
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // Unhides the cursor

  write(STDOUT_FILENO, ab.b, ab.len);  // write stuff from buffer
  abFree(&ab);
}

/* input */

void editorMoveCursor(int key){
  switch (key){
    case ARROW_LEFT:
      if (E.cx != 0){
        E.cx--;
      }
      break;
    case ARROW_RIGHT:
      if (E.cx != E.screencols - 1){
        E.cx++;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0){
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows){
        E.cy++;
      }
      break;
  }
}

void editorProcessKeypress(){  // Waits for a keypress, then handles it.
  int c = editorReadKey();

  switch (c) {
    case CTRL_KEY('q'):
      // We don't use atexit() to clear the screen when program exits so that the error message printed by die() doesn't get erased
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      E.cx = 0; // Home key sends cursor to left
      break;

    case END_KEY:
      E.cx = E.screencols - 1;  // End key sends cursor to right
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screenrows;  // Move the cursor up or down as many times as there are screenrows
        while(times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN); // Move up if page up and move down if page down
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

/* init */

void initEditor() { // Initializes all fields in the E struct except termios
  E.numrows = 0;
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.row = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]){
  enableRawMode();
  initEditor();
  if (argc >= 2){
    editorOpen(argv[1]);
  }

  while (1){
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
