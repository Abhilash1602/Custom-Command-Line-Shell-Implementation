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
#include <limits.h>    // For PATH_MAX on most systems
#include <linux/limits.h>  // For PATH_MAX on some Linux systems

// Fallback definition if PATH_MAX is not defined
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ctrl(x) ((x) & 0x1f)
#define ENTER 10
#define SHELL "[TIMBEE 2.0]$ "
#define DATA_START_CAPACITY 128
#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 64
#define DELIMITERS " \t\r\n\a"

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

// Existing function declarations
void shell_initialize(void);
void shell_terminate(void);
int process_command(char* command_line);
char** parse_command(char* line);
int execute_external_command(char** args, int in_fd, int out_fd);
void list_directory(char** args);
int is_executable(const char* path);
int handle_cd(char** args);
int handle_help(char** args);
int shell_exit(char** args);
char* get_formatted_cwd(void);
void handle_io_redirection(char** args, int* in_fd, int* out_fd);
int count_args(char** args);
char** remove_redirection_args(char** args);


// String handling functions
void string_init(String *str) {
    str->data = NULL;
    str->count = 0;
    str->capacity = 0;
}

void string_append(String *str, char ch) {
    DA_APPEND(str, ch);
}

void string_clear(String *str) {
    free(str->data);
    string_init(str);
}

// Get formatted current working directory
char* get_formatted_cwd(void) {
    char cwd[PATH_MAX];
    char* home = getenv("HOME");
    static char formatted[PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return "[ERROR]";
    }

    // Replace home directory with ~
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(formatted, sizeof(formatted), "~%s", cwd + strlen(home));
    } else {
        strncpy(formatted, cwd, sizeof(formatted) - 1);
    }

    return formatted;
}

char** remove_redirection_args(char** args) {
    int count = 0;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0 || strcmp(args[i], ">") == 0) {
            i++; // Skip the redirection operator and filename
        } else {
            count++;
        }
    }

    char** clean_args = malloc((count + 1) * sizeof(char*));
    int j = 0;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0 || strcmp(args[i], ">") == 0) {
            i++; // Skip the redirection operator and filename
        } else {
            clean_args[j++] = args[i];
        }
    }
    clean_args[j] = NULL;

    return clean_args;
}


// Main shell interactive loop
void shell_interactive_loop(void) {
    shell_initialize();

    String command;
    Strings command_history;
    string_init(&command);
    command_history.data = NULL;
    command_history.count = 0;
    command_history.capacity = 0;

    int line = LINES - 1;
    bool QUIT = false;
    int ch;

    // Initial newline for better formatting
    printf("\n");

    do {
        // Clear line before printing prompt
        move(line, 0);
        clrtoeol();

        // Get formatted current working directory
        char* formatted_cwd = get_formatted_cwd();

        // Display prompt with current directory
        mvprintw(line, 0, "timbee-2-0:%s$ ", formatted_cwd);

        // Display current command
        if (command.data) {
            mvprintw(line, strlen("timbee-2-0:") + strlen(formatted_cwd) + 2, 
                    "%.*s", (int)command.count, command.data);
        }
        refresh();

        // Get input
        ch = getch();
        switch (ch) {
            case KEY_ENTER:
            case ENTER:
                if (command.count > 0) {
                    // Move to new line before executing command
                    printf("\n");
                    
                    // Null-terminate the string for processing
                    command.data[command.count] = '\0';

                    // Add to command history
                    String hist_command;
                    hist_command.data = strdup(command.data);
                    hist_command.count = command.count;
                    hist_command.capacity = command.count + 1;
                    DA_APPEND(&command_history, hist_command);

                    // Process the command
                    int result = process_command(command.data);
                    if (result == 0) {
                        QUIT = true;
                    }

                    // Clear the command
                    string_clear(&command);
                    
                    // Move cursor to new line for next prompt
                    printf("\n");
                }
                break;
            case KEY_BACKSPACE:
            case 127:
                if (command.count > 0) {
                    command.count--;
                    command.data[command.count] = '\0';
                }
                break;
            default:
                string_append(&command, ch);
                break;
        }
        refresh();
    } while (!QUIT);

    // Cleanup
    shell_terminate();

    // Print and free command history
    for (size_t i = 0; i < command_history.count; i++) {
        free(command_history.data[i].data);
    }
    free(command_history.data);
    free(command.data);
}

// Existing functions from previous implementation
int is_executable(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR));
}

