// Host-only authored scenery extraction. Reuse the reference geometrizer and
// existing source-mesh oracle; never infer static geometry from screen pixels.
#pragma once
#include "export_models.hpp"
#include <memory>

namespace daytona::native {
// 0x13f5c traverses vehicle links, NOT authored road-section pointers.
// Host evidence only: verify membership in the live player/opponent pool before
// following links; never let a corrupt pointer turn inspection into MMIO reads.
template<class Read> std::array<uint32_t, 14> vehicle_neighbors(Read read, unsigned count) {
    const auto ram = [&](uint32_t at) {
        if (at % 4 || at < 0x500000 || at > 0x5ffffc)
            throw std::runtime_error("invalid vehicle RAM address");
        return read(at);
    };
    if (count < 1 || count > 64) throw std::runtime_error("invalid vehicle count");
    const uint32_t player = ram(0x501220), others = ram(0x501224);
    // Validate bases before offset arithmetic, so wrapped pointers cannot read RAM.
    ram(player); ram(others);
    const uint32_t stride = ram(others + 8);
    if (stride < 0x2a8 || stride > 0x1000 || stride % 4)
        throw std::runtime_error("invalid vehicle stride");
    std::set<uint32_t> pool{player};
    for (unsigned i = 1; i < count; ++i) pool.insert(others + (i-1)*stride);
    if (pool.size() != count) throw std::runtime_error("overlapping vehicle pool");
    for (auto at : pool) {
        ram(at); ram(at + 0x2a4);
        const auto next = ram(at + 0x88), previous = ram(at + 0x8c);
        if (!pool.contains(next) || !pool.contains(previous) ||
            ram(next + 0x8c) != at || ram(previous + 0x88) != at)
            throw std::runtime_error("vehicle links disagree with pool or reverse link");
    }
    uint32_t at = ram(0x5012f0);
    if (!pool.contains(at)) throw std::runtime_error("view vehicle outside pool");
    for (unsigned i = 0; i < 5; ++i) at = ram(at + 0x8c);
    std::array<uint32_t, 14> result;
    for (auto &entry : result) { entry = at; at = ram(at + 0x88); }
    return result;
}

struct CourseExport {
    struct Object { uint32_t model, uv, headers, cost; };
    std::array<std::vector<Object>, 256> cells;
    std::set<std::array<uint32_t, 3>> keys;
    ModelExport sampled;
    std::unique_ptr<rt::Geo> inspection;
    rt::Geo::ObjectSource prototype{};
    unsigned slot = 0;
    uint32_t table = 0;
    bool race_scenery_lights=false;

    template<class Read> void load(Read read) {
        if (table) throw std::runtime_error("course export already initialized");
        slot = read(0x501460) & 255;
        if (slot >= 4) throw std::runtime_error("unsupported scenery table slot");
        table = read(0x501420);
        if (table != read(0x39b0 + slot*4)) throw std::runtime_error("scenery table disagrees with course selector");
        const auto rom = [&](uint32_t address) {
            if (address % 4 || address < 0x2000000 || address >= 0x4000000)
                throw std::runtime_error("out-of-ROM scenery table read");
            return read(address);
        };
        unsigned total = 0;
        for (unsigned cell = 0; cell < cells.size(); ++cell) {
            uint32_t at = rom(table + cell*4);
            for (uint32_t model; (model = rom(at)) != 0; at += 16) {
                const Object object{model, rom(at+4), rom(at+8), rom(at+12)};
                if (model < 0x800000 || model >= 0xc00000 || object.uv >= 0x800000 || object.headers >= 0x800000)
                    throw std::runtime_error("expected ROM-only scenery object");
                if (++total > 65536) throw std::runtime_error("too many scenery objects");
                cells[cell].push_back(object);
                keys.insert({model, object.uv, object.headers});
            }
        }
    }

    static void observe(void *data, const rt::Geo::geo_state &state,
                        const rt::Geo::ObjectSource &source, std::span<const uint32_t> words) {
        auto &self = *static_cast<CourseExport *>(data);
        if (!self.keys.contains({source.address, source.texture_points, source.texture_headers})) return;
        self.sampled.add(state, source, words);
        if (!self.inspection) {
            self.inspection = std::make_unique<rt::Geo>(state);
            self.prototype = source;
        }
    }

