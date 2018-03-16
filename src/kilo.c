/*** includes ***/

#include <termios.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <ctype.h>

/* FLAGS:
ECHO: Echo mode echos all characters back to terminal
ICANON: Canonical mode waits to feed characters to program until return key is pressed
ISIG: Disables terminate or suspend control codes
IXON: Disables software flow/transmission control codes
IEXTEN: Fixes Ctrl-O in macOS which would otherwise discard character
ICRNL: Disables carriage return to newline translation (fixes Ctrl-M)
OPOST: Disables output processing
*/

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

// Struct to contain editor state
struct editorConfig {
  int screenrows;
  int screencols;
  // Original terminal attributes
  struct termios orig_termios;
};

struct editorConfig E;
/*** terminal ***/

// Prints error message and exits program
void die(const char *s) {
  // Clear screen
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  // Print error message
  perror(s);
  exit(1);
}

// Disable raw mode at exit
void disableRawMode() {
  // Error handling
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  // Make copy of original before making changes
  struct termios raw = E.orig_termios;
  // Disables terminal flags (see above for details)
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // Adds timeout
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");;
}

// Waits for key press, then return it
char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread ==-1 && errno != EAGAIN) die ("read");
  }
  return c;
}

// Get the current size of the terminal window
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

/*** output ***/

// Draw column of tildas on left hand side of screen
void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

// Refreshes screen by writing escape sequence to terminal after each keypress
void editorRefreshScreen() {
  write(STDIN_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  editorDrawRows();

  // Reposition cursor on screen
  write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/

// Waits for a keypress, then handles it
void editorProcessKeypress() {
  char c = editorReadKey();
  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
  }
}

/*** init ***/

// Initialize fields in E struct
void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die ("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
