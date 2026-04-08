#include "cursory.h"
#include <ncurses.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>

int main() {
    signal(SIGSEGV, segv_handler); signal(SIGABRT, segv_handler);
    initscr(); raw(); keypad(stdscr, TRUE); noecho(); curs_set(0); 
    mousemask(ALL_MOUSE_EVENTS, NULL); 
    printf("\033[?1003h"); fflush(stdout);
    start_color(); init_pair(1, COLOR_CYAN, COLOR_BLACK);
    
    AppState state; init_app(&state);
    
    int reload_check = 0;
    log_debug("App Main Loop Starting");
    while (state.running) {
        update_terminal(&state); 
        calculate_layout(&state); 
        
        if (++reload_check > 100) {
            reload_check = 0;
            if (state.editor.filepath[0] != '\0' && strcmp(state.editor.filepath, "[No File]") != 0) {
                struct stat st;
                if (stat(state.editor.filepath, &st) == 0 && st.st_mtime > state.editor.last_modified) {
                    log_debug("Auto-reloading file: %s", state.editor.filepath);
                    load_file(&state, state.editor.filepath);
                }
            }
            
            struct stat cur_dir_st;
            if (stat(".", &cur_dir_st) == 0 && cur_dir_st.st_mtime > state.last_dir_modified) {
                state.last_dir_modified = cur_dir_st.st_mtime;
                if (state.root) free_file_tree(state.root);
                state.root = create_file_node(".", ".", true, 0); 
                state.root->is_expanded = true; 
                load_directory(state.root);
            }
        }

        erase();
        attron(A_REVERSE); mvhline(0,0,' ',COLS); mvprintw(0,1," Cursory "); 
        mvprintw(0, 11, " F1=Menu ");
        mvhline(LINES-1,0,' ',COLS); mvprintw(LINES-1,1," [%s] ", state.last_action); attroff(A_REVERSE);
        
        draw_panels(&state); refresh(); handle_input(&state);
    }

    
    printf("\033[?1003l"); fflush(stdout);
    endwin(); 
    return 0;
}