    void write(const std::filesystem::path &directory, const rt::Video &video) {
        if (!inspection || sampled.instances.empty())
            throw std::runtime_error("no scenery snapshot; choose a frame with visible scenery");
        ModelExport complete;
        complete.race_scenery_lights=race_scenery_lights;
        auto probe = std::move(inspection); // Disposable: no dangling observer if extraction throws.
        probe->record_pushes = true;
        probe->object_observer = ModelExport::observe;
        probe->object_observer_data = &complete;
        std::ostringstream ranges, materials;
        ranges << "# cell first_instance instance_count; all 256 authored cells, including empty\n";
        materials << "# cell h0 h1 h2 h3 luma; pre-cull dependencies, snapshot palette; "
                  << (race_scenery_lights ? "normal-race world-light envelope\n" : "captured lighting\n");
        std::set<std::array<uint16_t, 5>> all_materials, all_race_materials;
        unsigned objects = 0, empty_objects = 0;
        for (unsigned cell = 0; cell < cells.size(); ++cell) {
            const auto first = complete.instances.size();
            complete.material_variants.clear();
            complete.race_variants.clear();
            for (const auto &object : cells[cell]) {
                auto command = prototype;
                command.address = object.model;
                command.texture_points = object.uv;
                command.texture_headers = object.headers;
                command.count = 0; // Reference's full-strip limit, NOT the per-frame polygon budget.
                const auto before = complete.instances.size();
                probe->inspect_object(command);
                empty_objects += complete.instances.size() == before;
                ++objects;
            }
            ranges << cell << ' ' << first << ' ' << complete.instances.size()-first << '\n';
            for (const auto &key : (race_scenery_lights ? complete.race_variants : complete.material_variants)) {
                materials << cell;
                for (auto value : key) materials << ' ' << value;
                materials << '\n';
            }
            all_materials.insert(complete.material_variants.begin(), complete.material_variants.end());
            all_race_materials.insert(complete.race_variants.begin(),complete.race_variants.end());
        }
        complete.material_variants = std::move(all_materials);
        complete.race_variants = std::move(all_race_materials);
        // Same local mesh AND source-view/material context for every observed
        // cell. A different snapshot or a budget-truncated source must not be
        // silently presented as an exact reconstruction of this sample.
        // ponytail: linear instance membership for this offline 256-cell table;
        // index canonical instance keys if larger exports make it significant.
        for (const auto &[mesh, id] : sampled.meshes) {
            const auto found = complete.meshes.find(mesh);
            if (found == complete.meshes.end()) throw std::runtime_error("complete scenery differs from sampled mesh");
            for (const auto &instance : sampled.instances) {
                const auto space = instance.find(' ');
                if (std::stoul(instance.substr(0, space)) != id) continue;
                const auto expected = std::to_string(found->second) + instance.substr(space);
                if (std::find(complete.instances.begin(), complete.instances.end(), expected) == complete.instances.end())
                    throw std::runtime_error("scenery instance context differs from sampled view");
            }
        }
        complete.write(directory, video, false, race_scenery_lights
            ? "all authored scenery cells; static world normals, normal-race light, rigid views; snapshot palette; no road/entities/other scene lights"
            : "all authored cells of selected scenery table; snapshot lighting/palette; no road/entities or dynamic material coverage");
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(directory / "cells.txt"); output << ranges.str(); output.close();
        output.open(directory / "cell_materials.txt"); output << materials.str(); output.close();
        std::printf("native_course_geometry=PASS slot=%u table=%08x cells=256 objects=%u empty_objects=%u sampled_instances=%zu complete_instances=%zu\n",
            slot, table, objects, empty_objects, sampled.instances.size(), complete.instances.size());
    }
};

// Complete authored course0 intro body/wheel catalog. Captured state supplies
// only a geometry-validation context; prepare_models discards its transforms.
struct IntroCarExport {
    bool finish_only=false;
    bool result_only=false;
    bool name_only=false;
    std::map<std::array<uint32_t,3>,uint32_t> name_strip_counts;
    std::array<uint32_t,6> finish_setup{};
    std::array<uint32_t,3> finish_first{};
    std::unique_ptr<rt::Geo> finish_moving;
    std::set<std::array<uint32_t,3>> objects;
    std::array<PlayerWindowExport,2> windows;
    std::unique_ptr<rt::Geo> inspection;
    rt::Geo::ObjectSource prototype{};
    template<class Read> void load(Read read,bool race=false,bool finish=false,bool result=false,bool name=false) {
        const auto rom=[&](uint32_t at) {
            if(at%4 || at<0x2000000 || at>0x3fffffc) throw std::runtime_error("intro car table outside ROM");
            return read(at);
        };
        finish_only=finish;
        result_only=result;
        name_only=name;
        if(finish_only || result_only || name_only) {
            //17d2c..17d68 copies this authored record to502030, selected by
            //44bc.17884..178c8 emits focus/light, not a car's moving context.
            for(unsigned i=0;i<finish_setup.size();++i) finish_setup[i]=read(((result_only || name_only) ? 0x235e90 : 0x235dd0)+4*i);
            if(finish_setup!=std::array<uint32_t,6>{0,0x438c0000,0x438c0000,0,0,0x3f800000})
                throw std::runtime_error("presentation authored focus/light record changed");
            std::vector<uint32_t> lists={0x288b44cu,0x288b460u,0x288b474u,0x288b488u};
            if(result_only) {
                lists.clear();
                for(unsigned i=0;i<8;++i) lists.push_back(read(0x23267c+4*i));
            }
            if(name_only) {
                lists.clear();
                //226c24: 29 unaligned32-byte records, descriptor pointer at+4.
                //226af8 selects5021e0, initialized from235e90 by17eec.
                for(unsigned i=0;i<29;++i) {
                    const uint32_t at=0x23b8f2+32*i+4;
                    lists.push_back((read(at&~3u)>>16)|(read((at&~3u)+4)<<16));
                }
                lists.push_back(0x288ac90); //22750c: independently rotating selection marker.
            }
            for(uint32_t list:lists) {
                const uint32_t model=rom(list),uv=rom(list+4),headers=rom(list+8);
                if(model<0x800000 || model>=0xc00000 || uv>=0x800000 || headers>=0x800000 ||
                   (!name_only && rom(list+12)!=1) || !rom(list+12) || rom(list+12)>128 || rom(list+16))
                    throw std::runtime_error("presentation descriptor changed");
                objects.insert({model,uv,headers});
                if(name_only) name_strip_counts[{model,uv,headers}]=rom(list+12);
                if(list==0x288b460) finish_first={model,uv,headers};
            }
            if(objects.size()!=(name_only ? 30u : result_only ? 8u : 4u)) throw std::runtime_error("presentation parts are not distinct");
            return;
        }
        std::set<uint32_t> lists,stages;
        uint32_t stage=rom(0x2865a60);
        while(!stages.contains(stage)) {
            if(stages.size()>=64) throw std::runtime_error("intro stage bound");
            stages.insert(stage);
            const uint32_t events=rom(stage),cars=rom(stage+4);
            if(!cars || cars>44) throw std::runtime_error("intro car count invalid");
            for(unsigned id=0;id<cars;++id) {
                const uint32_t updater=rom(stage+24+16*id);
                if(updater==0xd05c) lists.insert(0x288d65c);
                else if(updater==0xcf74 || updater==0xcfc0)
                    for(unsigned choice=0;choice<8;++choice) lists.insert(rom(0x286fac0+48*id+4*choice));
                else throw std::runtime_error("unsupported intro car initializer");
            }
            bool transition=false;
            for(unsigned event=0;event<256;++event) {
                if(rom(events+16*event+8)&0x8000) { stage=rom(events+16*event+12);transition=true;break; }
            }
            if(!transition) throw std::runtime_error("intro car stage lacks bounded transition");
        }
        if(race) for(unsigned id=0;id<40;++id)
            for(unsigned choice=0;choice<8;++choice) lists.insert(rom(0x286fac0+48*id+4*choice));
        for(unsigned i=0;i<4;++i) {
            lists.insert(rom(0x286fab0+4*i));
            lists.insert((read(0x32094+4*i)>>16)|(read(0x32098+4*i)<<16));
        }
        for(unsigned i=0;i<16;++i) lists.insert(read(0x832c+4*i));
        for(unsigned i=0;i<8;++i) lists.insert(read(0x234d58+4*i));
        lists.insert(0x288b7a8); // Shared four-quad body shadow, all detail>=2.
        lists.insert(0x288f600);lists.insert(0x288f5ec); // Detail1/2 crash attachments.
        // The player's damaged bodies of the automatic car: crash_model[0][d] = word 235b5c+8d (16bd4/15cdc
        // write them to +14; 7358 draws +14). Exported for tools/prepare_damage_bodies.py, which turns them into
        // position/normal variants of the clean body 95ff9d (car-damage.dnb); not catalogue entries of car.dnm.
        // The manual set (235b60+8d) has the same polygon data with the manual body's texture headers: the port draws
        // these variants in the manual body's face order (render/scene/practice.c), not exported.
        if(race) for(unsigned d=0;d<16;++d) lists.insert(read(0x235b5c+8*d));
        // The car effects (include/car_effects.h, spec-effects.md section 2): the entries of the dust 231f06, smoke
        // 231f86, spark 232006 and spin smoke 232206 tables the rules reach (82 lists, one model each; the spin
        // table's 27 entries name 4 lists). Word tables at unaligned addresses, read as the i960 does. Appended to
        // the car pack by tools/append_player_parts.py --effects; not catalogue entries of prepare_models.py
        // --race-cars. Smoke and spin smoke (header e409: screen checker) are the translucent class of DNT6
        // (prepare_palette_materials.py).
        if(race) {
            const auto word=[&](uint32_t at) { return at&3u ? (read(at&~3u)>>16)|(read((at&~3u)+4)<<16) : read(at); };
            static const struct { uint32_t table; unsigned first,last; } effects[]={{0x231f06,1,31},{0x231f86,1,31},
                {0x232006,9,24},{0x232206,0,26}};
            for(const auto &effect:effects)
                for(unsigned i=effect.first;i<=effect.last;++i) lists.insert(word(effect.table+4*i));
        }
        // The hoods of the hood view VR2 (7328..7350: list 231eb0[v][i], v = 501464, i by the front corners' damage;
        // include/player_parts.h nm_player_hood_models, nm_player_manual_hood_models), one model each. The clean
        // ones are appended to the car pack by tools/append_player_parts.py --hoods (not catalogue entries); the
        // damaged ones become position/normal variants of the clean one (tools/prepare_damage_bodies.py --hoods).
        if(race) for(unsigned i=0;i<8;++i) lists.insert(read(0x231eb0+4*i));
        // The pit crew (include/pit_crew.h, spec-pitstop.md section 3): every list of the model sets that the six
        // crew records 239848[0] and their scripts (action 5) name (25 lists, one model each), and the tool 288ddc8
        // of 1b33c (its wheel 288b730 is the player's), and the manual twins 64 bytes further that 1ad3c draws with
        // the manual transmission (501464 = 1: the crew's colours of the manual car; 12 more models, the other 11
        // twins are automatic lists). Appended to the car pack by tools/append_player_parts.py --crew; not catalogue
        // entries of prepare_models.py --race-cars.
        if(race) {
            const uint32_t base=read(0x239848);
            std::set<uint32_t> sets;
            for(unsigned crew=0;crew<6;++crew) {
                sets.insert(read(base+32*crew+4));
                for(uint32_t entry=read(base+32*crew+8),n=0;;entry+=32,++n) {
                    if(n>=64) throw std::runtime_error("pit crew script bound");
                    if(read(entry+4)==5) sets.insert(read(entry+8));
                    if(read(entry)==999999) break;
                }
            }
            for(uint32_t set:sets) for(unsigned i=0;i<16;++i) { lists.insert(read(set+4*i)); lists.insert(read(set+64+4*i)); }
            lists.insert(0x288ddc8);
        }
        for(uint32_t list:lists) {
            bool end=false;
            for(unsigned i=0;i<128;++i) {
                const uint32_t at=list+16*i,model=rom(at);
                if(!model) {end=true;break;}
                const uint32_t uv=rom(at+4),headers=rom(at+8);
                if(model<0x800000 || model>=0xc00000 || uv>=0x800000 || headers>=0x800000)
                    throw std::runtime_error("intro car object is not ROM-backed");
                objects.insert({model,uv,headers});
            }
            if(!end) throw std::runtime_error("intro car model-list bound");
        }
        if(objects.size()!=(race ? 332u : 108u)) throw std::runtime_error("car body/wheel/special/shadow/crash/effect/crew/hood inventory changed");
        for(unsigned i=0;i<windows.size();++i) {
            windows[i].load(read,i ? 0x241 : 0x1c8);
            objects.insert({windows[i].address,windows[i].points,windows[i].header});
        }
    }
    static void observe(void *data,const rt::Geo::geo_state &state,
            const rt::Geo::ObjectSource &source,std::span<const uint32_t>) {
        auto &self=*static_cast<IntroCarExport *>(data);
        if(!self.inspection && (self.objects.contains({source.address,source.texture_points,source.texture_headers}) ||
                ((self.finish_only || self.result_only || self.name_only) && source.address>=0x800000 && source.address<0xc00000))) {
            if(self.finish_only || self.result_only || self.name_only) {
                auto finish=std::make_unique<rt::Geo::geo_state>(state);
                finish->focus.x=std::bit_cast<float>(self.finish_setup[1]);
                finish->focus.y=std::bit_cast<float>(self.finish_setup[2]);
                finish->light.x=std::bit_cast<float>(self.finish_setup[3]);
                finish->light.y=std::bit_cast<float>(self.finish_setup[4]);
                finish->light.p[0]=std::bit_cast<float>(self.finish_setup[5]);
                // Authored settled transform: no rotation, scale2. Translation
                // is a validation pose only and prepare_models discards it.
                // Keep geo mode:502030[0] selects the object-command port
                // at1798c/17990, NOT the geometrizer mode command.
                std::fill(std::begin(finish->matrix),std::end(finish->matrix),0.0f);
                finish->matrix[0]=finish->matrix[4]=finish->matrix[8]=2.0f;
                finish->matrix[9]=std::bit_cast<float>(UINT32_C(0xbe99999a));
                finish->matrix[11]=0.5f;
                if(self.result_only) {
                    // b84c scale1.2; explicit validation pose, NOT animation or
                    // proof of the live camera/rotations/palette ownership.
                    finish->matrix[0]=finish->matrix[4]=finish->matrix[8]=1.2f;
                    finish->matrix[9]=0;finish->matrix[10]=-0.01f;
                }
                self.inspection=std::make_unique<rt::Geo>(*finish);
                //4690..46f4 omits scale2 for the FIRST moving part. Validate
                // its second material with the real parser, not guessed luma.
                finish->matrix[0]=finish->matrix[4]=finish->matrix[8]=1.0f;
                if(self.finish_only) self.finish_moving=std::make_unique<rt::Geo>(*finish);
            } else self.inspection=std::make_unique<rt::Geo>(state);
            self.prototype=source;
            // Original1798c/17990 with502030[0]==0 emits this exact opcode:
            // center0, reverse0. Never inherit the sampled car's eye selector.
            if(self.finish_only || self.result_only || self.name_only) self.prototype.opcode=0x00800000;
        }
    }
    void write(const std::filesystem::path &directory,const rt::Video &video) {
        if(!inspection) throw std::runtime_error("no visible intro car validation context");
        ModelExport complete;
        auto probe=std::move(inspection);
        struct Context { ModelExport &output; const std::array<PlayerWindowExport,2> &windows; } context{complete,windows};
        probe->record_pushes=true;probe->object_observer_data=&context;
        probe->object_observer=[](void *data,const rt::Geo::geo_state &state,
                const rt::Geo::ObjectSource &source,std::span<const uint32_t> words) {
            auto &context=*static_cast<Context *>(data);
            for(const auto &window:context.windows) if(source.address==window.address) {
                window.add(context.output,state,source,words);return;
            }
            context.output.add(state,source,words);
        };
        for(const auto &object:objects) {
            auto command=prototype;command.address=object[0];command.texture_points=object[1];command.texture_headers=object[2];
            command.count=name_only ? name_strip_counts.at(object) : 0;
            const auto before=complete.instances.size();
            probe->inspect_object(command);
            if(complete.instances.size()!=before+1) throw std::runtime_error("intro catalog object did not export exactly once");
        }
        if(finish_only && (complete.meshes.size()!=4 || complete.unique_faces!=4))
            throw std::runtime_error("finish catalog must contain four single-face parts");
        if(result_only && (complete.meshes.size()!=8 || complete.unique_faces!=8))
            throw std::runtime_error("result catalog must contain eight single-face parts");
        if(finish_only) {
            for(const auto &key:complete.material_variants)
                if(key[4]!=255) throw std::runtime_error("finish settled lighting differs");
            ModelExport moving;
            finish_moving->record_pushes=true;
            finish_moving->object_observer_data=&moving;
            finish_moving->object_observer=&ModelExport::observe;
            auto command=prototype;command.address=finish_first[0];
            command.texture_points=finish_first[1];command.texture_headers=finish_first[2];command.count=0;
            finish_moving->inspect_object(command);
            if(moving.material_variants.size()!=1 || (*moving.material_variants.begin())[4]!=238)
                throw std::runtime_error("finish first moving lighting differs");
            complete.material_variants.insert(moving.material_variants.begin(),moving.material_variants.end());
            complete.material_uv_bounds.insert(moving.material_uv_bounds.begin(),moving.material_uv_bounds.end());
            finish_moving.reset();
            if(complete.material_variants.size()!=5) throw std::runtime_error("finish requires five light bindings");
            std::puts("native_finish_lighting PASS settled4x255 first-moving238; five offline material bindings");
        }
        complete.write(directory,video,false,name_only ?
            "29 authored name glyphs and selection marker with original descriptor strip counts; original5021e0 focus/light and center0 opcode; validation transform only; borrowed viewport/palette/geo mode, live name scene unverified" : result_only ?
            "eight authored result parts; original5021e0 focus/light; center0 opcode and scale1.2 validation pose; borrowed viewport/palette/geo mode, live transforms/material ownership unverified" : finish_only ?
            "four authored finish parts; original502030 focus/light and center0/reverse0 opcode; settled scale2 and first-moving scale1 light variants; borrowed dynamic viewport/palette/geo mode; strip validation transforms; final material ownership unverified" :
            "selected authored course0 car body/wheel/window/d144/shadow/low-detail-crash catalog including16 wheel and8 special phases; windows use verified ROM geometry/base UV/full axes; strip captured transforms; animation separate");
        // Offline light palettes share the existing M2T texel indices. Do not
        // emit another full image for every luma or import board colour RAM.
        std::set<std::array<uint16_t,4>> headers;
        for(const auto &key:complete.material_variants)
            headers.insert({key[0],key[1],key[2],key[3]});
        std::ofstream lights;
        lights.exceptions(std::ios::badbit|std::ios::failbit);
        lights.open(directory/"material_lights.txt");
        lights << "# Offline source colours: h0 h1 h2 h3 luma then16 textured ARGB or1 solid ARGB.\n"
               << "# All256 lumas at exported palette/translation state; not a runtime pack.\n";
        for(const auto &header:headers) {
            rt::GeoPoly poly{};
            std::copy(header.begin(),header.end(),poly.texheader);
            for(unsigned luma=0;luma<256;++luma) {
                poly.luma=uint8_t(luma);
                for(auto value:header) lights << value << ' ';
                lights << luma;
                if(header[0]&0x4000)
                    for(auto color:rt::material_colors(poly,video.gpu_mem())) lights << ' ' << color;
                else lights << ' ' << rt::material_solid_color(poly,video.gpu_mem());
                lights << '\n';
            }
        }
        lights.close();
        std::printf("native_%s_catalog PASS objects=%zu instances=%zu (animation excluded)\n",
            name_only ? "name" : result_only ? "result" : finish_only ? "finish" : "intro_car",objects.size(),complete.instances.size());
    }
};
}
