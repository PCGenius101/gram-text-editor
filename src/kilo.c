/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
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

#define KILO_VERSION "1.0"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 2

#define CTRL_KEY(k) ((k) & 0x1f)

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

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)


/*** data ***/

struct editorSyntax {
  char *filetype; // Name of file type to be displayed
  char **filematch; // Array of strings to match filename against
  char **keywords; // Array of keywords
  char *singleline_comment_start; // Pattern for signle line comments
  char *multiline_comment_start; // Pattern for start and end of multi-line comments
  char *multiline_comment_end;
  int flags; // Contains flags for whether to highlight numbers or strings for filetype
};

// Data type for storing a row of text
typedef struct erow {
  int idx; // Index within file of row
  int size;
  int rsize;
  char *chars;
  char *render;
  // Array with highlighting of each line
  unsigned char *hl;
  // Store whether prev line is part of unenclosed ml comment
  int hl_open_comment;
} erow;

// Struct to contain editor state
struct editorConfig {
  // Cursor x and y position
  int cx, cy;
  int rx;
  // Row and column offset to keep track of row/column of the file the user is scrolled to
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  // Dirty variable, dirty if been modified since opening or saving file
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  // Original terminal attributes
  struct termios orig_termios;
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL};
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else"
  "struct", "union", "typedef", "static", "enum", "class", "case",

  // End common C types (secondary keywords) with | character
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

// Highlight database
struct editorSyntax HLDB[] = {
  {
    "c",
    C_HL_extensions,
    C_HL_keywords,
    "//", "/*", "*/",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};
// Length of HLDB array
#define HLDB_ENTIRES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread ==-1 && errno != EAGAIN) die ("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    // Check to see if escape sequence is arrow key or page key, then return corresponding character
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
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
  } else if (seq[0] == '0') {
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

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  // Get cursor position by querying terminal for status info
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

// Get the current size of the terminal window
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  // Fallback if ioctl doesn't work
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
      *cols = ws.ws_col;
      *rows = ws.ws_row;
      return 0;
  }
}

/*** syntax highlighting ***/

// If string doesn't contain character return NULL, otherwise return pointer to character
int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
  // Allocate needed memory
  row->hl = realloc(row->hl, row->rsize);
  // Set all characters to HL_NORMAL by default
  memset(row->hl, HL_NORMAL, row->rsize);
  // If no filetype is set return immediatly
  if (E.syntax == NULL) return;
  // Aliases
  char **keywords = E.syntax->keywords;

  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;
  // Set comment length to length of string, 0 if string is null
  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;
  // Check if previous character was seperator
  int prev_sep = 1;
  // Keep track of whether currently inside string or multi-line comment
  int in_string  = 0;
  // True if previous row has unclosed multi-line comment
  int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

  int i = 0;
  // Loop through characters and set digits to HL_NUMBER
  while (i < row->rsize) {
    char c = row->render[i];
    // Highlight type of previous character
    unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

    if (scs_len && !in_string && !in_comment) {
      // Check if character is start of single line comment
      if (!strncmp(&row->render[i], scs, scs_len)) {
        // Set line to HL_COMMENT
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    if (mcs_len && mce_len && !in_string) {
      if (in_comment) {
        // If currently in multi-line comment, highlight current char
        row->hl[i] = HL_MLCOMMENT;
        // If at end of comment highlight end string and consume
        if (!strncmp(&row->render[i], mce, mce_len)) {
          memset(&row->hl[i], HL_MLCOMMENT, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          // Consume current, already highlighted char
          i++;
          continue;
        }
        // Check if at beginning of multi-line comment
      } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
        memset(&row->hl[i], HL_MLCOMMENT, mcs_len); // Highlight whole mcs string
        i += mcs_len; // Consume mcs string
        in_comment = 1; // Change in_comment to true
        continue;
      }
    }

    // Highlight character as string until closing quote
    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        row->hl[i] = HL_STRING;
        // If current character is backslash, and one more character in line, highlight character after backslash
        if (c == '\\' && i + 1 < row->rsize) {
          row->hl[i + 1] = HL_STRING;
          i += 2;
          continue;
        }
        // Check if current character is closing quote
        if (c == in_string) in_string = 0;
        i++;
        prev_sep = 1; // Closing quote is considered seperator
        continue;
      } else {
        if (c == '"' || c == '\'') { // Check if at beginning of string
          in_string = c; // Store quote in in_string and highlight it
          row->hl[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    // Check if numbers should be highlighted for current file type
    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      // To highlight digit, previous char must be seperator or also highlighted with HL_NUMBER
      if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    // Make sure seperator came before keyword
    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]); // Length of keyword
        // Check if secondary character
        int kw2 = keywords[j][klen - 1] == '|';
        // Decremnt klen to account for | character
        if (kw2) klen--;

        // Check if keyword exists at current position and check if seperator comes after keyword
        if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen])) {
          // Highlight whole keyword, depending on value of kw2
          memset(&row->hl[i], kw2 ? HL_KEYWORD2: HL_KEYWORD1, klen);
          i += klen; // Consume entire keyword
          break; // Break out of inner loop
        }
      }
      // Check if loop was broken out of by checking for termianting NULL value
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }

  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if (changed && row->idx + 1 < E.numrows) {
    editorUpdateSyntax(&E.row[row->idx + 1])
  }
}

