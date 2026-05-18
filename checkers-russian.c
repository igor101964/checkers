/*
 * checkers.c — Polished Checkers game (SDL2 + SDL2_ttf)
 * Project: Art2Dec SoftLab
 *
 * Build:
 *   gcc -Wall -Wextra -g checkers.c -o checkers -lSDL2 -lSDL2_ttf -lm -lpthread
 *
 * Dependencies:
 *   sudo apt install libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core
 *   sudo apt install zenity   # for Save/Load file dialogs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pthread.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BOARD_SIZE 8
#define PANEL_MIN_WIDTH 260

static int g_win_w = 1100;
static int g_win_h = 720;
static int g_sq    = 64;
static int g_ox    = 50;
static int g_oy    = 50;

#define SQUARE_SIZE    g_sq
#define BOARD_OFFSET_X g_ox
#define BOARD_OFFSET_Y g_oy

static int g_list_top = 400;
static int g_panel_px = 580;
static int g_btn_bw   = 130;

typedef struct {
    char result[1024];
    int  done;
    int  mode;
    int  busy;
} FileDialogCtx;
static FileDialogCtx g_fdlg = {"", 0, 0, 0};

typedef struct {
    int board[BOARD_SIZE][BOARD_SIZE];
    int player;
    int valid;
} BoardSnap;
static BoardSnap g_snaps[1000];  /* снапшоты доски после каждого хода */

#define PANEL_WIDTH 300

#define EMPTY 0
#define WHITE_MAN 1
#define WHITE_KING 2
#define BLACK_MAN 3
#define BLACK_KING 4

#define WHITE 0
#define BLACK 1

#define CLASSIC_MODE 0
#define GIVEAWAY_MODE 1

#define MANUAL 0
#define SEMI_AUTO 1
#define FULL_AUTO 2

#define REPLAY_STOPPED 0
#define REPLAY_PLAYING 1
#define REPLAY_PAUSED 2

#define MAX_MOVES 1000
#define MAX_LEGAL_MOVES 50

typedef struct {
    int from_row, from_col;
    int to_row, to_col;
    int captured_pieces[10][2];
    int num_captures;
    char notation[50];
} Move;

typedef struct {
    int board[BOARD_SIZE][BOARD_SIZE];
    int current_player;
    int selected_row, selected_col;
    Move legal_moves[MAX_LEGAL_MOVES];
    int num_legal_moves;
    int has_capture;
    int must_continue_capture;
    int multi_capture_row, multi_capture_col;
    Move move_history[MAX_MOVES];
    int move_count;
    int rule_mode;
    int play_mode;
    int game_over;
    int winner;
    int replay_state;
    int replay_index;
    double replay_delay;
    Uint32 last_replay_time;
    int    move_list_scroll;
    int    game_paused;
    int    game_started;
    Uint32 computer_move_delay;
    Uint32 last_computer_move_time;
    int    browse_mode;
    int    browse_index;
    Uint32 save_msg_timer;
    Uint32 load_msg_timer;
    int    ai_level;
} GameState;

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
TTF_Font* font = NULL;
GameState game;

void init_board(void);
void reset_game(void);
int is_valid_square(int row, int col);
int get_piece_at(int row, int col);
void set_piece_at(int row, int col, int piece);
void generate_legal_moves(void);
int is_legal_move(int from_row, int from_col, int to_row, int to_col);
void make_move(Move* move);
void get_move_notation(Move* move);
int is_game_over(void);
void computer_move(void);
void handle_click(int x, int y);
void render_game(void);
void render_board(void);
void render_pieces(void);
void render_ui(void);
void render_text(const char* text, int x, int y, SDL_Color color);
int init_sdl(void);
void cleanup_sdl(void);

static void draw_button(SDL_Rect r, const char *label, Uint8 br, Uint8 bg, Uint8 bb) {
    SDL_Rect sh={r.x+2,r.y+2,r.w,r.h};
    SDL_SetRenderDrawColor(renderer,20,40,30,180); SDL_RenderFillRect(renderer,&sh);
    SDL_SetRenderDrawColor(renderer,br,bg,bb,255); SDL_RenderFillRect(renderer,&r);
    SDL_Rect top={r.x+1,r.y+1,r.w-2,r.h/3};
    SDL_SetRenderDrawColor(renderer,(Uint8)(br+40<255?br+40:255),(Uint8)(bg+40<255?bg+40:255),(Uint8)(bb+40<255?bb+40:255),100);
    SDL_RenderFillRect(renderer,&top);
    SDL_SetRenderDrawColor(renderer,20,50,35,255); SDL_RenderDrawRect(renderer,&r);
    SDL_Color tc={240,255,245,255};
    int tw=(int)strlen(label)*7;
    render_text(label,r.x+(r.w-tw)/2,r.y+(r.h-14)/2,tc);
}

static void parse_notation(Move *m) {
    const char *n=m->notation; if(!n||strlen(n)<4) return;
    m->from_col=n[0]-'A'; m->from_row=8-(n[1]-'0');
    int sep=2; if(n[sep]=='-'||n[sep]=='x') sep++;
    m->to_col=n[sep]-'A'; m->to_row=8-(n[sep+1]-'0');
}

/* Forward declarations для goto_move */
typedef struct { int b[BOARD_SIZE][BOARD_SIZE]; int player; } LBoard;
static LBoard lb_apply(const LBoard *src, const Move *mv);
void goto_move(int index){
    if(index<0||index>=game.move_count) return;

    if(index<MAX_MOVES&&g_snaps[index].valid){
        memcpy(game.board,g_snaps[index].board,sizeof(game.board));
        game.current_player=g_snaps[index].player;
    }

    game.selected_row=-1; game.selected_col=-1;
    game.must_continue_capture=0; game.game_over=0;
    generate_legal_moves();
    game.browse_mode=1; game.browse_index=index; game.game_paused=1;
    game.move_list_scroll=index-4;
    if(game.move_list_scroll<0) game.move_list_scroll=0;
}

void play_from_here(void){
    if(!game.browse_mode) return;
    int idx=game.browse_index,rule=game.rule_mode,play=game.play_mode;
    Uint32 del=game.computer_move_delay; int al=game.ai_level;
    int board_copy[BOARD_SIZE][BOARD_SIZE]; int pc;
    if(idx<MAX_MOVES&&g_snaps[idx].valid){memcpy(board_copy,g_snaps[idx].board,sizeof(board_copy));pc=g_snaps[idx].player;}
    else {memcpy(board_copy,game.board,sizeof(board_copy));pc=game.current_player;}
    static Move saved[MAX_MOVES]; memcpy(saved,game.move_history,sizeof(Move)*(idx+1));
    memcpy(game.board,board_copy,sizeof(game.board));
    game.current_player=pc; game.rule_mode=rule; game.play_mode=play;
    game.computer_move_delay=del; game.ai_level=al;
    game.game_started=1; game.game_paused=0; game.game_over=0;
    game.selected_row=-1; game.selected_col=-1;
    game.must_continue_capture=0; game.multi_capture_row=-1; game.multi_capture_col=-1;
    game.browse_mode=0; game.browse_index=-1; game.replay_state=REPLAY_STOPPED;
    memcpy(game.move_history,saved,sizeof(Move)*(idx+1));
    game.move_count=idx+1;
    game.move_list_scroll=idx-4; if(game.move_list_scroll<0)game.move_list_scroll=0;
    generate_legal_moves();
    game.last_computer_move_time=SDL_GetTicks();
}

