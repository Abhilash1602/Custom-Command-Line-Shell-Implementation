#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ncurses.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <linux/limits.h>
#include <locale.h>
#include <ctype.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ctrl(x) ((x) & 0x1f)
#define ENTER 10
#define SHELL "[custom_shell]$ "
#define DATA_START_CAPACITY 128
#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 64
#define DELIMITERS " \t\r\n\a"

typedef struct {
    char* data;
    size_t count;
    size_t capacity;
} String;

typedef struct {
    String* data;
    size_t count;
    size_t capacity;
} Strings;

typedef struct {
    int cursor_pos;
    String clipboard;
    String current_cmd;
    Strings history;
    int history_pos;
    bool searching;
    String search_term;
    int current_line;
} ShellState;

// Function declarations
void shell_initialize(void);
void shell_terminate(void);
void shell_interactive_loop(void);
char* get_formatted_cwd(void);
char** parse_command(char* line);
int execute_command(char** args);
int handle_cd(char** args);
int handle_exit(char** args);
void execute_help_command(void);
void handle_io_redirection(char** args, int* in_fd, int* out_fd);
void handle_cursor_movement(int ch, ShellState* state);
void handle_line_editing(int ch, ShellState* state);
void handle_history(int ch, ShellState* state);
void handle_search(int ch, ShellState* state);
void redraw_prompt(ShellState* state);
void clear_screen_keep_prompt(ShellState* state);

// String handling functions
void string_init(String* str) {
    str->data = NULL;
    str->count = 0;
    str->capacity = 0;
}

