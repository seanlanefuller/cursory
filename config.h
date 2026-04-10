#ifndef CONFIG_H
#define CONFIG_H

#include <curses.h>

#define CTRL_KEY(k) ((k) & 0x1F)

typedef enum {
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_FIND,
    CMD_FIND_NEXT,
    CMD_REPLACE,
    CMD_REPLACE_ALL,
    CMD_SELECT_ALL,
    CMD_COPY,
    CMD_CUT,
    CMD_PASTE,
    CMD_TOGGLE_TREE,
    CMD_TOGGLE_EDITOR,
    CMD_TOGGLE_AI,
    CMD_TOGGLE_TERMINAL,
    CMD_QUIT,
    CMD_MENU,
    CMD_RUN_PYTHON,
    CMD_NONE
} CommandType;

typedef struct {
    int key;            // Primary key or 27 for Alt prefix
    int alt_key;        // Secondary key if key is 27
    CommandType cmd;
    const char *label;
    const char *shortcut_str;
} KeyBinding;

static const KeyBinding key_bindings[] = {
    {CTRL_KEY('s'), 0,   CMD_SAVE,            "Save File",         "Ctrl+S"},
    {0,             0,   CMD_SAVE_AS,         "Save As...",        ""},
    {CTRL_KEY('f'), 0,   CMD_FIND,            "Find",              "Ctrl+F"},
    {KEY_F(3),      0,   CMD_FIND_NEXT,       "Find Next",         "F3"},
    {CTRL_KEY('h'), 0,   CMD_REPLACE,         "Replace",           "Ctrl+H"},
    {27,            'r', CMD_REPLACE_ALL,     "Replace All",       "Alt+R"},
    {CTRL_KEY('a'), 0,   CMD_SELECT_ALL,      "Select All",        "Ctrl+A"},
    {CTRL_KEY('c'), 0,   CMD_COPY,            "Copy",              "Ctrl+C"},
    {CTRL_KEY('x'), 0,   CMD_CUT,             "Cut",               "Ctrl+X"},
    {CTRL_KEY('v'), 0,   CMD_PASTE,           "Paste",             "Ctrl+V"},
    {CTRL_KEY('b'), 0,   CMD_TOGGLE_TREE,     "Toggle File Tree",  "Ctrl+B"},
    {27,            'e', CMD_TOGGLE_EDITOR,   "Toggle Editor",     "Alt+E"},
    {27,            'a', CMD_TOGGLE_AI,       "Toggle AI Chat",    "Alt+A"},
    {27,            't', CMD_TOGGLE_TERMINAL, "Toggle Terminal",   "Alt+T"},
    {CTRL_KEY('r'), 0,   CMD_RUN_PYTHON,      "Run Python Script", "Ctrl+R"},
    {27,            'q', CMD_QUIT,            "Quit Application",  "Alt+Q"},
    {KEY_F(1),      0,   CMD_MENU,            "Command Palette",   "F1"},
    {0, 0, CMD_NONE, NULL, NULL}
};

#endif
