#include "dropdown.h"
#include "utils.h"

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct termios orig_termios;
static int raw_mode_enabled = 0;

static void disable_raw_mode(void) {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_enabled = 0;
        // Show cursor again
        printf("\033[?25h");
        fflush(stdout);
    }
}

static int enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return -1;
    
    struct termios raw = orig_termios;
    // Disable canonical mode (buffered I/O) and local echo. Keep ISIG to allow Ctrl-C.
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
    raw_mode_enabled = 1;
    return 0;
}

enum Key {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_ENTER,
    KEY_ESC,
    KEY_CTRL_C
};

static int read_key(void) {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return -1;

    if (c == 3) { // Ctrl-C
        return KEY_CTRL_C;
    }

    if (c == '\033') {
        char seq[2];
        struct termios temp_raw;
        tcgetattr(STDIN_FILENO, &temp_raw);
        struct termios non_blocking = temp_raw;
        non_blocking.c_cc[VMIN] = 0;
        non_blocking.c_cc[VTIME] = 1; // 100ms timeout
        tcsetattr(STDIN_FILENO, TCSANOW, &non_blocking);

        int r1 = read(STDIN_FILENO, &seq[0], 1);
        int r2 = 0;
        if (r1 == 1) {
            r2 = read(STDIN_FILENO, &seq[1], 1);
        }

        tcsetattr(STDIN_FILENO, TCSANOW, &temp_raw);

        if (r1 != 1 || r2 != 1) return KEY_ESC;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
            }
        }
        return KEY_ESC;
    }

    if (c == '\n' || c == '\r') {
        return KEY_ENTER;
    }

    // Support vi keys and common alternatives
    if (c == 'k' || c == 'w' || c == 'p') return KEY_UP;
    if (c == 'j' || c == 's' || c == 'n') return KEY_DOWN;

    return c;
}

static void draw_menu(const char *title, const char *options[], int num_options, int current_selection) {
    // Print title in bold cyan
    printf("\033[1;36m? %s\033[0m \033[37m(Use ↑/↓ or j/k to navigate, Enter to select):\033[0m\n", title);
    for (int i = 0; i < num_options; i++) {
        if (i == current_selection) {
            // Highlight selected in bold green with a cursor
            printf("  \033[1;32m❯ %s\033[0m\n", options[i]);
        } else {
            printf("    %s\n", options[i]);
        }
    }
    fflush(stdout);
}

int dropdown_select(const char *title, const char *options[], int num_options, int default_index) {
    if (num_options <= 0 || !options) return -1;
    if (default_index < 0 || default_index >= num_options) default_index = 0;

    // Check if stdin is a TTY. If not, default immediately
    if (!isatty(STDIN_FILENO)) {
        return default_index;
    }

    if (enable_raw_mode() != 0) {
        return default_index;
    }

    // Hide cursor for a cleaner look
    printf("\033[?25l");
    fflush(stdout);

    int selected = default_index;
    draw_menu(title, options, num_options, selected);

    while (1) {
        int key = read_key();
        if (key == KEY_UP) {
            selected = (selected - 1 + num_options) % num_options;
        } else if (key == KEY_DOWN) {
            selected = (selected + 1) % num_options;
        } else if (key == KEY_ENTER) {
            break;
        } else if (key == KEY_ESC || key == KEY_CTRL_C || key == -1) {
            selected = -1; // Canceled
            break;
        }

        // Clear and redraw
        for (int i = 0; i < num_options + 1; i++) {
            printf("\033[A\033[2K");
        }
        printf("\r");
        draw_menu(title, options, num_options, selected);
    }

    // Clear menu and print final result
    for (int i = 0; i < num_options + 1; i++) {
        printf("\033[A\033[2K");
    }
    printf("\r");

    if (selected != -1) {
        printf("\033[1;32m✔\033[0m \033[1m%s\033[0m: \033[1;36m%s\033[0m\n", title, options[selected]);
    } else {
        printf("\033[1;31m✘\033[0m \033[1m%s\033[0m: \033[31mCanceled\033[0m\n", title);
    }
    fflush(stdout);

    disable_raw_mode();
    return selected;
}

char *prompt_text_input(const char *prompt, int is_password) {
    if (!isatty(STDIN_FILENO)) {
        char buf[1024];
        if (fgets(buf, sizeof(buf), stdin)) {
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
            return duplicate_string(buf);
        }
        return NULL;
    }

    printf("\033[1;36m? %s\033[0m: ", prompt);
    fflush(stdout);

    if (enable_raw_mode() != 0) {
        char buf[1024];
        if (fgets(buf, sizeof(buf), stdin)) {
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
            return duplicate_string(buf);
        }
        return NULL;
    }

    char input_buf[1024];
    int input_len = 0;

    // Show cursor for writing input
    printf("\033[?25h");
    fflush(stdout);

    while (input_len < (int)sizeof(input_buf) - 1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == '\n' || c == '\r') {
            break;
        } else if (c == 127 || c == 8) { // Backspace / Del
            if (input_len > 0) {
                input_len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == 3 || c == 27) { // Ctrl-C or Esc
            input_len = 0;
            break;
        } else if (c >= 32 && c <= 126) {
            input_buf[input_len++] = c;
            if (is_password) {
                printf("*");
            } else {
                printf("%c", c);
            }
            fflush(stdout);
        }
    }

    input_buf[input_len] = '\0';
    disable_raw_mode();
    printf("\n");

    if (input_len == 0) return NULL;
    return duplicate_string(input_buf);
}
