/*
    cmatrix.c -- BruceOS port

    Copyright (C) 1999-2017 Chris Allegretta
    Copyright (C) 2017-Present Abishek V Ashok

    This file is part of cmatrix, adapted for BruceOS from the upstream
    source at https://github.com/abishekvashok/cmatrix (commit current as
    of this port). The rendering algorithm and main loop below are
    Abishek V Ashok's and Chris Allegretta's work, carried over
    essentially unchanged; what's been removed or rewritten is
    exclusively the OS-integration layer upstream needed and BruceOS
    doesn't: command-line parsing (getopt), real signal handling
    (SIGINT/SIGQUIT/SIGWINCH/SIGTSTP), the Linux "screensaver"/TIOCSTI
    keystroke-injection mode, console-font switching (consolechars/
    setfont), alternate-tty redirection (-t), and the wide-character
    Unicode kana rendering path -- BruceOS's ncurses (components/ncurses)
    is built without wide-char support, by design (see that component's
    README.md), so this port always uses the plain-ASCII character set
    upstream's own default (non -c/-l/-x) mode already uses.

    cmatrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    cmatrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with cmatrix. If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curses.h>

/* Matrix typedef */
typedef struct cmatrix {
    int val;
    bool is_head;
} cmatrix;

/* Global variables */
cmatrix **matrix = (cmatrix **) NULL;
int *length = NULL;  /* Length of cols in each line */
int *spaces = NULL;  /* Spaces left to fill */
int *updates = NULL; /* Update speed per column */

/* What we do when we're all set to exit */
void finish(void) {
    curs_set(1);
    clear();
    refresh();
    resetty();
    endwin();
    exit(0);
}

/* nmalloc from nano by Big Gaute */
void *nmalloc(size_t howmuch) {
    void *r = malloc(howmuch);
    if (!r) {
        /* No c_die()/stderr drama here -- there's no terminal left to
         * print to usefully once curses is this far in, and this
         * should never happen on a device with no swap/overcommit
         * surprises to begin with. */
        finish();
    }
    return r;
}

/* Initialize the global variables */
void var_init(void) {
    int i, j;

    if (matrix != NULL) {
        free(matrix[0]);
        free(matrix);
    }

    matrix = nmalloc(sizeof(cmatrix *) * (LINES + 1));
    matrix[0] = nmalloc(sizeof(cmatrix) * (LINES + 1) * COLS);
    for (i = 1; i <= LINES; i++) {
        matrix[i] = matrix[i - 1] + COLS;
    }

    if (length != NULL) free(length);
    length = nmalloc(COLS * sizeof(int));

    if (spaces != NULL) free(spaces);
    spaces = nmalloc(COLS * sizeof(int));

    if (updates != NULL) free(updates);
    updates = nmalloc(COLS * sizeof(int));

    /* Make the matrix */
    for (i = 0; i <= LINES; i++) {
        for (j = 0; j <= COLS - 1; j += 2) {
            matrix[i][j].val = -1;
        }
    }

    for (j = 0; j <= COLS - 1; j += 2) {
        spaces[j] = (int) rand() % LINES + 1;
        length[j] = (int) rand() % (LINES - 3) + 3;
        matrix[1][j].val = ' ';
        updates[j] = (int) rand() % 3 + 1;
    }
}