int editorSyntaxToColor(int hl) {
  // Return ANSI code for each text
  switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36; // Set comments to cyan
    case HL_KEYWORD1: return 33; // Set keyword 1 to yellow
    case HL_KEYWORD2: return 32; // Set keyword 2 to green
    case HL_STRING: return 35; // Set strings to magenta
    case HL_NUMBER: return 31; // Set numbers to red
    case HL_MATCH: return 34; // Set search matches to blue
    default: return 37; // Set anything else to white
  }
}

void editorSelectSyntaxHighlight() {
  E.syntax = NULL;
  if (E.filename == NULL) return;
  // Find last occurrence of . characrer to get extension part of filename
  char *ext = strrchr(E.filename, '.');

  // Loop through editSyntax in HLDB
  for (unsigned int j = 0; j < HLDB_ENTIRES; j++) {
    struct editorSyntax *s = &HLDB[j];
    unsigned int i = 0;
    // Loop through each pattern in filematch array
    while (s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.'); // If starts with a . then file extension pattern
      // Check if filename ends with extension
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[j]))) {
        // Set E.syntax to current editorSyntax struct
        E.syntax = s;

        // Rehighlight entire file
        int filerow;
        for (filerow = 0; filerow < E.numrows; filerow++) {
          editorUpdateSyntax(&E.row[filerow]);
        }

        return;
      }
      i++;
    }
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') {
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    rx++;
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) { // Loop through chars string
    if (row->chars[cx] == '\t')
      cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP); // Calculate current rx value
    cur_rx++;

    // Stop when current rx hits given rx value
    if (cur_rx > rx) return cx;
  }
  return cx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++){
    if (row->chars[j] == '\t') tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}


void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 ||at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  // Update index of each row that was displaced
  for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  E.row[at].idx = at; // Row's index in file at time of insert

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].hl_open_comment = 0;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++; // Change dirty flag
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  // Update index of each row that was displaced
  for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  // Validate at which is index to insert char into
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc (row->chars, row->size + 2);
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
  // Check if need to append new row before inserting character
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  // If at beginning of line, insert new blank row before
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else { // Otherwise, split line into two rows
    erow *row = &E.row[E.cy];
    // Call editorInsertRow and pass characters to the right of cursor
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    // Reassign row pointer as editorInsertRow() calls realloc which could move mem around
    row = &E.row[E.cy];
    // Truncate current row contents by setting size to position of cursor
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  // Move cursor to beginning of next line
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  // Check if cursor is past end of file or at the beginning of first line
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  // Get row cursor is on, if character to the left delete it
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    // Move cursor after deleting
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  // Add up lenghts of each row of text
  for (j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size + 1; // Add one for the newline character
  }
  // Save total length into buflen to allocate memory
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  // Loop through rows
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n'; // Add newline after each row
    p++;
  }

  return buf;
}

// Open and reading file from disk
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  // Check if new file
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
    editorSelectSyntaxHighlight();
  }

  int len;
  // Change row to string
  char *buf = editorRowsToString(&len);

  // Open a new file if doesn't exist
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

