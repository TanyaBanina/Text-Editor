
//cd /home/tanya/TextEditor --> code .
//produces an executable cc tanya.c -o tanya
//runs the program ./tanya
//the exit status of the last command executed echo $?
// ls = dir
/*tanya: tanya.c
	$(CC) tanya.c -o tanya -Wall -Wextra -pedantic -std=c99 the same as the cc tanya... but now we only run make to compile */
// make;./tanya

/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
//12
#include <errno.h>
//4
#include <ctype.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
//2
#include <stdlib.h>
#include <sys/ioctl.h>
//"Unix Standard" and typically includes functions related to system calls, file I/O, process management, and other low-level operations
#include <unistd.h>
//1
#include <termios.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define TANYA_VERSION "0.0.1"
#define TANYA_TAB_STOP 8
#define TANYA_QUIT_TIMES 3
enum editorKey {
  BACKSPACE = 127,
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


/*** data ***/
typedef struct erow {
  int size;
  int rsize;
  char *chars;
  //for tabs
  char *render;
} erow;

struct editorConfig {
  int cx, cy;
  //for tabs
  int rx;
  //row scroll
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  //dynamically-allocated array of rows?
  erow *row;
  char *filename;
  char statusmsg[80];
  int dirty;
  time_t statusmsg_time;
  struct termios orig_termios;
};
struct editorConfig E;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/*** terminal ***/
//12
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  //perror() looks at the global errno variable and prints a descriptive error message for it
  perror(s);
  exit(1);
}
//2
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}
//1
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  //comes from <stdlib.h>. We use it to register our disableRawMode() function to be called automatically when the program exits
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  //6 IXON //7 ICRNL , &= bitwise AND, ~ bitwise NOT //10
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  //9
  raw.c_oflag &= ~(OPOST);
  //10
   raw.c_cflag |= (CS8);
  //3 Now the program will quit as soon as you press q.//5 ISIG
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  //turns off the ECHO flag in the c_lflag field
  /*11 The VMIN value sets the minimum number of bytes of input needed before read() can return. We set it to 0 so that read() returns as soon as there is any input 
  to be read. 
  The VTIME value sets the maximum amount of time to wait before read() returns. It is in tenths of a second, so we set it to 1/10 of a second*/ 
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

//wait for one keypress, and return it and read it
int editorReadKey() {

  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  //reads esc sequences
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    //page up, down
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}
//terminal window size
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/
//for tabs
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (TANYA_TAB_STOP - 1) - (rx % TANYA_TAB_STOP);
    rx++;
  }
  return rx;
}


//copy row, renders tabs as multiple space characters
void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs*(TANYA_TAB_STOP - 1) + 1);
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % TANYA_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
  //void *realloc(void *ptr, size_t size) attempts to resize the memory block pointed to by ptr that was previously allocated with a call to malloc
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  //void *memcpy(void *dest, const void *src, size_t n) copies n characters from memory area src to memory area dest.
  memcpy(E.row[at].chars, s, len);
  //null at the end
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
  //dest, source, amount of bytes to move. It is like memcpy(), but is safe to use when the source and destination arrays overlap.
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

/*** editor operations ***/
void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
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
/*** file i/o ***/

//converts our array of erow structs into a single string
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    //
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

//open file
void editorOpen(char *filename) {
  free(E.filename);
  //makes a copy of the given string, allocating the required memory and assuming you will free() that memory
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");
  //points to the beginning of the memory block containing the line
  char *line = NULL;
  //unsigned integer
  size_t linecap = 0;
  //signed integer
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
      //clears extra symbols
      while (linelen > 0 && (line[linelen - 1] == '\n' ||
                            line[linelen - 1] == '\r'))
        linelen--;
      editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

//save file
void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }
  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
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

/*** append buffer ***/
struct abuf {
  char *b;
  int len;
};
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}
void abFree(struct abuf *ab) {
  free(ab->b);
}


/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  //above
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  //below
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  //left
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  //right
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}


void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Tanya editor -- version %s", TANYA_VERSION);
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
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    abAppend(ab, "\x1b[K", 3);
    //moves the cursor to the beginning of the current line, moves the cursor to the beginning of the next line
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  //switches to inverted colors
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");

  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
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
  //cursor off
  abAppend(&ab, "\x1b[?25l", 6);
  //\x1b, which is the escape character, or 27
  //<esc>[12;40H postioning the cursor at 1 1
  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  //move cursor to x y
  char buf[32];
  //escape sequence to position the cursor at a specific row and column, +1 because 1 based indexing
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  //cursor on
  abAppend(&ab, "\x1b[?25h", 6);
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}


/*** input ***/

char *editorPrompt(char *prompt) {
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
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
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
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}
//waits for a keypress, and then handles it
void editorProcessKeypress() {
  static int quit_times = TANYA_QUIT_TIMES;
  int c = editorReadKey();
  switch (c) {
    //Enter key
    case '\r':
      editorInsertNewline();
      break;
    case CTRL_KEY('s'):
      editorSave();
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
          editorSetStatusMessage("WARNING!!! File has unsaved changes. "
            "Press Ctrl-Q %d more times to quit.", quit_times);
          quit_times--;
          return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
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
        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
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
  quit_times = TANYA_QUIT_TIMES;
} 

/*** init ***/
//initialize all the fields in the E
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.dirty = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  //doesn’t try to draw a line of text at the bottom of the screen
  E.screenrows -= 2;
}
//filename argument
int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }
  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");
    /*"raw mode" - opposite of canonical mode(In this mode, keyboard input is only sent to your program when the user presses Enter)
    We want to process each keypress as it comes in, so we can respond to it immediately.*/
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
/*1.Turn off echoing , you dont see commands(enableRawMode();)
2.Disable raw mode at exit(restore their terminal’s original attributes when our program exits)
3.Turn off canonical mode
4.Display keypresses
5.Turn off Ctrl-C and Ctrl-Z signals(By default, Ctrl-C sends a SIGINT signal to the current process which causes it to terminate, and Ctrl-Z sends a 
SIGTSTP signal to the current process which causes it to suspend. 
Let’s turn off the sending of both of these signals.)
6.Disable Ctrl-S and Ctrl-Q(By default, Ctrl-S and Ctrl-Q are used for software flow control. Ctrl-S stops data from being transmitted to the terminal until you press Ctrl-Q)
7.Disable Ctrl-V IEXTEN
8.Fix Ctrl-M It turns out that the terminal is helpfully translating any carriage returns (13, '\r') inputted by the user into newlines (10, '\n'). 
Let’s turn off this feature.
9.Turn off all output processing + carriage returns. Terminal translates each newline ("\n") we print into a carriage return followed by a newline ("\r\n").
10.Different flags
11.A timeout for read(): Currently, read() will wait indefinitely for input from the keyboard before it returns. What if we want to do something like animate 
something on the screen while waiting for user input? We can set a timeout, 
so that read() returns if it doesn’t get any input for a certain amount of time.
12.Error handling: First, we’ll add a die() function that prints an error message and exits the program.
Let’s check each of our library calls for failure, and call die() when they fail.
*/