int process_command(char* command_line) {
    char** args = parse_command(command_line);
    int result = 1;
    int in_fd = STDIN_FILENO;
    int out_fd = STDOUT_FILENO;

    if (args[0] == NULL) {
        free(args);
        return 1;
    }

    // Handle I/O redirection
    handle_io_redirection(args, &in_fd, &out_fd);
    char** clean_args = remove_redirection_args(args);

    if (clean_args[0] == NULL) {
        free(clean_args);
        free(args);
        return 1;
    }

    // Check built-in commands
    if (strcmp(clean_args[0], "cd") == 0) {
        result = handle_cd(clean_args);
    } else if (strcmp(clean_args[0], "help") == 0) {
        result = handle_help(clean_args);
    } else if (strcmp(clean_args[0], "exit") == 0) {
        result = shell_exit(clean_args);
    } else if (strcmp(clean_args[0], "ls") == 0) {
        result = execute_external_command(clean_args, in_fd, out_fd);
    } else {
        result = execute_external_command(clean_args, in_fd, out_fd);
    }

    // Restore standard file descriptors if they were changed
    if (in_fd != STDIN_FILENO) close(in_fd);
    if (out_fd != STDOUT_FILENO) close(out_fd);

    free(clean_args);
    free(args);
    return result;
}


char** parse_command(char* line) {
    int bufsize = MAX_ARGS;
    char** tokens = malloc(bufsize * sizeof(char*));
    char* token;
    int position = 0;
    bool in_quotes = false;
    char* start = line;

    if (!tokens) {
        perror("Allocation error");
        exit(EXIT_FAILURE);
    }

    for (char* p = line; *p != '\0'; p++) {
        if (*p == '\'') {
            in_quotes = !in_quotes;
            if (!in_quotes) {
                *p = '\0';
                tokens[position++] = strdup(start);
                start = p + 1;
            } else {
                start = p + 1;
            }
        } else if (!in_quotes && strchr(DELIMITERS, *p)) {
            *p = '\0';
            if (start != p) {
                tokens[position++] = start;
            }
            start = p + 1;
        }
    }

    if (start != line + strlen(line)) {
        tokens[position++] = start;
    }

    tokens[position] = NULL;
    return tokens;
}



void handle_io_redirection(char** args, int* in_fd, int* out_fd) {
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0 && args[i + 1] != NULL) {
            *in_fd = open(args[i + 1], O_RDONLY);
            if (*in_fd == -1) {
                printf("\nError opening input file '%s': %s\n", 
                       args[i + 1], strerror(errno));
                return;
            }
            // Mark these arguments for removal
            args[i] = NULL;
            args[i + 1] = NULL;
            i++;  // Skip the filename
        } else if (strcmp(args[i], ">") == 0 && args[i + 1] != NULL) {
            *out_fd = open(args[i + 1], 
                          O_WRONLY | O_CREAT | O_TRUNC, 
                          0644);
            if (*out_fd == -1) {
                printf("\nError opening output file '%s': %s\n", 
                       args[i + 1], strerror(errno));
                return;
            }
            // Mark these arguments for removal
            args[i] = NULL;
            args[i + 1] = NULL;
            i++;  // Skip the filename
        }
    }
}

// Modified execute_external_command to handle I/O redirection and print output in a new line

int execute_external_command(char** args, int in_fd, int out_fd) {
    pid_t pid = fork();

    if (pid == 0) {
        // Child process
        // Move cursor to new line before executing command
        printf("\n");
        fflush(stdout);
        
        if (in_fd != STDIN_FILENO) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        }
        if (out_fd != STDOUT_FILENO) {
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
        }

        if (execvp(args[0], args) == -1) {
            printf("Command execution failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    } else if (pid < 0) {
        mvprintw(0, 0, "Fork failed: %s\n", strerror(errno));
        refresh();
        return 0;
    } else {
        // Parent process
        int status;
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));

        // Print a new line after command execution
        printf("\n");
        fflush(stdout);
    }

    return 1;
}