void string_append(String* str, char ch) {
    if (str->count >= str->capacity) {
        str->capacity = str->capacity == 0 ? DATA_START_CAPACITY : str->capacity * 2;
        char* new_data = realloc(str->data, str->capacity);
        if (!new_data) {
            endwin();
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
        str->data = new_data;
    }
    str->data[str->count++] = ch;
}

void string_clear(String* str) {
    if (str->data) {
        free(str->data);
        string_init(str);
    }
}

// Shell state initialization
void init_shell_state(ShellState* state) {
    string_init(&state->current_cmd);
    string_init(&state->clipboard);
    state->cursor_pos = 0;
    state->history_pos = -1;
    state->searching = false;
    string_init(&state->search_term);
    state->history.data = NULL;
    state->history.count = 0;
    state->history.capacity = 0;
}

char* get_timestamp() {
    static char buffer[26];
    time_t timer;
    struct tm* tm_info;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    return buffer;
}

// Main shell loop
void shell_interactive_loop(void) {
    shell_initialize();
    ShellState state;
    init_shell_state(&state);
    
    int ch;
    bool running = true;
    
    while (running) {
        redraw_prompt(&state);
        ch = getch();
        
        if (ch == ctrl('d') && state.current_cmd.count == 0) {
            running = false;
            continue;
        }
        
        if (state.searching) {
            handle_search(ch, &state);
            continue;
        }
        
        switch (ch) {
            case ctrl('a'): // Move to beginning
                state.cursor_pos = 0;
                break;
                
            case ctrl('e'): // Move to end
                state.cursor_pos = state.current_cmd.count;
                break;
                
            case ctrl('b'): // Move backward
                if (state.cursor_pos > 0) state.cursor_pos--;
                break;
                
            case ctrl('f'): // Move forward
                if (state.cursor_pos < state.current_cmd.count) state.cursor_pos++;
                break;
                
            case ctrl('k'): // Cut after cursor
                if (state.cursor_pos < state.current_cmd.count) {
                    string_clear(&state.clipboard);
                    for (size_t i = state.cursor_pos; i < state.current_cmd.count; i++) {
                        string_append(&state.clipboard, state.current_cmd.data[i]);
                    }
                    state.current_cmd.count = state.cursor_pos;
                }
                break;
                
            case ctrl('u'): // Cut before cursor
                if (state.cursor_pos > 0) {
                    string_clear(&state.clipboard);
                    for (size_t i = 0; i < state.cursor_pos; i++) {
                        string_append(&state.clipboard, state.current_cmd.data[i]);
                    }
                    memmove(state.current_cmd.data, 
                           state.current_cmd.data + state.cursor_pos,
                           state.current_cmd.count - state.cursor_pos);
                    state.current_cmd.count -= state.cursor_pos;
                    state.cursor_pos = 0;
                }
                break;
                
            case ctrl('y'): // Paste
                if (state.clipboard.count > 0) {
                    // Make space for paste
                    if (state.current_cmd.count + state.clipboard.count >= state.current_cmd.capacity) {
                        size_t new_cap = (state.current_cmd.count + state.clipboard.count) * 2;
                        char* new_data = realloc(state.current_cmd.data, new_cap);
                        if (!new_data) {
                            endwin();
                            fprintf(stderr, "Memory allocation failed\n");
                            exit(1);
                        }
                        state.current_cmd.data = new_data;
                        state.current_cmd.capacity = new_cap;
                    }
                    
                    // Move existing content
                    memmove(state.current_cmd.data + state.cursor_pos + state.clipboard.count,
                           state.current_cmd.data + state.cursor_pos,
                           state.current_cmd.count - state.cursor_pos);
                    
                    // Insert clipboard content
                    memcpy(state.current_cmd.data + state.cursor_pos,
                           state.clipboard.data,
                           state.clipboard.count);
                    
                    state.cursor_pos += state.clipboard.count;
                    state.current_cmd.count += state.clipboard.count;
                }
                break;
                
            case ctrl('l'): // Clear screen
                clear_screen_keep_prompt(&state);
                break;
                
            case ctrl('r'): // Reverse search
                state.searching = true;
                string_clear(&state.search_term);
                break;

            case KEY_UP:
                if (state.history.count > 0) {
                    if (state.history_pos == -1) {
                        state.history_pos = state.history.count - 1;
                    } else if (state.history_pos > 0) {
                        state.history_pos--;
                    }
                    string_clear(&state.current_cmd);
                    for (size_t i = 0; i < state.history.data[state.history_pos].count; i++) {
                        string_append(&state.current_cmd, state.history.data[state.history_pos].data[i]);
                    }
                    state.cursor_pos = state.current_cmd.count;
                }
                break;
                
            case KEY_DOWN:
                if (state.history_pos != -1) {
                    if (state.history_pos < state.history.count - 1) {
                        state.history_pos++;
                        string_clear(&state.current_cmd);
                        for (size_t i = 0; i < state.history.data[state.history_pos].count; i++) {
                            string_append(&state.current_cmd, state.history.data[state.history_pos].data[i]);
                        }
                    } else {
                        state.history_pos = -1;
                        string_clear(&state.current_cmd);
                    }
                    state.cursor_pos = state.current_cmd.count;
                }
                break;

            case ENTER:
                if (state.current_cmd.count > 0) {
                    string_append(&state.current_cmd, '\0');
                    char** args = parse_command(state.current_cmd.data);
                    if (args != NULL) {
                        if (strcmp(args[0], "exit") == 0) {
                            running = false;
                        } else if (strcmp(args[0], "cd") == 0) {
                            handle_cd(args);
                        } else if (strcmp(args[0], "help") == 0) {
                            execute_help_command();
                        } else {
                            printw("\n");  // New line before command output
                            execute_command(args);
                        }
                        free(args);
                    }
                    
                    // Add to history
                    String hist_cmd = {
                        .data = strdup(state.current_cmd.data),
                        .count = state.current_cmd.count,
                        .capacity = state.current_cmd.count
                    };
                    if (state.history.count >= state.history.capacity) {
                        size_t new_cap = state.history.capacity == 0 ? 
                            DATA_START_CAPACITY : state.history.capacity * 2;
                        String* new_data = realloc(state.history.data, 
                                                 new_cap * sizeof(String));
                        if (!new_data) {
                            endwin();
                            fprintf(stderr, "Memory allocation failed\n");
                            exit(1);
                        }
                        state.history.data = new_data;
                        state.history.capacity = new_cap;
                    }
                    state.history.data[state.history.count++] = hist_cmd;
                    
                    string_clear(&state.current_cmd);
                    state.cursor_pos = 0;
                    state.history_pos = -1;
                    state.current_line += 1;  // Move down one lines after command
                    if (state.current_line >= LINES - 1) {
                        scroll(stdscr);
                        state.current_line = LINES - 2;
                    }
                }
                break;
                
            case KEY_BACKSPACE:
            case 127:
                if (state.cursor_pos > 0) {
                    memmove(state.current_cmd.data + state.cursor_pos - 1,
                           state.current_cmd.data + state.cursor_pos,
                           state.current_cmd.count - state.cursor_pos);
                    state.current_cmd.count--;
                    state.cursor_pos--;
                    redraw_prompt(&state);
                }
                break;
                
            default:
                if (isprint(ch)) {
                    if (state.current_cmd.count >= state.current_cmd.capacity) {
                        size_t new_cap = state.current_cmd.capacity == 0 ? 
                            DATA_START_CAPACITY : state.current_cmd.capacity * 2;
                        char* new_data = realloc(state.current_cmd.data, new_cap);
                        if (!new_data) {
                            endwin();
                            fprintf(stderr, "Memory allocation failed\n");
                            exit(1);
                        }
                        state.current_cmd.data = new_data;
                        state.current_cmd.capacity = new_cap;
                    }
                    memmove(state.current_cmd.data + state.cursor_pos + 1,
                           state.current_cmd.data + state.cursor_pos,
                           state.current_cmd.count - state.cursor_pos);
                    state.current_cmd.data[state.cursor_pos] = ch;
                    state.current_cmd.count++;
                    state.cursor_pos++;
                }
                break;
        }
    }
    
    // Cleanup
    string_clear(&state.current_cmd);
    string_clear(&state.clipboard);
    string_clear(&state.search_term);
    for (size_t i = 0; i < state.history.count; i++) {
        string_clear(&state.history.data[i]);
    }
    free(state.history.data);
    
    shell_terminate();
}

void execute_help_command(void) {
    printw("\n\nAvailable Commands:\n");
    printw("------------------\n");
    printw("cd [directory]     : Change current directory\n");
    printw("help              : Display this help message\n");
    printw("exit              : Exit the shell\n");
    printw("ls [directory]    : List directory contents\n");
    printw("[cmd] < [input]   : Redirect input from file\n");
    printw("[cmd] > [output]  : Redirect output to file\n");
    printw("\nKeyboard Shortcuts:\n");
    printw("-----------------\n");
    printw("CTRL+A : Move to beginning of line\n");
    printw("CTRL+E : Move to end of line\n");
    printw("CTRL+K : Cut text after cursor\n");
    printw("CTRL+U : Cut text before cursor\n");
    printw("CTRL+Y : Paste cut text\n");
    printw("CTRL+R : Search command history\n");
    printw("UP     : Previous command\n");
    printw("DOWN   : Next command\n");
    printw("\n");
    refresh();
}

void handle_io_redirection(char** args, int* in_fd, int* out_fd) {
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0 && args[i + 1] != NULL) {
            *in_fd = open(args[i + 1], O_RDONLY);
            if (*in_fd == -1) {
                printw("\nError opening input file: %s\n", strerror(errno));
                refresh();
            }
            args[i] = NULL;  // Remove redirection from arguments
        }
        else if (strcmp(args[i], ">") == 0 && args[i + 1] != NULL) {
            *out_fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (*out_fd == -1) {
                printw("\nError opening output file: %s\n", strerror(errno));
                refresh();
            }
            args[i] = NULL;  // Remove redirection from arguments
        }
    }
}