int app_main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    int i, y, z, keypress;
    int j = 0;
    int count = 0;
    int asynch = 0;
    int bold = 0;
    int pause = 0;
    int update = 4;
    int highnum = 0;
    int mcolor = COLOR_GREEN;
    int rainbow = 0;
    int lambda = 0;
    int changes = 0;
    int randnum = 0;
    int randmin = 0;

    srand((unsigned) time(NULL));

    initscr();
    savetty();
    nonl();
    cbreak();
    noecho();
    timeout(0);
    leaveok(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        if (use_default_colors() != ERR) {
            init_pair(COLOR_BLACK, -1, -1);
            init_pair(COLOR_GREEN, COLOR_GREEN, -1);
            init_pair(COLOR_WHITE, COLOR_WHITE, -1);
            init_pair(COLOR_RED, COLOR_RED, -1);
            init_pair(COLOR_CYAN, COLOR_CYAN, -1);
            init_pair(COLOR_MAGENTA, COLOR_MAGENTA, -1);
            init_pair(COLOR_BLUE, COLOR_BLUE, -1);
            init_pair(COLOR_YELLOW, COLOR_YELLOW, -1);
        } else {
            init_pair(COLOR_BLACK, COLOR_BLACK, COLOR_BLACK);
            init_pair(COLOR_GREEN, COLOR_GREEN, COLOR_BLACK);
            init_pair(COLOR_WHITE, COLOR_WHITE, COLOR_BLACK);
            init_pair(COLOR_RED, COLOR_RED, COLOR_BLACK);
            init_pair(COLOR_CYAN, COLOR_CYAN, COLOR_BLACK);
            init_pair(COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
            init_pair(COLOR_BLUE, COLOR_BLUE, COLOR_BLACK);
            init_pair(COLOR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
        }
    }

    /* Plain-ASCII printable range -- upstream's own default character
     * set (what it uses without -c/-l/-x), the only one this port
     * supports (see this file's top comment). */
    randmin = 33;
    highnum = 123;
    randnum = highnum - randmin;

    var_init();

    while (1) {
        count++;
        if (count > 4) {
            count = 1;
        }

        if ((keypress = wgetch(stdscr)) != ERR) {
            switch (keypress) {
            case 'q':
                finish();
                break;
            case 'a':
                asynch = 1 - asynch;
                break;
            case 'b':
                bold = 1;
                break;
            case 'B':
                bold = 2;
                break;
            case 'n':
                bold = 0;
                break;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                update = keypress - 48;
                break;
            case '!':
                mcolor = COLOR_RED;
                rainbow = 0;
                break;
            case '@':
                mcolor = COLOR_GREEN;
                rainbow = 0;
                break;
            case '#':
                mcolor = COLOR_YELLOW;
                rainbow = 0;
                break;
            case '$':
                mcolor = COLOR_BLUE;
                rainbow = 0;
                break;
            case '%':
                mcolor = COLOR_MAGENTA;
                rainbow = 0;
                break;
            case 'r':
                rainbow = 1;
                break;
            case 'm':
                lambda = !lambda;
                break;
            case '^':
                mcolor = COLOR_CYAN;
                rainbow = 0;
                break;
            case '&':
                mcolor = COLOR_WHITE;
                rainbow = 0;
                break;
            case 'k':
                changes = !changes;
                break;
            case 'p':
            case 'P':
                pause = (pause == 0) ? 1 : 0;
                break;
            }
        }

        for (j = 0; j <= COLS - 1; j += 2) {
            if ((count > updates[j] || asynch == 0) && pause == 0) {
                /* New-style scrolling (upstream's default, and the only
                 * mode this port keeps -- see this file's top comment:
                 * old-style scrolling is dropped along with the other
                 * command-line-only options). */
                if (matrix[0][j].val == -1 && matrix[1][j].val == ' ' && spaces[j] > 0) {
                    spaces[j]--;
                } else if (matrix[0][j].val == -1 && matrix[1][j].val == ' ') {
                    length[j] = (int) rand() % (LINES - 3) + 3;
                    matrix[0][j].val = (int) rand() % randnum + randmin;
                    spaces[j] = (int) rand() % LINES + 1;
                }
                i = 0;
                y = 0;
                int firstcoldone = 0;
                while (i <= LINES) {
                    while (i <= LINES && (matrix[i][j].val == ' ' || matrix[i][j].val == -1)) {
                        i++;
                    }
                    if (i > LINES) break;

                    z = i;
                    y = 0;
                    while (i <= LINES && (matrix[i][j].val != ' ' && matrix[i][j].val != -1)) {
                        matrix[i][j].is_head = false;
                        if (changes) {
                            if (rand() % 8 == 0) matrix[i][j].val = (int) rand() % randnum + randmin;
                        }
                        i++;
                        y++;
                    }
                    if (i > LINES) {
                        matrix[z][j].val = ' ';
                        continue;
                    }

                    matrix[i][j].val = (int) rand() % randnum + randmin;
                    matrix[i][j].is_head = true;

                    if (y > length[j] || firstcoldone) {
                        matrix[z][j].val = ' ';
                        matrix[0][j].val = -1;
                    }
                    firstcoldone = 1;
                    i++;
                }
            }

            y = 1;
            z = LINES;
            for (i = y; i <= z; i++) {
                move(i - y, j);

                if (matrix[i][j].val == 0 || (matrix[i][j].is_head && !rainbow)) {
                    attron(COLOR_PAIR(COLOR_WHITE));
                    if (bold) attron(A_BOLD);
                    if (matrix[i][j].val == 0) {
                        /* Upstream's "flash" head character -- codepoint
                         * 183 in its console/xwindow font-remap modes
                         * (dropped, see this file's top comment), plain
                         * '&' otherwise, which is the path this port
                         * always takes. */
                        addch('&');
                    } else if (matrix[i][j].val == -1) {
                        addch(' ');
                    } else {
                        addch((unsigned char) matrix[i][j].val);
                    }
                    if (bold) attroff(A_BOLD);
                    attroff(COLOR_PAIR(COLOR_WHITE));
                } else {
                    if (rainbow) {
                        /* Matches upstream exactly, including its own
                         * existing quirk: this mutates mcolor itself, so
                         * if rainbow mode is toggled back off afterward,
                         * mcolor is left at whatever it last randomly
                         * landed on rather than reverting -- not
                         * something to "fix" here, just preserved. */
                        switch (rand() % 6) {
                            case 0: mcolor = COLOR_GREEN; break;
                            case 1: mcolor = COLOR_BLUE; break;
                            case 2: mcolor = COLOR_BLACK; break;
                            case 3: mcolor = COLOR_YELLOW; break;
                            case 4: mcolor = COLOR_CYAN; break;
                            case 5: mcolor = COLOR_MAGENTA; break;
                        }
                    }
                    attron(COLOR_PAIR(mcolor));
                    if (matrix[i][j].val == 1) {
                        if (bold) attron(A_BOLD);
                        addch('|');
                        if (bold) attroff(A_BOLD);
                    } else {
                        if (bold == 2 || (bold == 1 && matrix[i][j].val % 2 == 0)) {
                            attron(A_BOLD);
                        }
                        if (matrix[i][j].val == -1) {
                            addch(' ');
                        } else if (lambda && matrix[i][j].val != ' ') {
                            /* Upstream prints the real Unicode lambda
                             * character here; that needs wide-char
                             * ncurses, which this build doesn't have
                             * (see this file's top comment). A plain
                             * ASCII stand-in keeps the 'm' toggle
                             * meaningful without it. */
                            addch('$');
                        } else {
                            addch((unsigned char) matrix[i][j].val);
                        }
                        if (bold == 2 || (bold == 1 && matrix[i][j].val % 2 == 0)) {
                            attroff(A_BOLD);
                        }
                    }
                    attroff(COLOR_PAIR(mcolor));
                }
            }
        }

        napms(update * 10);
    }
}
