// ROM-free differential renderer tests and synthetic host benchmark.
// The reference translation units are built without M2_VITA_RENDER_OPT.
#include "runtime/video.h"
#include "reference/video.h"
#include "../platform/vita/system24_upload.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {
std::mt19937 rng(0x9030abcd);
uint32_t random32() { return rng(); }
int pick(int max) { return int(random32() % unsigned(max)); }
void require(bool ok, const char *what) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); } }
uint64_t clock_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Memory {
    std::vector<uint8_t> tile, chars, pal, xlat, luma;
    std::vector<uint32_t> tex0, tex1;
    Memory() : tile(0x10000), chars(0x80000), pal(0x4000), xlat(0xc000), luma(0x20000),
        tex0(0x80000), tex1(0x80000) {
        for (auto *bytes : {&chars, &pal, &xlat, &luma}) for (auto &b : *bytes) b = uint8_t(random32());
        for (auto *sheet : {&tex0, &tex1}) for (auto &b : *sheet) b = random32();
        for (unsigned i=0;i<0x4000;++i) word(i,uint16_t(random32()));
    }
    void word(unsigned index, uint16_t value) { tile[index*2]=uint8_t(value);tile[index*2+1]=uint8_t(value>>8); }
    uint16_t word(unsigned index) const { return uint16_t(tile[index*2] | tile[index*2+1]<<8); }
    rt::VideoMem view() const { return {pal.data(),xlat.data(),luma.data(),tex0.data(),tex1.data()}; }
    reference::VideoMem refview() const { return {pal.data(),xlat.data(),luma.data(),tex0.data(),tex1.data()}; }
};
reference::GeoPoly convert(const rt::GeoPoly &p) {
    reference::GeoPoly q;
    q.z=p.z; q.luma=p.luma; q.texlod=p.texlod; q.window=p.window; q.reverse=p.reverse;q.num_vertices=p.num_vertices;
    std::copy_n(p.texheader,4,q.texheader);std::copy_n(p.viewport,4,q.viewport);std::copy_n(p.center,2,q.center);
    for(int i=0;i<8;++i) {q.v[i].x=p.v[i].x;q.v[i].y=p.v[i].y;std::copy_n(p.v[i].p,3,q.v[i].p);}
    return q;
}
std::vector<reference::GeoPoly> convert(const std::vector<rt::GeoPoly> &polys) {
    std::vector<reference::GeoPoly> q;for(auto &p:polys)q.push_back(convert(p));return q;
}
rt::GeoPoly polygon(int vertices, int renderer, int lod, bool checker, bool micro) {
    rt::GeoPoly p;
    p.num_vertices=uint8_t(vertices);p.window=uint8_t(pick(4));p.z=uint16_t(pick(16));p.luma=uint8_t(random32());p.texlod=lod;
    p.viewport[0]=0;p.viewport[1]=0;p.viewport[2]=495;p.viewport[3]=384;
    p.texheader[0]=uint16_t(pick(8) | (pick(8)<<3) | (pick(4)<<6) | (pick(4)<<8) |
        (pick(4)<<10) | (micro?0x1000:0) | (renderer<<13) | (checker?0x8000:0));
    p.texheader[1]=uint16_t(random32());p.texheader[2]=uint16_t(random32());p.texheader[3]=uint16_t(random32());
    const float cx=float(pick(540)-22),cy=float(pick(440)-28),rx=float(pick(170)+1),ry=float(pick(150)+1);
    for(int i=0;i<vertices;++i) {
        const double angle=6.2831853071795864769*double(i)/double(vertices);
        const float z=float(pick(160)+10)/10.f;
        const float x=cx+rx*float(std::cos(angle)),y=cy+ry*float(std::sin(angle));
        p.v[i].x=x*z;p.v[i].y=(384.f-y)*z;p.v[i].p[0]=z;
        p.v[i].p[1]=float(pick(8200)-64)*8.f;p.v[i].p[2]=float(pick(8200)-64)*8.f;
    }
    return p;
}
void compare(const uint32_t *a,const uint32_t *b,size_t n,int frame,const char *kind) {
    for(size_t i=0;i<n;++i)if(a[i]!=b[i]) {
        std::fprintf(stderr,"FAIL: %s case %d pixel %zu actual=%08x expected=%08x\n",kind,frame,i,a[i],b[i]);std::exit(1);
    }
}
#include "vita_system24_upload.inc"