void redraw_prompt(ShellState* state) {
    move(state->current_line, 0);
    clrtoeol();
    char* cwd = get_formatted_cwd();
    int prompt_len = strlen(SHELL);
    int cwd_len = strlen(cwd);
    
    // Print prompt and current directory with fixed spacing
    printw("%s%s  ", SHELL, cwd);  // Two spaces after cwd
    
    // Print user input
    if (state->current_cmd.count > 0) {
        printw("%.*s", (int)state->current_cmd.count, state->current_cmd.data);
    }
    
    // Calculate cursor position including the fixed spaces
    int base_pos = prompt_len + cwd_len + 2;  // +2 for the two spaces after cwd
    move(state->current_line, base_pos + state->cursor_pos);
    refresh();
}


void clear_screen_keep_prompt(ShellState* state) {
    clear();
    move(0, 0);
    redraw_prompt(state);
}

char* get_formatted_cwd(void) {
    static char formatted[PATH_MAX];
    char cwd[PATH_MAX];
    char* home = getenv("HOME");
    
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return "[ERROR]";
    }
    
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(formatted, sizeof(formatted), "~%s", cwd + strlen(home));
    } else {
        strncpy(formatted, cwd, sizeof(formatted) - 1);
    }
    
    return formatted;
}

int execute_command(char** args) {
    pid_t parent_pid = getpid();
    pid_t pid = fork();
    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;
    
    handle_io_redirection(args, &in_fd, &out_fd);
    
    if (pid == 0) {
        // Child process
        pid_t child_pid = getpid();
        printw("\n[%s] Child process created - Parent PID: %d, Child PID: %d, Command: %s\n", 
               get_timestamp(), parent_pid, child_pid, args[0]);

        refresh();
        
        // Add sleep to give time to check process tree
        sleep(700);  // Sleep for 700 seconds before executing command
       
        if (in_fd != STDIN_FILENO) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        }
        if (out_fd != STDOUT_FILENO) {
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
        }
        
        if (execvp(args[0], args) == -1) {
            printw("\nCommand execution failed: %s\n", strerror(errno));
            refresh();
            exit(EXIT_FAILURE);
        }
    } else if (pid < 0) {
        printw("\nFork failed: %s\n", strerror(errno));
        refresh();
        return 0;
    } else {
        // Parent process
        printw("\n[%s] Parent process waiting - PID: %d, Child PID: %d\n", 
               get_timestamp(), parent_pid, pid);
        refresh();

        if (in_fd != STDIN_FILENO) close(in_fd);
        if (out_fd != STDOUT_FILENO) close(out_fd);
        
        int status;
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));

        printw("\n[%s] Child process completed - PID: %d\n", get_timestamp(), pid);
        refresh();
   
    }
    
    return 1;
}