static void replay_apply(int idx);
static void replay_scroll_to(int ri);
void replay_from_here(void){
    if(!game.browse_mode||game.browse_index<0) return;
    int idx  = game.browse_index;
    int rule = game.rule_mode;
    int play = game.play_mode;
    Uint32 del = game.computer_move_delay;
    int cnt  = game.move_count;
    int al   = game.ai_level;

    /* Сохраняем историю */
    static Move saved[MAX_MOVES];
    memcpy(saved, game.move_history, sizeof(Move)*cnt);

    /* Парсим координаты */
    for(int i=0;i<cnt;i++)
        if(!saved[i].from_row&&!saved[i].to_row&&!saved[i].from_col&&!saved[i].to_col)
            parse_notation(&saved[i]);

    /* Восстанавливаем позицию idx */
    if(idx<MAX_MOVES&&g_snaps[idx].valid){
        /* Быстрый путь — снапшот */
        memcpy(game.board,g_snaps[idx].board,sizeof(game.board));
        game.current_player=g_snaps[idx].player;
    } /* снапшот есть — заполнен при do_load */

    /* Восстанавливаем полную историю и настройки */
    memcpy(game.move_history,saved,sizeof(Move)*cnt);
    game.move_count          = cnt;
    game.rule_mode           = rule;
    game.play_mode           = play;
    game.computer_move_delay = del;
    game.ai_level            = al;
    game.game_started        = 1;
    game.game_paused         = 0;
    game.game_over           = 0;
    game.browse_mode         = 0;
    game.browse_index        = -1;
    game.selected_row        = -1;
    game.selected_col        = -1;
    game.must_continue_capture = 0;
    game.multi_capture_row   = -1;
    game.multi_capture_col   = -1;
    generate_legal_moves();

    /* Запускаем replay с следующего хода */
    game.replay_state     = REPLAY_PLAYING;
    game.replay_index     = idx+1;
    game.last_replay_time = SDL_GetTicks();
    replay_scroll_to(idx);
}


void save_game(void);
void load_game(void);
void replay_start(void);
static void do_save(const char *path);
static void do_load(const char *path);
static void start_file_dialog(int mode);
void goto_move(int index);
void play_from_here(void);
void replay_step(void);
char get_col_char(int col);
int get_col_from_char(char c);
void find_captures(int row, int col, Move* moves, int* count, int depth);
void find_regular_moves(int row, int col, Move* moves, int* count);

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    if (!init_sdl()) {
        return 1;
    }
    
    srand((unsigned int)time(NULL));
    reset_game();
    
    SDL_Event e;
    int quit = 0;
    Uint32 cur;

    while (!quit) {
        cur = SDL_GetTicks();

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { quit = 1; }
            else if (e.type == SDL_WINDOWEVENT &&
                     e.window.event == SDL_WINDOWEVENT_RESIZED) {
                g_win_w = e.window.data1;
                g_win_h = e.window.data2;
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN &&
                     e.button.button == SDL_BUTTON_LEFT) {
                handle_click(e.button.x, e.button.y);
            }
            else if (e.type == SDL_MOUSEWHEEL) {
                int mx, my; SDL_GetMouseState(&mx, &my);
                if (mx >= g_panel_px) {
                    game.move_list_scroll -= e.wheel.y * 2;
                    if (game.move_list_scroll < 0) game.move_list_scroll = 0;
                    int ms = game.move_count - 8;
                    if (ms < 0) ms = 0;
                    if (game.move_list_scroll > ms) game.move_list_scroll = ms;
                }
            }
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                int bi = game.browse_mode ? game.browse_index : game.move_count-1;
                if (k == SDLK_DOWN) {
                    if (bi+1 < game.move_count) goto_move(bi+1);
                } else if (k == SDLK_UP) {
                    if (bi-1 >= 0) goto_move(bi-1);
                    else if (game.browse_mode) { game.browse_mode=0; game.browse_index=-1; }
                } else if (k == SDLK_ESCAPE && game.browse_mode) {
                    game.browse_mode=0; game.browse_index=-1;
                } else if (k == SDLK_RETURN && game.browse_mode) {
                    play_from_here();
                }
            }
        }

        /* File dialog result from pthread */
        if (!g_fdlg.busy && g_fdlg.done != 0) {
            if (g_fdlg.done == 1 && g_fdlg.result[0]) {
                if (g_fdlg.mode == 0) do_save(g_fdlg.result);
                else                  do_load(g_fdlg.result);
            }
            g_fdlg.done = 0; g_fdlg.result[0] = '\0';
        }

        /* Replay */
        if (!game.game_paused && game.replay_state == REPLAY_PLAYING &&
            cur - game.last_replay_time >= (Uint32)(game.replay_delay * 1000)) {
            replay_step();
            game.last_replay_time = cur;
        }

        /* Computer move with delay */
        if (game.last_computer_move_time == 0) game.last_computer_move_time = cur;
        if (game.game_paused) game.last_computer_move_time = cur;
        if (!game.game_over && !game.browse_mode && !game.game_paused &&
            game.game_started && game.replay_state == REPLAY_STOPPED) {
            int comp = (game.play_mode == FULL_AUTO) ||
                       (game.play_mode == SEMI_AUTO && game.current_player == BLACK);
            if (comp && cur - game.last_computer_move_time >= game.computer_move_delay) {
                computer_move();
                game.last_computer_move_time = cur;
            }
        }

        render_game();
        SDL_Delay(16);
    }

    cleanup_sdl();
    return 0;
}

int init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 0;
    }
    
    if (TTF_Init() == -1) {
        printf("SDL_ttf could not initialize! SDL_ttf Error: %s\n", TTF_GetError());
        return 0;
    }
    
    window = SDL_CreateWindow("Checkers", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                             1100, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (window) SDL_SetWindowMinimumSize(window, 1050, 700);
    if (!window) {
        printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 0;
    }
    
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 0;
    }
    
    font = TTF_OpenFont("/System/Library/Fonts/Arial.ttf", 16);
    if (!font) {
        font = TTF_OpenFont("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 16);
        if (!font) {
            font = TTF_OpenFont("C:/Windows/Fonts/arial.ttf", 16);
            if (!font) {
                printf("Failed to load font! SDL_ttf Error: %s\n", TTF_GetError());
                return 0;
            }
        }
    }
    
    return 1;
}

void cleanup_sdl(void) {
    if (font) {
        TTF_CloseFont(font);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    TTF_Quit();
    SDL_Quit();
}

void init_board(void) {
    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            game.board[row][col] = EMPTY;
        }
    }
    
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            if ((row + col) % 2 == 1) {
                game.board[row][col] = BLACK_MAN;
            }
        }
    }
    
    for (int row = 5; row < 8; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            if ((row + col) % 2 == 1) {
                game.board[row][col] = WHITE_MAN;
            }
        }
    }
}

void reset_game(void) {
    init_board();
    game.current_player = WHITE;
    game.selected_row = -1;
    game.selected_col = -1;
    game.num_legal_moves = 0;
    game.has_capture = 0;
    game.must_continue_capture = 0;
    game.multi_capture_row = -1;
    game.multi_capture_col = -1;
    game.move_count = 0;
    game.game_over = 0;
    game.winner = -1;
    game.replay_state = REPLAY_STOPPED;
    game.replay_index = 0;
    game.replay_delay = 1.0;
    game.last_replay_time = 0;
    memset(g_snaps, 0, sizeof(g_snaps));
    /* Новые поля — инициализируем но НЕ трогаем ai_level */
    game.move_list_scroll      = 0;
    game.game_paused           = 0;
    game.game_started          = 0;
    game.computer_move_delay   = 500;
    game.last_computer_move_time = 0;
    game.browse_mode           = 0;
    game.browse_index          = -1;
    game.save_msg_timer        = 0;
    game.load_msg_timer        = 0;
    generate_legal_moves();
}

int is_valid_square(int row, int col) {
    return row >= 0 && row < BOARD_SIZE && col >= 0 && col < BOARD_SIZE;
}

int get_piece_at(int row, int col) {
    if (!is_valid_square(row, col)) return EMPTY;
    return game.board[row][col];
}

void set_piece_at(int row, int col, int piece) {
    if (is_valid_square(row, col)) {
        game.board[row][col] = piece;
    }
}

