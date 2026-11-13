#include "tt.h"
#include <SDL2/SDL_ttf.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const SDL_Color TT_PALETTE[8] = {
  {0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {255, 255, 0, 255},
  {0, 0, 255, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}, {255, 255, 255, 255},
};

typedef struct { Uint32 ch; Uint8 fg, bg, dbl; } Cell;   // dbl: 0 normal, 1 top half, 2 bottom half
static Cell grid[TT_ROWS][TT_COLS];
static SDL_Renderer *ren;
static TTF_Font *font;
static SDL_Texture *glyphs[0x2800];   // white glyphs, coloured with SDL_SetTextureColorMod

static SDL_Texture *glyph(Uint32 ch) {
  if (ch >= 0x2800 || ch < 32) ch = '?';
  if (!glyphs[ch]) {
    SDL_Surface *s = TTF_RenderGlyph_Blended(font, (Uint16)ch, (SDL_Color){255, 255, 255, 255});
    if (!s) { if (ch == '?') return NULL; return glyphs[ch] = glyph('?'); }
    glyphs[ch] = SDL_CreateTextureFromSurface(ren, s);
    SDL_SetTextureScaleMode(glyphs[ch], SDL_ScaleModeNearest);   // crisp teletext pixels
    SDL_FreeSurface(s);
  }
  return glyphs[ch];
}

int tt_init(SDL_Renderer *r, const char *font_path) {
  ren = r;
  if (TTF_Init() != 0) return -1;
  font = TTF_OpenFont(font_path, 20);   // Bedstead is drawn on a grid; it's scaled up per cell
  if (!font) return -1;
  tt_clear();
  return 0;
}

void tt_clear(void) {
  for (int r = 0; r < TT_ROWS; r++) for (int c = 0; c < TT_COLS; c++) grid[r][c] = (Cell){ ' ', TT_WHITE, TT_BLACK, 0 };
}

static Uint32 utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  Uint32 c = *p++;
  if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
  else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
  else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
  *s = (const char *)p;
  return c;
}

int tt_print(int col, int row, int fg, int bg, int dbl, const char *s) {
  while (*s && col < TT_COLS) {
    Uint32 ch = utf8_next(&s);
    if (row >= 0 && row < TT_ROWS && col >= 0) grid[row][col] = (Cell){ ch, (Uint8)fg, (Uint8)bg, (Uint8)(dbl ? 1 : 0) };
    if (dbl && row + 1 < TT_ROWS && col >= 0) grid[row + 1][col] = (Cell){ ch, (Uint8)fg, (Uint8)bg, 2 };
    col++;
  }
  return col;
}

int tt_printf(int col, int row, int fg, int bg, int dbl, const char *fmt, ...) {
  char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  return tt_print(col, row, fg, bg, dbl, buf);
}

void tt_fill(int row, int col0, int col1, int bg) {
  if (row < 0 || row >= TT_ROWS) return;
  for (int c = col0 < 0 ? 0 : col0; c < col1 && c < TT_COLS; c++) grid[row][c].bg = (Uint8)bg;
}

void tt_render(void) {
  int W, H; SDL_GetRendererOutputSize(ren, &W, &H);
  for (int r = 0; r < TT_ROWS; r++) for (int c = 0; c < TT_COLS; c++) {
    Cell *cell = &grid[r][c];
    SDL_Rect dst = { c * W / TT_COLS, r * H / TT_ROWS, (c + 1) * W / TT_COLS - c * W / TT_COLS, (r + 1) * H / TT_ROWS - r * H / TT_ROWS };
    SDL_Color b = TT_PALETTE[cell->bg];
    SDL_SetRenderDrawColor(ren, b.r, b.g, b.b, 255); SDL_RenderFillRect(ren, &dst);
    if (cell->ch == ' ') continue;
    SDL_Texture *t = glyph(cell->ch); if (!t) continue;
    int tw, th; SDL_QueryTexture(t, NULL, NULL, &tw, &th);
    SDL_Rect src = { 0, 0, tw, th };
    if (cell->dbl == 1) src.h = th / 2;
    else if (cell->dbl == 2) { src.y = th / 2; src.h = th - th / 2; }
    SDL_Color f = TT_PALETTE[cell->fg];
    SDL_SetTextureColorMod(t, f.r, f.g, f.b);
    SDL_RenderCopy(ren, t, &src, &dst);
  }
}

void tt_draw(int x, int y, int h, const char *s, SDL_Color c) {
  int w = h * 4 / 5;   // a teletext cell's shape
  while (*s) {
    Uint32 ch = utf8_next(&s);
    SDL_Texture *t = ch == ' ' ? NULL : glyph(ch);
    if (t) {
      SDL_Rect sh = { x + h / 10, y + h / 10, w, h }, dst = { x, y, w, h };
      SDL_SetTextureColorMod(t, 0, 0, 0); SDL_SetTextureAlphaMod(t, 180); SDL_RenderCopy(ren, t, NULL, &sh);
      SDL_SetTextureAlphaMod(t, 255); SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_RenderCopy(ren, t, NULL, &dst);
    }
    x += w;
  }
}

int tt_text_width(int h, const char *s) {
  int n = 0; while (*s) { utf8_next(&s); n++; }
  return n * (h * 4 / 5);
}
