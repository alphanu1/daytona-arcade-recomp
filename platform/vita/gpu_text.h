#pragma once
#if defined(DAYTONA_VITA_GPU_GL) && DAYTONA_VITA_GPU_GL
#include "gpu_gl.h"
#else
#include <vita2d.h>
#endif
#include <cctype>
#include <cstdint>
#include <string_view>
namespace vita {
// One rectangle backend for the menu font: vitaGL's recorded draw list or libvita2d.
inline void fill_rect(float x, float y, float w, float h, unsigned color) {
#if defined(DAYTONA_VITA_GPU_GL) && DAYTONA_VITA_GPU_GL
    gl_fill_rect(x, y, w, h, color);
#else
    vita2d_draw_rectangle(x, y, w, h, color);
#endif
}
inline void gpu_text(std::string_view value, int x, int y, unsigned color, int scale = 2, int columns = 74, int lines = 8) {
    struct Glyph { char character; uint8_t rows[7]; };
    static constexpr Glyph font[] = {
        {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}}, {'C',{14,17,16,16,16,17,14}},
        {'D',{30,17,17,17,17,17,30}}, {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
        {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}}, {'I',{14,4,4,4,4,4,14}},
        {'J',{7,2,2,2,18,18,12}}, {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
        {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}}, {'O',{14,17,17,17,17,17,14}},
        {'P',{30,17,17,30,16,16,16}}, {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
        {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}}, {'U',{17,17,17,17,17,17,14}},
        {'V',{17,17,17,17,17,10,4}}, {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
        {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
        {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}}, {'2',{14,17,1,2,4,8,31}},
        {'3',{30,1,1,14,1,1,30}}, {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
        {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}}, {'8',{14,17,17,14,17,17,14}},
        {'9',{14,17,17,15,1,1,14}}, {'.',{0,0,0,0,0,6,6}}, {':',{0,6,6,0,6,6,0}},
        {'/',{1,2,2,4,8,8,16}}, {'-',{0,0,0,31,0,0,0}}, {'_',{0,0,0,0,0,0,31}},
        {'+',{0,4,4,31,4,4,0}}, {'(',{2,4,8,8,8,4,2}}, {')',{8,4,2,2,2,4,8}},
        {'>',{16,8,4,2,4,8,16}}, {'?',{14,17,1,2,4,0,4}}, {'!',{4,4,4,4,4,0,4}},
        {'=',{0,0,31,0,31,0,0}}, {',',{0,0,0,0,6,4,8}}, {'\'',{4,4,0,0,0,0,0}}
    };
    int column = 0, line = 0;
    for (unsigned char raw : value) {
        if (raw == '\n' || column >= columns) {
            column = 0; if (++line >= lines) break; if (raw == '\n') continue;
        }
        const char c = char(std::toupper(raw));
        for (const auto &glyph : font) if (glyph.character == c) {
            for (int row = 0; row < 7; ++row) for (int bit = 0; bit < 5; ++bit)
                if (glyph.rows[row] & (16 >> bit))
                    fill_rect(float(x + (column * 6 + bit) * scale), float(y + (line * 9 + row) * scale),
                              float(scale), float(scale), color);
            break;
        }
        ++column;
    }
}
} // namespace vita