void find_captures(int row, int col, Move* moves, int* count, int depth) {
    if (*count >= MAX_LEGAL_MOVES) return;
    
    int piece = get_piece_at(row, col);
    if (piece == EMPTY) return;
    
    int is_king = (piece == WHITE_KING || piece == BLACK_KING);
    int player = (piece == WHITE_MAN || piece == WHITE_KING) ? WHITE : BLACK;
    
    int directions[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    int start_dir = 0, end_dir = 4;
    
    if (!is_king && player == WHITE) {
        end_dir = 2;
    } else if (!is_king && player == BLACK) {
        start_dir = 2;
    }
    
    for (int dir = start_dir; dir < end_dir; dir++) {
        int dr = directions[dir][0];
        int dc = directions[dir][1];
        int jump_row = row + dr;
        int jump_col = col + dc;
        int land_row = row + 2 * dr;
        int land_col = col + 2 * dc;
        
        if (!is_valid_square(land_row, land_col)) continue;
        
        int jump_piece = get_piece_at(jump_row, jump_col);
        int land_piece = get_piece_at(land_row, land_col);
        
        if (jump_piece != EMPTY && land_piece == EMPTY) {
            int jump_player = (jump_piece == WHITE_MAN || jump_piece == WHITE_KING) ? WHITE : BLACK;
            
            if (jump_player != player) {
                Move new_move;
                if (depth == 0) {
                    new_move.from_row = row;
                    new_move.from_col = col;
                    new_move.to_row = land_row;
                    new_move.to_col = land_col;
                    new_move.num_captures = 1;
                    new_move.captured_pieces[0][0] = jump_row;
                    new_move.captured_pieces[0][1] = jump_col;
                } else {
                    new_move = moves[*count - 1];
                    new_move.to_row = land_row;
                    new_move.to_col = land_col;
                    new_move.captured_pieces[new_move.num_captures][0] = jump_row;
                    new_move.captured_pieces[new_move.num_captures][1] = jump_col;
                    new_move.num_captures++;
                }
                
                int original_jump = get_piece_at(jump_row, jump_col);
                int original_land = get_piece_at(land_row, land_col);
                int original_start = get_piece_at(row, col);
                
                set_piece_at(jump_row, jump_col, EMPTY);
                set_piece_at(land_row, land_col, piece);
                set_piece_at(row, col, EMPTY);
                
                int found_further = 0;
                find_captures(land_row, land_col, moves, count, depth + 1);
                if (*count > 0 && moves[*count - 1].num_captures > new_move.num_captures) {
                    found_further = 1;
                }
                
                set_piece_at(jump_row, jump_col, original_jump);
                set_piece_at(land_row, land_col, original_land);
                set_piece_at(row, col, original_start);
                
                if (!found_further) {
                    moves[*count] = new_move;
                    (*count)++;
                }
            }
        }
    }
}

void find_regular_moves(int row, int col, Move* moves, int* count) {
    if (*count >= MAX_LEGAL_MOVES) return;
    
    int piece = get_piece_at(row, col);
    if (piece == EMPTY) return;
    
    int is_king = (piece == WHITE_KING || piece == BLACK_KING);
    int player = (piece == WHITE_MAN || piece == WHITE_KING) ? WHITE : BLACK;
    
    int directions[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    int start_dir = 0, end_dir = 4;
    
    if (!is_king && player == WHITE) {
        end_dir = 2;
    } else if (!is_king && player == BLACK) {
        start_dir = 2;
    }
    
    for (int dir = start_dir; dir < end_dir; dir++) {
        int dr = directions[dir][0];
        int dc = directions[dir][1];
        int new_row = row + dr;
        int new_col = col + dc;
        
        if (is_valid_square(new_row, new_col) && get_piece_at(new_row, new_col) == EMPTY) {
            Move move;
            move.from_row = row;
            move.from_col = col;
            move.to_row = new_row;
            move.to_col = new_col;
            move.num_captures = 0;
            moves[*count] = move;
            (*count)++;
        }
    }
}

void generate_legal_moves(void) {
    game.num_legal_moves = 0;
    game.has_capture = 0;
    
    if (game.must_continue_capture && game.multi_capture_row != -1 && game.multi_capture_col != -1) {
        find_captures(game.multi_capture_row, game.multi_capture_col, game.legal_moves, &game.num_legal_moves, 0);
        game.has_capture = (game.num_legal_moves > 0);
        for (int i = 0; i < game.num_legal_moves; i++) {
            get_move_notation(&game.legal_moves[i]);
        }
        return;
    }
    
    Move all_moves[MAX_LEGAL_MOVES];
    int move_count = 0;
    
    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            int piece = get_piece_at(row, col);
            if (piece == EMPTY) continue;
            
            int player = (piece == WHITE_MAN || piece == WHITE_KING) ? WHITE : BLACK;
            if (player != game.current_player) continue;
            
            find_captures(row, col, all_moves, &move_count, 0);
        }
    }
    
    if (move_count > 0) {
        game.has_capture = 1;
        for (int i = 0; i < move_count && i < MAX_LEGAL_MOVES; i++) {
            game.legal_moves[i] = all_moves[i];
        }
        game.num_legal_moves = move_count;
    } else {
        for (int row = 0; row < BOARD_SIZE; row++) {
            for (int col = 0; col < BOARD_SIZE; col++) {
                int piece = get_piece_at(row, col);
                if (piece == EMPTY) continue;
                
                int player = (piece == WHITE_MAN || piece == WHITE_KING) ? WHITE : BLACK;
                if (player != game.current_player) continue;
                
                find_regular_moves(row, col, all_moves, &move_count);
            }
        }
        
        for (int i = 0; i < move_count && i < MAX_LEGAL_MOVES; i++) {
            game.legal_moves[i] = all_moves[i];
        }
        game.num_legal_moves = move_count;
    }
    
    for (int i = 0; i < game.num_legal_moves; i++) {
        get_move_notation(&game.legal_moves[i]);
    }
}

int is_legal_move(int from_row, int from_col, int to_row, int to_col) {
    for (int i = 0; i < game.num_legal_moves; i++) {
        if (game.legal_moves[i].from_row == from_row &&
            game.legal_moves[i].from_col == from_col &&
            game.legal_moves[i].to_row == to_row &&
            game.legal_moves[i].to_col == to_col) {
            return i;
        }
    }
    return -1;
}

void make_move(Move* move) {
    int piece = get_piece_at(move->from_row, move->from_col);
    
    for (int i = 0; i < move->num_captures; i++) {
        set_piece_at(move->captured_pieces[i][0], move->captured_pieces[i][1], EMPTY);
    }
    
    set_piece_at(move->from_row, move->from_col, EMPTY);
    set_piece_at(move->to_row, move->to_col, piece);
    
    if (piece == WHITE_MAN && move->to_row == 0) {
        set_piece_at(move->to_row, move->to_col, WHITE_KING);
    } else if (piece == BLACK_MAN && move->to_row == 7) {
        set_piece_at(move->to_row, move->to_col, BLACK_KING);
    }
    
    if (move->num_captures > 0) {
        game.multi_capture_row = move->to_row;
        game.multi_capture_col = move->to_col;
        generate_legal_moves();
        
        if (game.num_legal_moves > 0 && game.has_capture) {
            game.must_continue_capture = 1;
            return;
        }
    }
    
    game.must_continue_capture = 0;
    game.multi_capture_row = -1;
    game.multi_capture_col = -1;
    game.current_player = 1 - game.current_player;
    
    if (game.move_count < MAX_MOVES) {
        game.move_history[game.move_count] = *move;
        /* Снапшот доски после хода */
        int sn = game.move_count;
        memcpy(g_snaps[sn].board, game.board, sizeof(game.board));
        g_snaps[sn].player = game.current_player;
        g_snaps[sn].valid  = 1;
        game.move_count++;
    }
    
    generate_legal_moves();
    
    if (is_game_over()) {
        game.game_over = 1;
    }
}

char get_col_char(int col) {
    return 'A' + col;
}

int get_col_from_char(char c) {
    return c - 'A';
}

void get_move_notation(Move* move) {
    char from_col = get_col_char(move->from_col);
    char to_col = get_col_char(move->to_col);
    int from_row = 8 - move->from_row;
    int to_row = 8 - move->to_row;
    
    if (move->num_captures == 0) {
        snprintf(move->notation, sizeof(move->notation), "%c%d-%c%d", 
                from_col, from_row, to_col, to_row);
    } else if (move->num_captures == 1) {
        snprintf(move->notation, sizeof(move->notation), "%c%dx%c%d", 
                from_col, from_row, to_col, to_row);
    } else {
        snprintf(move->notation, sizeof(move->notation), "%c%dx%c%d(+%d)", 
                from_col, from_row, to_col, to_row, move->num_captures - 1);
    }
}