// Updated handle_cd function to fix snprintf warning
int handle_cd(char** args) {
    char* home = getenv("HOME");
    char* oldpwd = getenv("OLDPWD");
    char current[PATH_MAX];
    char resolved_path[PATH_MAX];
    bool follow_symlinks = true;
    int i = 1;

    // Save current directory
    if (getcwd(current, sizeof(current)) == NULL) {
        mvprintw(0, 0, "cd: error getting current directory: %s\n", strerror(errno));
        refresh();
        return 1;
    }

    // Parse options
    while (args[i] != NULL && args[i][0] == '-') {
        if (strcmp(args[i], "-P") == 0) {
            follow_symlinks = false;
        } else if (strcmp(args[i], "-L") == 0) {
            follow_symlinks = true;
        } else if (strcmp(args[i], "-") == 0) {
            if (oldpwd) {
                if (chdir(oldpwd) != 0) {
                    mvprintw(0, 0, "cd: %s\n", strerror(errno));
                    refresh();
                } else {
                    setenv("OLDPWD", current, 1);
                    char* newpwd = getcwd(NULL, 0);
                    setenv("PWD", newpwd, 1);
                    free(newpwd);
                }
                return 1;
            } else {
                mvprintw(0, 0, "cd: OLDPWD not set\n");
                refresh();
                return 1;
            }
        }
        i++;
    }

    // Handle no arguments or '~'
    if (args[i] == NULL || strcmp(args[i], "~") == 0) {
        if (home) {
            if (chdir(home) != 0) {
                mvprintw(0, 0, "cd: %s\n", strerror(errno));
                refresh();
            } else {
                setenv("OLDPWD", current, 1);
                char* newpwd = getcwd(NULL, 0);
                setenv("PWD", newpwd, 1);
                free(newpwd);
            }
        } else {
            mvprintw(0, 0, "cd: HOME not set\n");
            refresh();
        }
        return 1;
    }

    // Handle quoted paths and construct full path
    char* dir = args[i];
    char* unquoted_path = NULL;
    
    // Remove quotes if present
    if (dir[0] == '\'' && dir[strlen(dir) - 1] == '\'') {
        unquoted_path = strdup(dir + 1);
        unquoted_path[strlen(unquoted_path) - 1] = '\0';
    } else {
        unquoted_path = strdup(dir);
    }

    // Construct full path for relative paths
    if (unquoted_path[0] != '/') {
        if (snprintf(resolved_path, PATH_MAX, "%s/%s", current, unquoted_path) >= PATH_MAX) {
            mvprintw(0, 0, "cd: path too long\n");
            refresh();
            free(unquoted_path);
            return 1;
        }
    } else {
        strncpy(resolved_path, unquoted_path, PATH_MAX - 1);
        resolved_path[PATH_MAX - 1] = '\0';
    }
    
    free(unquoted_path);

    // Change directory
    if (chdir(resolved_path) != 0) {
        mvprintw(0, 0, "cd: %s: %s\n", resolved_path, strerror(errno));
        refresh();
    } else {
        setenv("OLDPWD", current, 1);
        char* newpwd = getcwd(NULL, 0);
        setenv("PWD", newpwd, 1);
        free(newpwd);
    }

    return 1;
}

int handle_help(char** args) {
    pid_t pid = fork();

    if (pid == 0) {
        // Child process
        printf("\nAvailable Commands:\n");
        printf("------------------\n");
        printf("cd [directory] [-P]    : Change current directory\n");
        printf("help                   : Display this help message\n");
        printf("exit                   : Exit the shell\n");
        printf("ls [directory]         : List directory contents\n");
        printf("[command] < [input]    : Redirect input from file\n");
        printf("[command] > [output]   : Redirect output to file\n");
        printf("\nCustom executables in current directory are also supported.\n\n");
        exit(EXIT_SUCCESS);
    } else if (pid < 0) {
        mvprintw(0, 0, "Fork failed: %s\n", strerror(errno));
        refresh();
        return 0;
    } else {
        // Parent process
        int status;
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }

    return 1;
}

int shell_exit(char** args) {
    return 0;
}

void list_directory(char** args) {
    pid_t pid = fork();

    if (pid == 0) {
        // Child process
        char* ls_args[] = {"ls", args[1] ? args[1] : ".", NULL};
        if (execvp(ls_args[0], ls_args) == -1) {
            mvprintw(0, 0, "Command execution failed: %s\n", strerror(errno));
            refresh();
            exit(EXIT_FAILURE);
        }
    } else if (pid < 0) {
        mvprintw(0, 0, "Fork failed: %s\n", strerror(errno));
        refresh();
        return;
    } else {
        // Parent process
        int status;
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
}

void shell_initialize(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
}

void shell_terminate(void) {
    endwin();
}

int main(void) {
    shell_interactive_loop();
    return EXIT_SUCCESS;
}