int handle_cd(char** args) {
    char* dir = args[1];
    if (dir == NULL) {
        dir = getenv("HOME");
    }
    
    if (chdir(dir) != 0) {
        printw("\ncd: %s: %s\n", dir, strerror(errno));
        refresh();
        return 1;
    }
    
    return 1;
}

void shell_initialize(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    scrollok(stdscr, TRUE);

    // Add initial PID information
    pid_t shell_pid = getpid();
    printw("Custom Shell started - PID: %d\n", shell_pid);
    refresh();
}

void shell_terminate(void) {
    endwin();
}

char** parse_command(char* line) {
    int bufsize = MAX_ARGS;
    char** tokens = malloc(bufsize * sizeof(char*));
    int position = 0;
    char* token_start = line;
    bool in_quotes = false;
    char quote_char = '\0';

    if (!tokens) {
        printw("\nAllocation error\n");
        refresh();
        return NULL;
    }

    while (*token_start != '\0') {
        // Skip leading whitespace
        while (isspace(*token_start)) token_start++;
        if (*token_start == '\0') break;

        // Handle quoted strings
        if (*token_start == '"' || *token_start == '\'') {
            quote_char = *token_start;
            token_start++;
            char* quote_end = strchr(token_start, quote_char);
            if (quote_end) {
                *quote_end = '\0';
                tokens[position++] = strdup(token_start);
                token_start = quote_end + 1;
                continue;
            }
        }

        // Handle unquoted tokens
        char* token_end = token_start;
        while (*token_end && !isspace(*token_end)) token_end++;
        
        if (*token_end != '\0') {
            *token_end = '\0';
            tokens[position++] = strdup(token_start);
            token_start = token_end + 1;
        } else {
            tokens[position++] = strdup(token_start);
            break;
        }

        if (position >= bufsize) {
            bufsize += MAX_ARGS;
            char** new_tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!new_tokens) {
                for (int i = 0; i < position; i++) {
                    free(tokens[i]);
                }
                free(tokens);
                printw("\nAllocation error\n");
                refresh();
                return NULL;
            }
            tokens = new_tokens;
        }
    }

    tokens[position] = NULL;
    return tokens;
}