/*** find ***/

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line; // Saves line to have highlighting restored
  static char *saved_hl = NULL; // Saves highlighting to restore

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }

  // If pressed enter or escape, exit search mode
  if (key == '\r' || key == '\x1b') {
    // Index of row the last match was on, -1 of no last match
    last_match = -1;
    // Direction of search, 1 is forward, -1 is backward
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) { // Always search in forward
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) { // Unless user asked to search backwards
    direction = -1;
  } else { // Only advance if arrow key is pressed
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) direction = 1;
  int current = last_match;
  int i;
  // Loop through all the rows of file
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;

    erow *row = &E.row[current];
    // Check if row contains query string
    char *match = strstr(row->render, query);
    // Move cursor to the match
    if (match) {
      // Start next match from current point
      last_match = current;
      E.cy = current;
      E.cx = editorRowRxToCx(row, match - row->render);
      E.cx = match - row->render;
      E.rowoff = E.numrows;

      saved_hl_line = current;
      saved_hl = malloc(row->rsize);

      memcpy(saved_hl, row->hl, row->rsize);
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
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

  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

// Append to buffer to prevent small flickers between writes
void abAppend(struct abuf *ab, const char *s, int len) {
  // Allocate memory to hold new string
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  // Copy string s after end of current data in buffer
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

// Deallocates dynamic memory used by struct abuf
void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

// Draw column of tildas on left hand side of screen
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    // Check if currently draw row part of text buffer
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        // Print welcome message a third of the way down screen
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        // Center welcome message
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        // Add welcome message to buffer
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      // Truncate line if it goes past the end of screen
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      char *c = &E.row[filerow].render[E.coloff];
      // Get pointer with part of hl array that corresponds to current part of render
      unsigned char *hl = &E.row[filerow].hl[E.coloff];
      int current_color = -1; // -1 for default
      int j;
      for (j = 0; j < len; j++) {
        if (iscntrl(c[j])) { // Check if current character is control value
          // Translate into printable character by adding value to '@' (capital letters) or '?' if not in alphabetic range
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          abAppend(ab, "\x1b[7m", 4); // Switch to inverted colors before printing translated symbol
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3); // Turn off inverted colors
          if (current_color != -1) {
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
            abAppend(ab, buf, clen);
          }
        } else if (hl[j] == HL_NORMAL) { // If HL_NORMAL char set to default text color
          if (current_color != -1) {
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) { // When color changes
            // Set curent_color to value editorSyntaxToColor last returned
            current_color = color;
            char buf[16];
            // Write escape sequence to buffer with color
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            // Append character
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
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
  if (msglen && time(NULL) - E.statusmsg_time < 5) {
    abAppend(ab, E.statusmsg, msglen);
  }
}

// Refreshes screen by writing escape sequence to terminal after each keypress
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // Reposition cursor on screen
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  // Write contents of buffer to screen
  write(STDOUT_FILENO, ab.b, ab.len);
  // Deallocate memory
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

// Take callback function, which will be called after each keypress
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  // User input stored in buf
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    // Allow user to press Backspace in input prompt
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') { // If escape key, cancel prompt
      editorSetStatusMessage("");
      if (callback) callback(buf, c); // Allow callback to return NULL
      free(buf);
      return NULL;
    } else if (c == '\r') { // When users presses enter && input is not empty, return input
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
      // Make sure input key isn't one of special keys in editorKey enum
    } else if (!iscntrl(c) && c < 128) { // Test if input key is in range of char (less than 128)
      if (buflen == bufsize - 1) {
        bufsize += 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) { // Move left at the start of a line
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      // Limit scrolling to the right
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
      if (E.cy  < E.numrows) {
        E.cy++;
      }
      break;
  }

  // Fix scrolling between lines
  row = (E.cy >= E.numrows) ? NULL: &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

// Waits for a keypress, then handles it
void editorProcessKeypress() {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editorReadKey();
  switch (c) {
    case '\r':
      editorInsertNewline();
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
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

    // Move cursor to left or right of page
    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    // Move cursor to top or bottom of page
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

  quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

// Initialize fields in E struct
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die ("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