int is_game_over(void) {
    if (game.num_legal_moves > 0) return 0;
    
    int white_count = 0, black_count = 0;
    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            int piece = get_piece_at(row, col);
            if (piece == WHITE_MAN || piece == WHITE_KING) white_count++;
            else if (piece == BLACK_MAN || piece == BLACK_KING) black_count++;
        }
    }
    
    if (white_count == 0) {
        game.winner = (game.rule_mode == CLASSIC_MODE) ? BLACK : WHITE;
        return 1;
    }
    
    if (black_count == 0) {
        game.winner = (game.rule_mode == CLASSIC_MODE) ? WHITE : BLACK;
        return 1;
    }
    
    if (game.rule_mode == CLASSIC_MODE) {
        game.winner = 1 - game.current_player;
    } else {
        game.winner = game.current_player;
    }
    
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
   AI — eval_board, minimax, computer_move
   Minimax работает на локальной копии доски — НЕ трогает game.*
   ═══════════════════════════════════════════════════════════════════════ */

/* Оценка позиции с точки зрения player */
static int lb_eval(const LBoard *lb, int player) {
    int score = 0;
    for (int r = 0; r < BOARD_SIZE; r++)
        for (int c = 0; c < BOARD_SIZE; c++) {
            int p = lb->b[r][c];
            if (p == EMPTY) continue;
            int mine = ((p==WHITE_MAN||p==WHITE_KING) && player==WHITE) ||
                       ((p==BLACK_MAN||p==BLACK_KING) && player==BLACK);
            int king  = (p==WHITE_KING||p==BLACK_KING);
            int val   = king ? 30 : 10;
            if (!king) val += (player==WHITE) ? (7-r) : r;
            int dr = r-3; if(dr<0) dr=-dr;
            int dc = c-3; if(dc<0) dc=-dc;
            val += (6 - dr - dc);
            if (c==0||c==7) val += 3;
            score += mine ? val : -val;
        }
    return score;
}

/* Применить ход к локальной доске (возвращает новую LBoard) */
static LBoard lb_apply(const LBoard *src, const Move *mv) {
    LBoard dst = *src;
    int piece = dst.b[mv->from_row][mv->from_col];
    dst.b[mv->from_row][mv->from_col] = EMPTY;
    for (int i = 0; i < mv->num_captures; i++) {
        int cr = mv->captured_pieces[i][0];
        int cc = mv->captured_pieces[i][1];
        if (cr>=0&&cr<BOARD_SIZE&&cc>=0&&cc<BOARD_SIZE)
            dst.b[cr][cc] = EMPTY;
    }
    dst.b[mv->to_row][mv->to_col] = piece;
    /* Дамка */
    if (piece==WHITE_MAN && mv->to_row==0) dst.b[mv->to_row][mv->to_col]=WHITE_KING;
    if (piece==BLACK_MAN && mv->to_row==7) dst.b[mv->to_row][mv->to_col]=BLACK_KING;
    dst.player = 1 - src->player;
    return dst;
}

/* Генерация ходов для локальной доски */
static int lb_gen(const LBoard *lb, Move *out) {
    /* Временно ставим доску в game для вызова find_captures/find_regular_moves */
    int save[BOARD_SIZE][BOARD_SIZE];
    int save_player = game.current_player;
    int save_mcc    = game.must_continue_capture;
    int save_mcr    = game.multi_capture_row;
    int save_mcc2   = game.multi_capture_col;
    memcpy(save, game.board, sizeof(save));

    memcpy(game.board, lb->b, sizeof(game.board));
    game.current_player        = lb->player;
    game.must_continue_capture = 0;
    game.multi_capture_row     = -1;
    game.multi_capture_col     = -1;

    int cnt = 0;
    /* Захваты */
    for (int r=0;r<BOARD_SIZE;r++) for (int c=0;c<BOARD_SIZE;c++) {
        int p = game.board[r][c]; if(p==EMPTY) continue;
        if (lb->player==WHITE && p!=WHITE_MAN && p!=WHITE_KING) continue;
        if (lb->player==BLACK && p!=BLACK_MAN && p!=BLACK_KING) continue;
        if (cnt < MAX_LEGAL_MOVES) find_captures(r,c,out,&cnt,0);
    }
    if (cnt == 0) {
        /* Тихие ходы */
        for (int r=0;r<BOARD_SIZE;r++) for (int c=0;c<BOARD_SIZE;c++) {
            int p = game.board[r][c]; if(p==EMPTY) continue;
            if (lb->player==WHITE && p!=WHITE_MAN && p!=WHITE_KING) continue;
            if (lb->player==BLACK && p!=BLACK_MAN && p!=BLACK_KING) continue;
            if (cnt < MAX_LEGAL_MOVES) find_regular_moves(r,c,out,&cnt);
        }
    }

    /* Восстанавливаем */
    memcpy(game.board, save, sizeof(save));
    game.current_player        = save_player;
    game.must_continue_capture = save_mcc;
    game.multi_capture_row     = save_mcr;
    game.multi_capture_col     = save_mcc2;

    return cnt;
}

/* Minimax alpha-beta на локальных досках */
static int lb_minimax(LBoard lb, int depth, int alpha, int beta, int mm_player) {
    if (depth == 0) return lb_eval(&lb, mm_player);

    Move moves[MAX_LEGAL_MOVES];
    int  nmoves = lb_gen(&lb, moves);
    if (nmoves == 0) return lb_eval(&lb, mm_player);

    int is_max = (lb.player == mm_player);
    int best   = is_max ? -32000 : 32000;

    for (int i = 0; i < nmoves; i++) {
        LBoard next = lb_apply(&lb, &moves[i]);
        int score   = lb_minimax(next, depth-1, alpha, beta, mm_player);
        if (is_max) {
            if (score > best) best = score;
            if (score > alpha) alpha = score;
        } else {
            if (score < best) best = score;
            if (score < beta)  beta = score;
        }
        if (beta <= alpha) break;
    }
    return best;
}

void computer_move(void) {
    if (game.num_legal_moves == 0 || game.game_over) return;
    int n      = game.num_legal_moves;
    int player = game.current_player;

    /* ── Beginner: случайный ── */
    if (game.ai_level == 0) {
        make_move(&game.legal_moves[rand() % n]);
        game.selected_row = -1; game.selected_col = -1;
        return;
    }

    /* ── Player / Champion: выбираем лучший ход ── */
    int best_score = -32000;
    int best_idx   = 0;
    int depth      = (game.ai_level == 1) ? 2 : 4;
    /* Player: смотрит на 2 полухода вперёд, Champion: на 4 */

    /* Начальная локальная доска */
    LBoard root;
    memcpy(root.b, game.board, sizeof(root.b));
    root.player = player;

    for (int i = 0; i < n; i++) {
        Move *mv = &game.legal_moves[i];
        /* Проверяем координаты */
        if (mv->from_row<0||mv->from_row>=BOARD_SIZE) continue;
        if (mv->from_col<0||mv->from_col>=BOARD_SIZE) continue;
        if (mv->to_row  <0||mv->to_row  >=BOARD_SIZE) continue;
        if (mv->to_col  <0||mv->to_col  >=BOARD_SIZE) continue;

        LBoard after = lb_apply(&root, mv);

        int score;
        /* Player depth=2, Champion depth=4 — оба через minimax */
        score = lb_minimax(after, depth, -32000, 32000, player);
        if (game.ai_level == 1) score += rand() % 3;  /* небольшой random для Player */

        if (score > best_score) {
            best_score = score;
            best_idx   = i;
        }
    }

    make_move(&game.legal_moves[best_idx]);
    game.selected_row = -1; game.selected_col = -1;
}