void handle_search(int ch, ShellState* state) {
    static int matched_pos = -1;
    
    switch (ch) {
        case ctrl('r'):  // Another ctrl-r press
            // If we have a current match, look for the next one
            if (matched_pos >= 0) {
                int found = 0;
                for (int i = matched_pos - 1; i >= 0; i--) {
                    if (strstr(state->history.data[i].data, state->search_term.data) != NULL) {
                        matched_pos = i;
                        string_clear(&state->current_cmd);
                        for (size_t j = 0; j < state->history.data[i].count; j++) {
                            string_append(&state->current_cmd, state->history.data[i].data[j]);
                        }
                        state->cursor_pos = state->current_cmd.count;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    flash();  // Visual feedback that no more matches were found
                }
            }
            break;
            
        case ctrl('c'):  // Cancel search
        case 27:         // ESC key
            state->searching = false;
            string_clear(&state->search_term);
            matched_pos = -1;
            break;
            
        case ENTER:      // Accept the current selection
            state->searching = false;
            string_clear(&state->search_term);
            matched_pos = -1;
            break;
            
        case KEY_BACKSPACE:
        case 127:
            if (state->search_term.count > 0) {
                state->search_term.count--;
                state->search_term.data[state->search_term.count] = '\0';
                
                // Reset match position
                matched_pos = -1;
                
                // Try to find a match with the updated search term
                if (state->search_term.count > 0) {
                    for (int i = state->history.count - 1; i >= 0; i--) {
                        if (strstr(state->history.data[i].data, state->search_term.data) != NULL) {
                            matched_pos = i;
                            string_clear(&state->current_cmd);
                            for (size_t j = 0; j < state->history.data[i].count; j++) {
                                string_append(&state->current_cmd, state->history.data[i].data[j]);
                            }
                            state->cursor_pos = state->current_cmd.count;
                            break;
                        }
                    }
                }
            }
            break;
            
        default:
            if (isprint(ch)) {
                string_append(&state->search_term, ch);
                state->search_term.data[state->search_term.count] = '\0';
                
                // Look for a match
                matched_pos = -1;
                for (int i = state->history.count - 1; i >= 0; i--) {
                    if (strstr(state->history.data[i].data, state->search_term.data) != NULL) {
                        matched_pos = i;
                        string_clear(&state->current_cmd);
                        for (size_t j = 0; j < state->history.data[i].count; j++) {
                            string_append(&state->current_cmd, state->history.data[i].data[j]);
                        }
                        state->cursor_pos = state->current_cmd.count;
                        break;
                    }
                }
                
                if (matched_pos == -1) {
                    flash();  // Visual feedback that no match was found
                }
            }
            break;
    }
    
    // Update display
    move(getcury(stdscr), 0);
    clrtoeol();
    if (state->searching) {
        printw("(reverse-i-search)`%s': %s", 
               state->search_term.data ? state->search_term.data : "", 
               state->current_cmd.data ? state->current_cmd.data : "");
    } else {
        redraw_prompt(state);
    }
    refresh();
}

int main(void) {
    shell_interactive_loop();
    return EXIT_SUCCESS;
}