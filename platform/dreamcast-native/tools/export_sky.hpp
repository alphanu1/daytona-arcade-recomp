// Host-only panorama preparation of the course 501460 the frame shows (39c0..39d0: the sky descriptor
// 3a48[course], 2800020 + 0x20 * course). Execute the authored initializer here, never on Dreamcast. The eight
// ROM tile maps become ordinary source art.
#pragma once
#include "dreamcast_assets.h"
#include "runtime/m2_board.h"
#include "refcore/i960_ref.h"
#include "../include/render/scene/sky.h"
#include "system24.hpp"

namespace daytona::native {
inline void export_sky(const std::filesystem::path &directory, rt::M2Board &board) {
    if (std::filesystem::exists(directory)) throw std::runtime_error("sky output already exists");
    const unsigned slot=board.read_byte(0x501460);
    if(slot>3) throw std::runtime_error("unsupported course slot");
    const uint32_t course=board.read_dword(0x3a48+4*slot), descriptor=board.read_dword(course);
    if(course!=0x2800020+0x20*slot || (!slot && descriptor!=0x2074240) ||
       board.read_dword(0x22f03c)!=0x1004000 || board.read_dword(0x22f040)!=0x1006000)
        throw std::runtime_error("unsupported authored sky layout");
    if(board.read_dword(0x53e5d4)!=course || !board.read_byte(0x53e598))
        throw std::runtime_error("sky export requires the initialized source state of the frame's course");
    uint32_t sections[8];
    for(unsigned s=0;s<8;++s) {
        sections[s]=board.read_dword(descriptor+8+4*s);
        if(sections[s]!=sections[0]+s*(12+64*board.read_dword(sections[0]+4)) ||
           board.read_dword(sections[s]+4)!=board.read_dword(sections[0]+4) || board.read_dword(sections[s]+8)!=32)
            throw std::runtime_error("unsupported sky section dimensions/address");
    }
    // Tile rows of the sections (49 on course 0, 54 on course 1, 43 on course 2, 66 on course 3): 8 pixel rows each.
    const unsigned tile_rows=board.read_dword(sections[0]+4),rows=8*tile_rows;
    if((!slot && tile_rows!=49) || tile_rows<1 || rows>512) throw std::runtime_error("unsupported sky height");
    const auto run=[&](uint32_t entry,uint32_t stop) {
        rt::I960Ref cpu(&board);cpu.m_IP=entry;
        unsigned count=0;
        while(cpu.m_IP!=stop) {
            if(++count>1000000) throw std::runtime_error("sky original instruction guard");
            cpu.execute_one();
        }
    };
    // Poison the two planes first: prepared fill must not depend on retained
    // previous-stage tile RAM. The original initializer owns both completely.
    for(unsigned i=0;i<8192;++i) board.write_word(0x1004000+2*i,0xdead);
    board.write_dword(0x53e5d4,course);
    board.write_byte(0x53e598,0);
    run(0x1c510,0x1c690);
    const auto &video=board.video();
    for(unsigned i=0;i<8192;++i)
        if(video.system24_word(8192+i)==0xdead) throw std::runtime_error("sky initializer retained old tile RAM");
    const uint16_t fill_tiles[2]={board.read_word(sections[0]+12),board.read_word(sections[0]+12+2*(tile_rows-1)*32)};
    uint32_t fill[2];
    for(unsigned f=0;f<2;++f) for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
        const unsigned tile=fill_tiles[f];
        const unsigned pen=((tile>>7)&255)*16+((video.system24_character_row(tile,y)>>((x^3)*4))&15);
        const uint32_t color=video.system24_pen(pen);
        if(!x && !y) fill[f]=color;
        if(color!=fill[f] || tile&0x8000) throw std::runtime_error("sky fill is not uniform opaque background");
    }
    // Exactly the authored rows, no captured scroll/camera or HUD. Rows
    // outside this band and the layer2/3 vertical split are NOT baked in.
    tools::dreamcast::TextureImage image{2048,rows,false,std::vector<uint32_t>(2048*rows)};
    for(unsigned y=0;y<rows;++y) for(unsigned x=0;x<2048;++x) {
        const uint16_t tile=board.read_word(sections[x/256]+12+2*((y/8)*32+(x/8)%32));
        if(tile&0x8000) throw std::runtime_error("sky unexpectedly uses foreground category");
        const uint32_t row=video.system24_character_row(tile,y);
        const unsigned pen=((tile>>7)&255)*16+((row>>(((x&7)^3)*4))&15);
        image.pixels[y*2048+x]=video.system24_pen(pen);
    }
    // Independent original tile updater, every column/direction, both sweep
    // directions and fine scroll bits. Compare pixels, not only tile IDs.
    uint64_t checked=0,layout_checked=0;
    for(unsigned i=0;i<512;++i) {
        const uint16_t heading=uint16_t((i<256 ? i : 511-i)*256+(i*73&255));
        board.write_word(0x5014a8,heading);run(0x37e8,0x3808);
        run(0x1c510,0x1c690);
        const unsigned scroll=board.read_word(0x501308);
        if(scroll!=((heading>>5)&511)) throw std::runtime_error("sky horizontal scroll differs");
        for(unsigned y=0;y<rows;++y) for(unsigned x=0;x<496;++x) {
            const uint32_t expected=video.system24_pen(video.system24_pixel(2,(x-scroll)&511,y+48));
            const unsigned u=(x-(heading>>5))&2047;
            if(image.pixels[y*2048+u]!=expected) {
                std::fprintf(stderr,"sky mismatch case=%u heading=%u x=%u y=%u u=%u actual=%08x expected=%08x\n",
                    i,heading,x,y,u,image.pixels[y*2048+u],expected);
                throw std::runtime_error("panorama differs from original sky tile update");
            }
            ++checked;
        }
        const uint16_t vertical=uint16_t(i*73&1023);
        board.write_word(0x100a004,uint16_t(scroll));
        board.write_word(0x100a00c,uint16_t(0x2000|vertical));
        nm_sky_rect_t rects[NM_SKY_RECTS];
        const unsigned count=nm_sky_rects_rows(heading,vertical,rows,rects);
        if(!count) throw std::runtime_error("native sky rectangle bound exceeded");
        for(unsigned r=0;r<count;++r) {
            const auto &rect=rects[r];
            for(unsigned y=0;y<rect.height;++y) for(unsigned x=0;x<rect.width;++x) {
                const uint32_t actual=rect.page<4 ? image.pixels[(rect.v+y)*2048+512*rect.page+rect.u+x] : fill[rect.page-NM_SKY_BLUE];
                const int pen=daytona::system24_layer_pen(video,4,true,rect.x+x,rect.y+y);
                if(pen<0 || actual!=video.system24_pen(unsigned(pen)))
                    throw std::runtime_error("native sky layout differs from original vertical split/fill");
                ++layout_checked;
            }
        }
    }
    if(!std::filesystem::create_directory(directory)) throw std::runtime_error("cannot create sky output");
    tools::dreamcast::write_tga(directory/"panorama.tga",image);
    std::ofstream out;
    out.exceptions(std::ios::badbit|std::ios::failbit);out.open(directory/"sky.txt");
    out << "# course" << slot << " ROM" << std::hex << descriptor << std::dec << ", eight 32x" << tile_rows << " tile sections; local ROM-derived art\n"
           "panorama.tga RGB565 2048 " << rows << "\n"
           "# source u=(screen_x-(uint16_heading>>5))&2047; source y=layer2_y-48\n"
           "# NOT a runtime pack; native layout checked, hardware/VQ remain separate\n";
    out << "fill_blue_argb " << std::hex << fill[0] << "\nfill_ground_argb " << fill[1] << '\n';
    out.close();
    std::printf("native_sky_panorama PASS headings=512 checked_pixels=%llu dimensions=2048x%u; original1c510/1c6a0, authored band only; not runtime integration\n",
        static_cast<unsigned long long>(checked),rows);
    std::printf("native_sky_layout PASS cases=512 checked_pixels=%llu fill=%08x/%08x; poisoned initialization, native4-rectangle bound, no target tile maps\n",
        static_cast<unsigned long long>(layout_checked),fill[0],fill[1]);
}
}
