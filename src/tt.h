// Teletext screen: a 40x25 grid of characters in the 8 teletext colours, drawn with the Bedstead font.
#pragma once
#include <SDL2/SDL.h>

enum { TT_BLACK, TT_RED, TT_GREEN, TT_YELLOW, TT_BLUE, TT_MAGENTA, TT_CYAN, TT_WHITE };
#define TT_COLS 40
#define TT_ROWS 25

int tt_init(SDL_Renderer *r, const char *font_path);
void tt_clear(void);
// Writes UTF-8 text at (col,row); dbl = double height (uses this row and the next). Returns the column after it.
int tt_print(int col, int row, int fg, int bg, int dbl, const char *utf8);
int tt_printf(int col, int row, int fg, int bg, int dbl, const char *fmt, ...);
void tt_fill(int row, int col0, int col1, int bg);   // background colour for cells col0..col1-1
void tt_render(void);
// Free text anywhere on screen (banners over video): top-left (x,y), character cell height h pixels.
void tt_draw(int x, int y, int h, const char *utf8, SDL_Color c);
int tt_text_width(int h, const char *utf8);   // pixels tt_draw would use
extern const SDL_Color TT_PALETTE[8];