void handle_click(int x, int y) {
    int px  = g_ox + BOARD_SIZE*g_sq + 18;
    int bw  = g_btn_bw;
    int bh  = 22;
    int bx2 = px + bw + 8;

    int py = 12;
    py += 22; py += 20;                    /* title + brand */
    int y_ai = py;  py += bh + 6;         /* AI level */
    if (game.browse_mode) py += 22;
    py += 20;                              /* player */
    int y_rule  = py;  py += bh + 6;
    int y_start = py;  py += bh + 6;
    int y_save  = py;  py += bh + 4 + 16;
    int y_replay= py;  py += bh + 6;
    py += 16;
    int y_speed = py;  py += bh + 6;
    int y_rpause = -1;
    if (game.replay_state != REPLAY_STOPPED) {
        py += 16;  /* replay delay label */
        y_rpause = py; py += bh + 6;
    }
    int y_pfh   = -1;
    if (game.browse_mode) { y_pfh = py; py += bh + 6; }
    if (game.game_over)   py += 44;
    if (game.move_count > 0) py += 36;
    py += 4 + 16;

    /* AI Level */
    { int sw=(bw*2+8)/3;
      for(int li=0;li<3;li++){int lx=px+li*sw;
          if(x>=lx&&x<=lx+sw-2&&y>=y_ai&&y<=y_ai+bh){game.ai_level=li;return;}
      }
    }

    /* Rule / Play Mode */
    if (x>=px&&x<=px+bw&&y>=y_rule&&y<=y_rule+bh) {
        game.rule_mode=1-game.rule_mode;
        if(!game.game_over) generate_legal_moves();
        return;
    }
    if (x>=bx2&&x<=bx2+bw&&y>=y_rule&&y<=y_rule+bh) {
        game.play_mode=(game.play_mode+1)%3; return;
    }

    /* Start Over — сохраняем все настройки */
    if (x>=px&&x<=px+bw*2+8&&y>=y_start&&y<=y_start+bh) {
        Uint32 d  = game.computer_move_delay;
        int al    = game.ai_level;
        int rule  = game.rule_mode;
        int play  = game.play_mode;
        reset_game();
        game.computer_move_delay = d;
        game.ai_level  = al;
        game.rule_mode = rule;
        game.play_mode = play;
        game.game_started = 1;
        return;
    }

    /* Save / Load */
    if (x>=px&&x<=px+bw&&y>=y_save&&y<=y_save+bh) { save_game(); return; }
    if (x>=bx2&&x<=bx2+bw&&y>=y_save&&y<=y_save+bh) { load_game(); return; }

    /* Replay Start / Pause */
    if (x>=px&&x<=px+bw&&y>=y_replay&&y<=y_replay+bh) { replay_start(); return; }
    if (x>=bx2&&x<=bx2+bw&&y>=y_replay&&y<=y_replay+bh) {
        game.game_paused=!game.game_paused; return;
    }

    /* Speed - / + */
    if (x>=px&&x<=px+bw&&y>=y_speed&&y<=y_speed+bh) {
        if(game.computer_move_delay>500) game.computer_move_delay-=500;
        else game.computer_move_delay=500;
        return;
    }
    if (x>=bx2&&x<=bx2+bw&&y>=y_speed&&y<=y_speed+bh) {
        if(game.computer_move_delay<5000) game.computer_move_delay+=500;
        return;
    }

    /* Replay Pause/Stop */
    if (y_rpause>=0) {
        if (x>=px&&x<=px+bw&&y>=y_rpause&&y<=y_rpause+bh) {
            if (game.replay_state==REPLAY_PLAYING)
                game.replay_state=REPLAY_PAUSED;
            else if (game.replay_state==REPLAY_PAUSED) {
                game.replay_state=REPLAY_PLAYING;
                game.last_replay_time=SDL_GetTicks();
            }
            return;
        }
        if (x>=bx2&&x<=bx2+bw&&y>=y_rpause&&y<=y_rpause+bh) {
            game.replay_state=REPLAY_STOPPED;
            game.game_paused=0;
            return;
        }
    }

    /* Play from here / Replay from here */
    if (y_pfh>=0&&x>=px&&x<=px+bw&&y>=y_pfh&&y<=y_pfh+bh) { play_from_here(); return; }
    if (y_pfh>=0&&x>=bx2&&x<=bx2+bw&&y>=y_pfh&&y<=y_pfh+bh) { replay_from_here(); return; }

    /* Move list click */
    if (g_list_top>0&&x>=g_panel_px&&x<=g_panel_px+g_btn_bw*2+14&&y>=g_list_top) {
        int clicked=(y-g_list_top)/16+game.move_list_scroll;
        if(clicked>=0&&clicked<game.move_count){goto_move(clicked);return;}
    }

    /* Scrollbar */
    { int sb_x=g_panel_px+g_btn_bw*2+14, sb_w=14;
      int list_h=g_win_h-g_list_top-8, visible=list_h/16;
      if(x>=sb_x&&x<=sb_x+sb_w&&y>=g_list_top&&y<=g_list_top+list_h&&game.move_count>visible){
          int ms=game.move_count-visible; if(ms<1)ms=1;
          if(y<g_list_top+14){game.move_list_scroll--;if(game.move_list_scroll<0)game.move_list_scroll=0;return;}
          if(y>g_list_top+list_h-14){game.move_list_scroll++;if(game.move_list_scroll>ms)game.move_list_scroll=ms;return;}
          int rel=y-g_list_top-14,th=list_h-28;
          if(th>0){game.move_list_scroll=rel*ms/th;if(game.move_list_scroll<0)game.move_list_scroll=0;if(game.move_list_scroll>ms)game.move_list_scroll=ms;}
          return;
      }
    }

    if (game.game_over) return;
    if (game.browse_mode) return;
    if (!game.game_started) return;
    if (game.play_mode==FULL_AUTO) return;
    if (game.play_mode==SEMI_AUTO&&game.current_player==BLACK) return;

    if (x<g_ox||x>=g_ox+BOARD_SIZE*g_sq||y<g_oy||y>=g_oy+BOARD_SIZE*g_sq) return;

    int col=(x-g_ox)/g_sq, row=(y-g_oy)/g_sq;
    if (!is_valid_square(row,col)) return;

    if (game.selected_row==-1) {
        int piece=get_piece_at(row,col); if(piece==EMPTY) return;
        int player=(piece==WHITE_MAN||piece==WHITE_KING)?WHITE:BLACK;
        if(player!=game.current_player) return;
        for(int i=0;i<game.num_legal_moves;i++)
            if(game.legal_moves[i].from_row==row&&game.legal_moves[i].from_col==col)
                {game.selected_row=row;game.selected_col=col;return;}
    } else {
        int mi=is_legal_move(game.selected_row,game.selected_col,row,col);
        if(mi!=-1){make_move(&game.legal_moves[mi]);game.selected_row=-1;game.selected_col=-1;}
        else {
            game.selected_row=-1;game.selected_col=-1;
            int piece=get_piece_at(row,col); if(piece==EMPTY) return;
            int player=(piece==WHITE_MAN||piece==WHITE_KING)?WHITE:BLACK;
            if(player!=game.current_player) return;
            for(int i=0;i<game.num_legal_moves;i++)
                if(game.legal_moves[i].from_row==row&&game.legal_moves[i].from_col==col)
                    {game.selected_row=row;game.selected_col=col;return;}
        }
    }
}


/* ── File dialog thread (non-blocking) ── */
static void* file_dialog_thread(void *arg) {
    FileDialogCtx *ctx = (FileDialogCtx*)arg;
    char cmd[512];
    if (ctx->mode == 0)
        snprintf(cmd, sizeof(cmd),
            "zenity --file-selection --save --confirm-overwrite "
            "--title='Save Checkers Game' --filename='checkers_game.txt' "
            "--file-filter='*.txt' 2>/dev/null");
    else
        snprintf(cmd, sizeof(cmd),
            "zenity --file-selection "
            "--title='Load Checkers Game' "
            "--file-filter='*.txt' 2>/dev/null");
    FILE *fp = popen(cmd, "r");
    if (fp) {
        if (fgets(ctx->result, sizeof(ctx->result), fp)) {
            size_t l = strlen(ctx->result);
            while (l > 0 && (ctx->result[l-1] == '\n' || ctx->result[l-1] == '\r'))
                ctx->result[--l] = '\0';
            ctx->done = ctx->result[0] ? 1 : -1;
        } else { ctx->done = -1; }
        pclose(fp);
    } else { ctx->done = -1; }
    ctx->busy = 0;
    return NULL;
}