void cache_tests() {
    Memory mem;
    std::fill(mem.tile.begin(),mem.tile.end(),0);
    auto fast=std::make_unique<rt::Video>(mem.tile.data(),mem.chars.data());
    auto slow=std::make_unique<reference::Video>(mem.tile.data(),mem.chars.data());
    auto step=[&](int n){fast->frame_start();slow->frame_start();fast->screen_update({},0,mem.view());slow->screen_update({},0,mem.refview());
        compare(fast->screen().data(),slow->screen().data(),fast->screen().size(),n,"cache");};
    fast->set_profile_clock(clock_ns);
    for(unsigned i=0;i<0x2000;++i){fast->palette_w(i,mem.pal.data(),mem.xlat.data());slow->palette_w(i,mem.pal.data(),mem.xlat.data());}
    step(0);require(fast->last_profile().tiles_rebuilt==16384,"initial tile rebuild");
    step(1);require(fast->last_profile().tiles_rebuilt==0 && !fast->last_profile().layers_rebuilt,"unchanged frame reuses layers");
    mem.word(7,1);step(2);require(fast->last_profile().tiles_rebuilt==1,"one changed tile entry");
    mem.chars[32+3]^=0xff;step(3);require(fast->last_profile().tiles_rebuilt==1,"one referenced changed glyph");
    mem.chars[0x7ffff]^=0xff;step(4);require(fast->last_profile().tiles_rebuilt==0 && !fast->last_profile().layers_rebuilt,"unused glyph does not redraw layers");
    mem.word(0x5000,127);step(5);require(!fast->last_profile().tiles_rebuilt && fast->last_profile().layers_rebuilt,"scroll change invalidates composition");
    mem.word(0x6000,0xaaa5);step(6);require(!fast->last_profile().tiles_rebuilt && fast->last_profile().layers_rebuilt,"window change invalidates composition");
    mem.word(0x1000+5,0x3fff);step(7);require(fast->last_profile().tiles_rebuilt==1,"previously unused glyph remains current");
    for (int n=8;n<40;++n) {
        unsigned offset=random32()%0x2000;mem.pal[offset*2]^=uint8_t(random32());
        fast->palette_w(offset,mem.pal.data(),mem.xlat.data());slow->palette_w(offset,mem.pal.data(),mem.xlat.data());step(n);
    }
    std::puts("PASS: tile/character/scroll/window/palette cache invalidation, including unused glyph changes");
}
void video_tests(int cases) {
    Memory mem;auto fast=std::make_unique<rt::Video>(mem.tile.data(),mem.chars.data());
    auto slow=std::make_unique<reference::Video>(mem.tile.data(),mem.chars.data());
    for(unsigned i=0;i<0x2000;++i){fast->palette_w(i,mem.pal.data(),mem.xlat.data());slow->palette_w(i,mem.pal.data(),mem.xlat.data());}
    for(int frame=0;frame<cases;++frame) {
        for(int j=0;j<17;++j)mem.word(unsigned(pick(0x4000)),uint16_t(random32()));
        for(int j=0;j<5;++j)mem.chars[size_t(pick(0x80000))]^=uint8_t(random32());
        const uint16_t mode=uint16_t((frame%4)<<13);
        mem.word(0x5004,uint16_t(pick(512))|mode);mem.word(0x5005,uint16_t(pick(512)));
        mem.word(0x5006,mode);
        if(frame%17==0)mem.word(0x5004,0x8000);
        if(frame%19==0)mem.word(0x5005,0x8000);
        mem.word(0x5000,uint16_t(pick(512)) | ((frame&4)?0x8000:0));
        mem.word(0x5001,uint16_t(pick(512)) | ((frame&8)?0x8000:0));
        for(unsigned j=0x4000;j<0x4400;++j)mem.word(j,uint16_t(pick(1024)));
        for(unsigned j=0x6000;j<0x7000;++j)mem.word(j,uint16_t(random32()));
        if(frame%3==0){mem.xlat[size_t(pick(0xc000))]^=0x67;fast->colorxlat_w(0x40);slow->colorxlat_w(0x40);}
        for(int j=0;j<4;++j){unsigned i=unsigned(pick(0x2000));mem.pal[i*2]^=0x99;fast->palette_w(i,mem.pal.data(),mem.xlat.data());slow->palette_w(i,mem.pal.data(),mem.xlat.data());}
        const uint16_t x=uint16_t(-84+pick(7)),y=uint16_t(-130+pick(7));fast->xhout_w(x);slow->xhout_w(x);fast->xvout_w(y);slow->xvout_w(y);
        std::vector<rt::GeoPoly> p;
        for(int j=0;j<12;++j)p.push_back(polygon(3+pick(6),pick(4),pick(1600)-800,bool(pick(2)),bool(pick(2))));
        const auto q=convert(p);
        if(frame%3!=1){fast->frame_start();slow->frame_start();}
        fast->screen_update(p,3,mem.view());slow->screen_update(q,3,mem.refview());
        compare(fast->screen().data(),slow->screen().data(),fast->screen().size(),frame,"video");
        compare(fast->raster().pixels(),slow->raster().pixels(),512*512,frame,"cached raster");
    }
    std::printf("PASS: %d randomized complete screen comparisons (all scroll modes, line scroll, masks, frame reuse)\n",cases);
}
void raster_tests(int cases) {
    Memory mem;rt::Raster fast;reference::Raster slow;
    for(int frame=0;frame<cases;++frame) {
        std::vector<rt::GeoPoly> p;
        for(int j=0;j<20;++j)p.push_back(polygon(3+pick(6),frame%4,pick(2400)-1200,bool(frame&4),bool(frame&8)));
        // Identical overlapping polygons exercise fully covered spans, equal-z
        // ordering, checkerboards and cache reuse. RAM mutations catch stale LUTs.
        if(frame%7==0)for(int j=0;j<30;++j)p.push_back(p[0]);
        const auto q=convert(p);
        for(int j=0;j<5;++j){mem.pal[size_t(pick(0x4000))]^=0x37;mem.luma[size_t(pick(0x20000))]^=0xb5;mem.xlat[size_t(pick(0xc000))]^=0x63;}
        mem.tex0[size_t(pick(0x80000))]^=random32();mem.tex1[size_t(pick(0x80000))]^=random32();
        const int w=frame%4, minx=pick(30),maxx=495-pick(30),miny=pick(20),maxy=383-pick(20);
        fast.render(p,w,mem.view(),0,0,minx,maxx,miny,maxy);slow.render(q,w,mem.refview(),0,0,minx,maxx,miny,maxy);
        compare(fast.pixels(),slow.pixels(),512*512,frame,"raster");
    }
    std::printf("PASS: %d randomized raster frames (3-8 vertices, 4 shaders, mips/microtextures, wrap/mirror, clipping, RAM changes)\n",cases);
}
template<class F> double measure(F f,int repeats) {
    const auto begin=clock_ns();for(int i=0;i<repeats;++i)f();return double(clock_ns()-begin)/1e6/double(repeats);
}
void benchmark() {
    Memory mem;
    auto fast=std::make_unique<rt::Video>(mem.tile.data(),mem.chars.data());
    auto slow=std::make_unique<reference::Video>(mem.tile.data(),mem.chars.data());
    fast->screen_update({},0,mem.view());slow->screen_update({},0,mem.refview());
    for(int rep=0;rep<3;++rep) {
        auto a=measure([&]{slow->screen_update({},0,mem.refview());},120);
        auto b=measure([&]{fast->screen_update({},0,mem.view());},120);
        std::printf("BENCH static 2D %d: reference %.4f ms, optimized %.4f ms, %.2fx\n",rep,a,b,a/b);
    }
    rt::Raster fr;reference::Raster sr;std::vector<rt::GeoPoly> p;
    auto surf=polygon(4,2,50,false,false);surf.window=0;surf.luma=240;surf.texheader[0]=uint16_t(0x4000|6|6<<3|0xc0);
    for(int i=0;i<4;++i){surf.v[i].p[0]=8;surf.v[i].x=(i==1||i==2)?496*8.f:0;surf.v[i].y=(i<2)?384*8.f:0;surf.v[i].p[1]=(i==1||i==2)?8192:0;surf.v[i].p[2]=(i<2)?0:8192;}
    p.push_back(surf);auto q=convert(p);
    for(int rep=0;rep<3;++rep) {
        auto a=measure([&]{sr.render(q,0,mem.refview(),0,0,0,495,0,383);},24);
        auto b=measure([&]{fr.render(p,0,mem.view(),0,0,0,495,0,383);},24);
        compare(fr.pixels(),sr.pixels(),512*512,rep,"bench");
        std::printf("BENCH full-screen textured quad %d: reference %.4f ms, optimized %.4f ms, %.2fx\n",rep,a,b,a/b);
    }
    for(int i=0;i<2000;++i)p.push_back(polygon(3+(i%6),i%4,(i%1000)-500,i%2,i%3==0));
    q=convert(p);
    auto a=measure([&]{sr.render(q,3,mem.refview(),0,0,0,495,0,383);},12);
    auto b=measure([&]{fr.render(p,3,mem.view(),0,0,0,495,0,383);},12);
    compare(fr.pixels(),sr.pixels(),512*512,0,"bench crowded");
    std::printf("BENCH 2001 overlapping polygons: reference %.4f ms, optimized %.4f ms, %.2fx\n",a,b,a/b);
    p.clear();
    for(int i=0;i<5000;++i) {
        auto small=surf;small.z=uint16_t(i%512);small.luma=uint8_t(i);small.texheader[3]=uint16_t(random32());
        float x=float(pick(490)), y=float(pick(378));
        for(int j=0;j<4;++j) { small.v[j].x=(x+((j==1||j==2)?4.f:0.f))*8.f;small.v[j].y=(384.f-y-((j>=2)?4.f:0.f))*8.f; }
        p.push_back(small);
    }
    q=convert(p);
    a=measure([&]{sr.render(q,0,mem.refview(),0,0,0,495,0,383);},12);
    b=measure([&]{fr.render(p,0,mem.view(),0,0,0,495,0,383);},12);
    compare(fr.pixels(),sr.pixels(),512*512,0,"bench small");
    std::printf("BENCH 5000 small textured quads: reference %.4f ms, optimized %.4f ms, %.2fx\n",a,b,a/b);
    std::puts("Synthetic host timings only, not Vita game FPS or full-race parity.");
}
}
int main(int argc,char **argv) {
    if(argc>1 && std::string(argv[1])=="--bench"){benchmark();return 0;}
    system24_upload_tests();system24_index_tests();cache_tests();video_tests(160);raster_tests(640);
    std::puts("All renderer comparisons passed.");
}
