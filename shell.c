#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ncurses.h>
#include <string.h>

#define ctrl(x) ((x) & 0x1f)
#define ENTER 10
#define SHELL "[TIMBEE 2.0]$ "
#define DATA_START_CAPACITY 128

#define ASSERT(cond, ...)                                      \
    do {                                                      \
        if (!(cond)) {                                        \
            fprintf(stderr, "%s:%d: ASSERTION FAILED: ",      \
                    __FILE__, __LINE__);                      \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                            \
            exit(1);                                          \
        }                                                     \
    } while (0)

#define DA_APPEND(da, item)                                    \
    do {                                                      \
        if ((da)->count >= (da)->capacity) {                  \
            (da)->capacity = (da)->capacity == 0              \
                               ? DATA_START_CAPACITY          \
                               : (da)->capacity * 2;          \
            void *new = realloc((da)->data,                   \
                               (da)->capacity * sizeof(*(da)->data)); \
            ASSERT(new, "out of memory");                     \
            (da)->data = new;                                 \
        }                                                     \
        (da)->data[(da)->count++] = (item);                   \
    } while (0)

typedef struct {
    char *data;
    size_t count;
    size_t capacity;
} String;

typedef struct {
    String *data;
    size_t count;
    size_t capacity;
} Strings;

int main() {
    initscr();
    raw();
    noecho();

    bool QUIT = false;

    int ch;

    String command = {NULL, 0, 0};
    Strings command_history = {NULL, 0, 0};

    size_t line = 0;

    while (!QUIT) {
        mvprintw(line, 0, SHELL);
        mvprintw(line, 0 + sizeof(SHELL) - 1, "%.*s", (int)command.count, command.data);
        ch = getch();

        switch (ch) {
            case ctrl('q'):
                QUIT = true;
                break;
            case KEY_ENTER:
            case ENTER:
                line++;
                mvprintw(line, 0, "'%.*s' is not recognized as an internal or external command",
                         (int)command.count, command.data);
                line++;
                DA_APPEND(&command_history, command);
                command = (String){NULL, 0, 0}; // Reset the command
                break;
            default:
                DA_APPEND(&command, ch);
                break;
        }
    }

    refresh();
    endwin();

    for (size_t i = 0; i < command_history.count; i++) {
        printf("%.*s\n", (int)command_history.data[i].count, command_history.data[i].data);
        free(command_history.data[i].data);
    }
    free(command_history.data);

    return 0;
}