static void start_file_dialog(int mode) {
    if (g_fdlg.busy) return;
    g_fdlg.busy = 1; g_fdlg.done = 0; g_fdlg.mode = mode;
    g_fdlg.result[0] = '\0';
    pthread_t th;
    pthread_create(&th, NULL, file_dialog_thread, &g_fdlg);
    pthread_detach(th);
}

static void do_save(const char *filepath) {
    char path[1024];
    strncpy(path, filepath, sizeof(path)-1);
    if (!strrchr(path, '.'))
        strncat(path, ".txt", sizeof(path)-strlen(path)-1);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "CHECKERS_GAME\n");
    fprintf(file, "RULE_MODE %d\n",  game.rule_mode);
    fprintf(file, "PLAY_MODE %d\n",  game.play_mode);
    fprintf(file, "AI_LEVEL %d\n",   game.ai_level);
    fprintf(file, "SPEED %u\n",      game.computer_move_delay);
    fprintf(file, "MOVES %d\n",      game.move_count);
    /* Воспроизводим партию чтобы записать состояние доски после каждого хода */
    {
        int sv_board[BOARD_SIZE][BOARD_SIZE];
        int sv_player = game.current_player;
        memcpy(sv_board, game.board, sizeof(sv_board));
        /* Берём снапшоты которые уже есть в g_snaps */
        for (int i = 0; i < game.move_count; i++) {
            fprintf(file, "MOVE %s ", game.move_history[i].notation);
            /* Записываем 64 цифры состояния доски после хода */
            if (i < MAX_MOVES && g_snaps[i].valid) {
                for (int r = 0; r < BOARD_SIZE; r++)
                    for (int c = 0; c < BOARD_SIZE; c++)
                        fprintf(file, "%d", g_snaps[i].board[r][c]);
            } else {
                /* Нет снапшота — пишем нули */
                for (int k = 0; k < 64; k++) fprintf(file, "0");
            }
            fprintf(file, "\n");
        }
        memcpy(game.board, sv_board, sizeof(game.board));
        game.current_player = sv_player;
    }
    fclose(file);
    game.save_msg_timer = SDL_GetTicks();
}


void save_game(void) { start_file_dialog(0); }

/* parse_notation: notation string -> from/to coordinates */

static void do_load(const char *filepath) {
    FILE *file = fopen(filepath, "r");
    if (!file) return;
    char line[256];
    if (!fgets(line, sizeof(line), file) ||
        strncmp(line, "CHECKERS_GAME", 13) != 0) {
        fclose(file); return;
    }
    int    loaded_rule  = game.rule_mode;
    int    loaded_play  = game.play_mode;
    int    loaded_ai    = game.ai_level;
    Uint32 loaded_speed = game.computer_move_delay;
    int    loaded_count = 0;
    static Move loaded_moves[MAX_MOVES];
    memset(loaded_moves, 0, sizeof(loaded_moves));
    static BoardSnap temp_snaps[MAX_MOVES];
    memset(temp_snaps, 0, sizeof(temp_snaps));
    while (fgets(line, sizeof(line), file)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r'))
            line[--ln] = '\0';
        if      (strncmp(line,"RULE_MODE ",10)==0) sscanf(line+10,"%d",&loaded_rule);
        else if (strncmp(line,"PLAY_MODE ",10)==0) sscanf(line+10,"%d",&loaded_play);
        else if (strncmp(line,"AI_LEVEL " , 9)==0) sscanf(line+9, "%d",&loaded_ai);
        else if (strncmp(line,"SPEED "    , 6)==0) sscanf(line+6, "%u",&loaded_speed);
        else if (strncmp(line,"MOVE ",5)==0 && loaded_count < MAX_MOVES) {
            /* Читаем нотацию и 64 цифры состояния доски */
            char board_str[65] = {0};
            sscanf(line+5, "%49s %64s",
                   loaded_moves[loaded_count].notation, board_str);
            parse_notation(&loaded_moves[loaded_count]);
            /* Если есть состояние доски — сохраняем в g_snaps сразу */
            if (strlen(board_str) == 64) {
                for (int r = 0; r < BOARD_SIZE; r++)
                    for (int c = 0; c < BOARD_SIZE; c++)
                        temp_snaps[loaded_count].board[r][c] =
                            board_str[r*BOARD_SIZE+c] - '0';
                temp_snaps[loaded_count].player = (loaded_count % 2 == 0) ? BLACK : WHITE;
                temp_snaps[loaded_count].valid  = 1;
            }
            loaded_count++;
        }
    }
    fclose(file);
    /* Сброс ПЕРЕД заполнением снапшотов */
    reset_game();
    game.rule_mode           = loaded_rule;
    game.play_mode           = loaded_play;
    game.ai_level            = loaded_ai;
    game.computer_move_delay = loaded_speed;

    /* Копируем снапшоты в g_snaps после reset_game */
    memcpy(g_snaps, temp_snaps, sizeof(BoardSnap)*loaded_count);

    /* Восстанавливаем историю */
    for (int i = 0; i < loaded_count; i++)
        game.move_history[i] = loaded_moves[i];
    game.move_count      = loaded_count;
    game.move_list_scroll = 0;
    game.load_msg_timer  = SDL_GetTicks();
}

void load_game(void) { start_file_dialog(1); }

void replay_start(void) {
    /* Сохраняем историю, снапшоты и настройки перед reset */
    static Move saved_hist[MAX_MOVES];
    static BoardSnap saved_snaps[MAX_MOVES];
    int    saved_cnt  = game.move_count;
    int    saved_rule = game.rule_mode;
    int    saved_play = game.play_mode;
    Uint32 saved_del  = game.computer_move_delay;
    int    saved_ai   = game.ai_level;
    memcpy(saved_hist,  game.move_history, sizeof(Move)*saved_cnt);
    memcpy(saved_snaps, g_snaps,           sizeof(BoardSnap)*saved_cnt);

    reset_game();

    /* Восстанавливаем */
    memcpy(game.move_history, saved_hist,  sizeof(Move)*saved_cnt);
    memcpy(g_snaps,           saved_snaps, sizeof(BoardSnap)*saved_cnt);
    game.move_count          = saved_cnt;
    game.rule_mode           = saved_rule;
    game.play_mode           = saved_play;
    game.computer_move_delay = saved_del;
    game.ai_level            = saved_ai;
    game.game_started        = 1;
    game.game_paused         = 0;
    game.replay_state        = REPLAY_PLAYING;
    game.replay_index        = 0;
    game.last_replay_time    = SDL_GetTicks();
}

static void replay_apply(int idx) {
    if (idx < 0 || idx >= MAX_MOVES || !g_snaps[idx].valid) return;
    memcpy(game.board, g_snaps[idx].board, sizeof(game.board));
    game.current_player        = g_snaps[idx].player;
    game.selected_row          = -1;
    game.selected_col          = -1;
    game.must_continue_capture = 0;
    game.multi_capture_row     = -1;
    game.multi_capture_col     = -1;
    generate_legal_moves();
}

static void replay_scroll_to(int ri) {
    int approx_visible = (g_win_h - g_list_top - 8) / 16;
    if (approx_visible < 4) approx_visible = 4;
    int ideal = ri - approx_visible / 2;
    if (ideal < 0) ideal = 0;
    int max_s = game.move_count - approx_visible;
    if (max_s < 0) max_s = 0;
    if (ideal > max_s) ideal = max_s;
    game.move_list_scroll = ideal;
}

void replay_step(void) {
    if (game.replay_index >= game.move_count) {
        game.replay_state = REPLAY_STOPPED;
        return;
    }
    replay_apply(game.replay_index);
    replay_scroll_to(game.replay_index);
    game.replay_index++;
}

