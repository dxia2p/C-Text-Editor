/* includes */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

/* defines */
#define EDITOR_VERSION "0.0.1"
#define EDITOR_TAB_STOP 8
#define EDITOR_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)  // A macro to turn alphabet key codes into their CTRL counterparts

enum editorkey{
  BACKSPACE = 127,

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
  int rsize;
  char *chars;
  char *render;  // Contains the actual characters to draw on screen
} erow;

struct editorConfig {
  int cx, cy; // Cursor x and y
  int rx;  // Render x
  int rowoff;  // Row offset (Stores how much the user has scrolled)
  int coloff;  // Column offset
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;  // Whether or not the file has been modified since opening/saving
  char *filename;
  char statusmsg[80];  // Stores a status message such as prompting the user for input when searching
  time_t statusmsg_time;  // Timestamp when we set a status message
  struct termios orig_termios;  // Stores the original terminal settings so we can restore the user's terminal!
};

struct editorConfig E;

/* prototypes */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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

int editorRowCxToRx(erow *row, int cx) {  // Converts chars index into render index
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
    rx++;
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for(cx = 0; cx < row->size; cx++) {
    if(row->chars[cx] == '\t')
      cur_rx += (EDITOR_TAB_STOP - 1) - (cur_rx % EDITOR_TAB_STOP);
    cur_rx++;

    if (cur_rx > rx) return cx;
  }
  return cx;
}

void editorUpdateRow(erow *row) {
  // Fills up the render array with characters
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs * (EDITOR_TAB_STOP - 1) + 1);

  int idx = 0; // Number of characters in row->render
  for (j = 0; j < row->size; j++){
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while(idx % EDITOR_TAB_STOP != 0) row->render[idx++] = ' ';  // Add spaces until tab stop (column divisible by 8)
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/* editor operations */

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {  // If cursor is on a new line then insert a row
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);  // Add the character
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {  // If we are at the beginning
    editorInsertRow(E.cy, "", 0);
  } else {  // We are splitting a row into 2
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];  // Move call this again because we called realloc in editorInsertRow, which might invalidate the pointer
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/* file i/o */

char *editorRowsToString(int *buflen) {  // converts array of erow into single string
  // buflen tells the caller how long the string is
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;  // add one for newline
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;  // caller must free the memory
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1){
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;

    editorInsertRow(E.numrows, line, linelen);
  }

  free(line);
  fclose(fp);

  E.dirty = 0;
}

void editorSave() {  // Saves text to file
  if (E.filename == NULL) {  // Prompts the user for a name if this is a new file
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) { // If user aborts
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);  // O_CREAT makes open() create a new file if it doesn't exist, and O_RDWR tells it to open for reading and writing
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* find */

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  if (key == '\r' || key == '\x1b') {  // return immediately instead of doing another search
    last_match = -1;  // Reset static variables when we exit
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {  // If arrow key not pressed reset last_match to -1
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) direction = 1;
  int current = last_match;
  int i;
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;  // Allows wrapping from end of file

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = editorRowRxToCx(row, match - row->render);  // Subtract pointers to get cx
      E.rowoff = E.numrows;  // Sets rowoff to the bottom so that when we refresh the screen, the matching line will be at the top of the screen
      break;
    }
  }
}

void editorFind() {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

  if(query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
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
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff){
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows){
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff){
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols){
    E.coloff = E.rx - E.screencols + 1;
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
      int len = E.row[filerow].rsize - E.coloff;  // Apply column offset
      if (len < 0) len = 0;  // len might drop below 0, so we set it to 0 if that happens
      if (len > E.screencols) len = E.screencols;
      char *c = &E.row[filerow].render[E.coloff];
      int j;
      for (j = 0; j < len; j++){
        if (isdigit(c[j])) {
          abAppend(ab, "\x1b[31m", 5);
          abAppend(ab, &c[j], 1);
          abAppend(ab, "\x1b[39m", 5);
        } else {
          abAppend(ab, &c[j], 1);
        }
      }
    }


    abAppend(ab, "\x1b[K", 3);  // Erases the part of the current line to the right of the cursor

    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);  // Change to inverted colors
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols; // Cut the string short if it doesn't fit
  abAppend(ab, status, len);

  while (len < E.screencols) {
    if(E.screencols - len == rlen) {  // Keep going until we get to the point where adding rstatus would end up against the right side
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // Hides cursor before we clear the screen

  abAppend(&ab, "\x1b[H", 3);  // Positions cursor at top left of screen using the H command

  editorDrawRows(&ab);  // Add stuff to buffer
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1); // Moves the cursor to its position
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // Unhides the cursor

  write(STDOUT_FILENO, ab.b, ab.len);  // write stuff from buffer
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/* input */

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {  // Prompt for input on things like name when saving to a file
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c); // Caller can pass in NULL if they don't want to use callback
      free(buf);
      return NULL;
    } else if (c == '\r') { // If user presses enter
      if (buflen != 0) {  // input cannot be empty
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {  // Not control and input key < 128 (in range of char)
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key){
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key){
    case ARROW_LEFT:
      if (E.cx != 0){
        E.cx--;
      } else if (E.cy >= 0) {  // Moves up to the previous line if at the end 
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) { // Only move right if cursor is to the left of the end of the line
        E.cx++;
      } else if (row && E.cx == row->size){ // Make sur cursor not at end of file before moving down
        E.cy++;
        E.cx = 0;
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

  // Set row again and set E.cx to the end of the line if it is to the right of the end of the line
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;  // NULL is considered 0 here
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress(){  // Waits for a keypress, then handles it.
  static int quit_times = EDITOR_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
    case '\r':
      editorInsertNewline();
      break;

    case CTRL_KEY('q'):
      // We don't use atexit() to clear the screen when program exits so that the error message printed by die() doesn't get erased
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes."
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0; // Home key sends cursor to left
      break;

    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;  // End key sends cursor to right
      break;

    case CTRL_KEY('f'):
      editorFind();  // Search function
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

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

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(c);
      break;
  }

  quit_times = EDITOR_QUIT_TIMES;
}

/* init */

void initEditor() { // Initializes all fields in the E struct except termios
  E.numrows = 0;
  E.rx = 0;
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2; // Last 2 rows are reserved for status bar and status message
}

int main(int argc, char *argv[]){
  enableRawMode();
  initEditor();
  if (argc >= 2){
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  while (1){
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