void render_game(void) {
    SDL_GetWindowSize(window, &g_win_w, &g_win_h);
    int pw = PANEL_MIN_WIDTH + 20;
    int aw = g_win_w - pw - 70;
    int ah = g_win_h - 80;
    g_sq = (aw/BOARD_SIZE < ah/BOARD_SIZE) ? aw/BOARD_SIZE : ah/BOARD_SIZE;
    if (g_sq < 30) g_sq = 30;
    if (g_sq > 90) g_sq = 90;
    g_ox = (g_win_w - pw - BOARD_SIZE*g_sq) / 2;
    if (g_ox < 25) g_ox = 25;
    g_oy = (g_win_h - BOARD_SIZE*g_sq - 30) / 2;
    if (g_oy < 25) g_oy = 25;
    SDL_SetRenderDrawColor(renderer, 40, 44, 52, 255);
    SDL_RenderClear(renderer);
    SDL_Rect bb = {g_ox-4, g_oy-4, BOARD_SIZE*g_sq+8, BOARD_SIZE*g_sq+8};
    SDL_SetRenderDrawColor(renderer, 60, 65, 75, 255);
    SDL_RenderFillRect(renderer, &bb);
    int ppx = g_ox + BOARD_SIZE*g_sq + 14;
    SDL_Rect pn = {ppx, 0, g_win_w - ppx, g_win_h};
    SDL_SetRenderDrawColor(renderer, 45, 80, 62, 255);
    SDL_RenderFillRect(renderer, &pn);
    render_board();
    render_pieces();
    render_ui();
    SDL_RenderPresent(renderer);
}

void render_board(void) {
    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            SDL_Rect rect = {
                BOARD_OFFSET_X + col * SQUARE_SIZE,
                BOARD_OFFSET_Y + row * SQUARE_SIZE,
                SQUARE_SIZE,
                SQUARE_SIZE
            };
            
            if ((row + col) % 2 == 0) {
                SDL_SetRenderDrawColor(renderer, 240, 217, 181, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 181, 136, 99, 255);
            }
            SDL_RenderFillRect(renderer, &rect);
            
            if (game.selected_row == row && game.selected_col == col) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 0, 128);
                SDL_RenderFillRect(renderer, &rect);
            }
            
            for (int i = 0; i < game.num_legal_moves; i++) {
                if (game.legal_moves[i].from_row == game.selected_row &&
                    game.legal_moves[i].from_col == game.selected_col &&
                    game.legal_moves[i].to_row == row &&
                    game.legal_moves[i].to_col == col) {
                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 128);
                    SDL_RenderFillRect(renderer, &rect);
                    break;
                }
            }
            
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDrawRect(renderer, &rect);
        }
    }
    
    char labels[9] = "87654321";
    for (int i = 0; i < 8; i++) {
        char label[2] = {labels[i], '\0'};
        render_text(label, BOARD_OFFSET_X - 25, BOARD_OFFSET_Y + i * SQUARE_SIZE + SQUARE_SIZE/2 - 8, (SDL_Color){240, 240, 240, 255});
    }
    
    char col_labels[9] = "ABCDEFGH";
    for (int i = 0; i < 8; i++) {
        char label[2] = {col_labels[i], '\0'};
        render_text(label, BOARD_OFFSET_X + i * SQUARE_SIZE + SQUARE_SIZE/2 - 5, BOARD_OFFSET_Y + BOARD_SIZE * SQUARE_SIZE + 10, (SDL_Color){240, 240, 240, 255});
    }
}

void render_pieces(void) {
    /* Мигающая красная подсветка фигур которые обязаны бить */
    if (game.has_capture && !game.game_over && !game.browse_mode &&
        game.game_started && game.replay_state == REPLAY_STOPPED) {
        /* Мигание: 500мс вкл / 500мс выкл */
        Uint32 t = SDL_GetTicks();
        int blink_on = (t / 500) % 2 == 0;
        if (blink_on) {
            /* Рисуем красный контур вокруг каждой фигуры которая может бить */
            for (int i = 0; i < game.num_legal_moves; i++) {
                int r = game.legal_moves[i].from_row;
                int c = game.legal_moves[i].from_col;
                /* Проверяем что это захват */
                if (game.legal_moves[i].num_captures == 0) continue;
                int x = BOARD_OFFSET_X + c * SQUARE_SIZE;
                int y = BOARD_OFFSET_Y + r * SQUARE_SIZE;
                /* Рисуем 3 вложенных красных прямоугольника для толстой линии */
                for (int d = 1; d <= 3; d++) {
                    SDL_Rect hl = {x+d, y+d, SQUARE_SIZE-2*d, SQUARE_SIZE-2*d};
                    SDL_SetRenderDrawColor(renderer, 220, 30, 30, 255);
                    SDL_RenderDrawRect(renderer, &hl);
                }
            }
        }
    }

    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int col = 0; col < BOARD_SIZE; col++) {
            int piece = get_piece_at(row, col);
            if (piece == EMPTY) continue;
            
            int center_x = BOARD_OFFSET_X + col * SQUARE_SIZE + SQUARE_SIZE / 2;
            int center_y = BOARD_OFFSET_Y + row * SQUARE_SIZE + SQUARE_SIZE / 2;
            int radius = SQUARE_SIZE / 3;
            
            if (piece == WHITE_MAN || piece == WHITE_KING) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
            }
            
            for (int y = -radius; y <= radius; y++) {
                for (int x = -radius; x <= radius; x++) {
                    if (x * x + y * y <= radius * radius) {
                        SDL_RenderDrawPoint(renderer, center_x + x, center_y + y);
                    }
                }
            }
            
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            for (int i = 0; i < 360; i++) {
                int x = (int)(center_x + radius * cos(i * M_PI / 180));
                int y = (int)(center_y + radius * sin(i * M_PI / 180));
                SDL_RenderDrawPoint(renderer, x, y);
            }
            
            if (piece == WHITE_KING || piece == BLACK_KING) {
                SDL_SetRenderDrawColor(renderer, 255, 215, 0, 255);
                int crown_radius = radius / 3;
                for (int y = -crown_radius; y <= crown_radius; y++) {
                    for (int x = -crown_radius; x <= crown_radius; x++) {
                        if (x * x + y * y <= crown_radius * crown_radius) {
                            SDL_RenderDrawPoint(renderer, center_x + x, center_y + y);
                        }
                    }
                }
            }
        }
    }
}

void render_text(const char* text, int x, int y, SDL_Color color) {
    if (!font) return;
    
    SDL_Surface* surface = TTF_RenderText_Solid(font, text, color);
    if (!surface) return;
    
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }
    
    SDL_Rect dest = {x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &dest);
    
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void render_ui(void) {
    int px  = g_ox + BOARD_SIZE*g_sq + 18;
    int bw  = (g_win_w - px - 16)/2 - 4;
    if(bw<110) bw=110;
    if(bw>200) bw=200;
    int bh=22, bx2=px+bw+8;
    g_panel_px=px; g_btn_bw=bw;

    SDL_Color white  = {240,255,245,255};
    SDL_Color yellow = {255,230,80, 255};
    SDL_Color red    = {255,100,90, 255};
    SDL_Color lime   = {130,255,160,255};
    SDL_Color gray   = {160,200,175,255};
    SDL_Color cyan   = {80, 220,255,255};
    SDL_Color orange = {255,180,60, 255};
    SDL_Color brand  = {100,160,200,255};
    SDL_Rect r;
    int py=12;

    render_text("CHECKERS", px+28, py, yellow); py+=22;
    render_text("Art2Dec SoftLab  Igor Lukyanov  MIT 2026", px+4, py, brand); py+=20;

    /* AI Level: [Beginner][Player][Champion] */
    {
        const char *lvl[]={"Beginner","Player","Champion"};
        Uint8 lr[]={60,50,150},lg[]={120,100,50},lb[]={60,140,50};
        int sw=(bw*2+8)/3;
        for(int li=0;li<3;li++){
            SDL_Rect lr2={px+li*sw,py,sw-2,bh};
            Uint8 br=lr[li],bg=lg[li],bb=lb[li];
            if(game.ai_level==li){
                br=(Uint8)(br+60<255?br+60:255);
                bg=(Uint8)(bg+60<255?bg+60:255);
                bb=(Uint8)(bb+60<255?bb+60:255);
                SDL_SetRenderDrawColor(renderer,255,230,80,255);
                SDL_Rect hl={lr2.x-1,lr2.y-1,lr2.w+2,lr2.h+2};
                SDL_RenderDrawRect(renderer,&hl);
            }
            draw_button(lr2,lvl[li],br,bg,bb);
        }
        py+=bh+6;
    }

    if(game.browse_mode){
        char bt[60]; snprintf(bt,sizeof(bt),"BROWSE move %d  [ESC=exit]",game.browse_index+1);
        render_text(bt,px,py,orange); py+=22;
    }

    if(!game.game_started&&!game.browse_mode)
        render_text("Press Start Over to begin",px,py,yellow);
    else {
        char pt[50]; snprintf(pt,sizeof(pt),"Player: %s",game.current_player==WHITE?"WHITE":"BLACK");
        SDL_Color pc=game.browse_mode?gray:(game.current_player==WHITE?(SDL_Color){240,240,240,255}:(SDL_Color){120,220,160,255});
        render_text(pt,px,py,pc);
    }
    py+=20;

    char btn_rule[32],btn_play[32];
    const char *pmodes[]={"MANUAL","SEMI-AUTO","FULL AUTO"};
    snprintf(btn_rule,sizeof(btn_rule),"Rule: %s",game.rule_mode==CLASSIC_MODE?"CLASSIC":"GIVEAWAY");
    snprintf(btn_play,sizeof(btn_play),"Mode: %s",pmodes[game.play_mode]);
    r=(SDL_Rect){px,py,bw,bh};   draw_button(r,btn_rule,50,100,70);
    r=(SDL_Rect){bx2,py,bw,bh};  draw_button(r,btn_play,50,70,100);
    py+=bh+6;

    r=(SDL_Rect){px,py,bw*2+8,bh}; draw_button(r,"Start Over",100,60,60); py+=bh+6;

    r=(SDL_Rect){px,py,bw,bh};   draw_button(r,"Save",60,100,80);
    r=(SDL_Rect){bx2,py,bw,bh};  draw_button(r,"Load",60,80,100);
    py+=bh+4;
    {
        Uint32 now=SDL_GetTicks(); SDL_Color sc={100,220,130,255},fc={100,140,110,255};
        if(game.save_msg_timer&&now-game.save_msg_timer<3000) render_text("Saved OK",px,py,sc);
        else if(game.load_msg_timer&&now-game.load_msg_timer<3000) render_text("Loaded OK",px,py,sc);
        else if(g_fdlg.busy) render_text("Selecting file...",px,py,fc);
        else render_text("Save/Load via zenity dialog",px,py,fc);
        py+=16;
    }

    r=(SDL_Rect){px,py,bw,bh};   draw_button(r,"Replay Start",70,90,70);
    { Uint8 pbr=game.game_paused?180:80,pbg=game.game_paused?80:90,pbb=game.game_paused?30:80;
      r=(SDL_Rect){bx2,py,bw,bh};
      draw_button(r,game.game_paused?">> Resume":"|| Pause",pbr,pbg,pbb);
    }
    py+=bh+6;

    { char dl[40];
      Uint32 disp_delay = game.computer_move_delay > 0 ? game.computer_move_delay : 500;
      snprintf(dl,sizeof(dl),"AI delay: %.1fs",disp_delay/1000.0);
      render_text(dl,px,py,gray); py+=16; }
    r=(SDL_Rect){px,py,bw,bh};  draw_button(r,"Speed -",60,80,90);
    r=(SDL_Rect){bx2,py,bw,bh}; draw_button(r,"Speed +",60,90,80);
    py+=bh+6;

    if(game.replay_state!=REPLAY_STOPPED){
        char rd[40]; snprintf(rd,sizeof(rd),"Replay: %.1fs",game.replay_delay);
        render_text(rd,px,py,gray); py+=16;
        r=(SDL_Rect){px,py,bw,bh};
        draw_button(r,game.replay_state==REPLAY_PLAYING?"Pause R":"Resume R",90,90,50);
        r=(SDL_Rect){bx2,py,bw,bh}; draw_button(r,"Stop",110,50,50);
        py+=bh+6;
    }

    if(game.browse_mode){
        r=(SDL_Rect){px,py,bw,bh};   draw_button(r,"Play from here [Enter]",80,110,60);
        r=(SDL_Rect){bx2,py,bw,bh};  draw_button(r,"Replay from here",60,90,110);
        py+=bh+6;
    }

    if(game.game_over){
        render_text("GAME OVER",px+20,py,red); py+=20;
        char wt[50]; snprintf(wt,sizeof(wt),"Winner: %s",game.winner==WHITE?"WHITE":"BLACK");
        render_text(wt,px,py,yellow); py+=24;
    }

    if(game.move_count>0){
        render_text("Last:",px,py,gray); py+=16;
        const char *last=game.browse_mode?game.move_history[game.browse_index].notation
                                         :game.move_history[game.move_count-1].notation;
        render_text(last,px+4,py,lime); py+=20;
    }

    SDL_SetRenderDrawColor(renderer,70,120,90,255);
    SDL_RenderDrawLine(renderer,px,py,px+bw*2+8,py); py+=4;
    render_text("Move History (click/arrows=browse, Enter=play):",px,py,gray); py+=16;
    g_list_top=py;

    int list_h=g_win_h-py-8, line_h=16, visible=list_h/line_h;
    SDL_Rect clip={px,py,bw*2+16,list_h};
    SDL_RenderSetClipRect(renderer,&clip);

    int start=game.move_list_scroll;
    int end_i=start+visible; if(end_i>game.move_count)end_i=game.move_count;

    /* Текущий ход при replay — индекс предыдущего (уже сыгранного) */
    int replay_cur = (game.replay_state != REPLAY_STOPPED)
                     ? game.replay_index - 1 : -1;

    for(int i=start;i<end_i;i++){
        int ly=py+(i-start)*line_h;

        if(game.browse_mode&&i==game.browse_index){
            /* Browse: зелёный фон */
            SDL_Rect hl={px,ly,bw*2+12,line_h};
            SDL_SetRenderDrawColor(renderer,80,130,60,180);
            SDL_RenderFillRect(renderer,&hl);
        } else if(i==replay_cur){
            /* Replay: оранжевый фон */
            SDL_Rect hl={px,ly,bw*2+12,line_h};
            SDL_SetRenderDrawColor(renderer,140,80,20,200);
            SDL_RenderFillRect(renderer,&hl);
        }

        char line[80];
        snprintf(line,sizeof(line),"%3d.%s %s",i/2+1,i%2==0?"W":"B",game.move_history[i].notation);
        SDL_Color lc;
        if(game.browse_mode&&i==game.browse_index)
            lc=cyan;                                    /* browse: голубой */
        else if(i==replay_cur)
            lc=orange;                                  /* replay: оранжевый */
        else if(i==game.move_count-1&&!game.browse_mode&&game.replay_state==REPLAY_STOPPED)
            lc=lime;                                    /* последний ход: зелёный */
        else
            lc=white;
        render_text(line,px+2,ly,lc);
    }
    SDL_RenderSetClipRect(renderer,NULL);

    /* Scrollbar 14px */
    int sb_x=px+bw*2+14,sb_w=14;
    if(game.move_count>visible){
        int ms=game.move_count-visible; if(ms<1)ms=1;
        int th=list_h*visible/game.move_count; if(th<20)th=20;
        int ty=py+(list_h-th)*game.move_list_scroll/ms;
        SDL_SetRenderDrawColor(renderer,30,55,42,255);
        SDL_Rect tr={sb_x,py,sb_w,list_h}; SDL_RenderFillRect(renderer,&tr);
        SDL_SetRenderDrawColor(renderer,100,180,130,255);
        SDL_Rect tb={sb_x+1,ty,sb_w-2,th}; SDL_RenderFillRect(renderer,&tb);
        SDL_SetRenderDrawColor(renderer,140,220,160,255);
        SDL_RenderDrawRect(renderer,&tb);
        SDL_SetRenderDrawColor(renderer,80,150,100,255);
        SDL_Rect at={sb_x,py,sb_w,14}; SDL_RenderFillRect(renderer,&at);
        SDL_Rect ab={sb_x,py+list_h-14,sb_w,14}; SDL_RenderFillRect(renderer,&ab);
        SDL_Color ac={200,255,210,255};
        render_text("^",sb_x+3,py+1,ac);
        render_text("v",sb_x+3,py+list_h-13,ac);
    }
    (void)sb_w;
}
