// Host-only extraction of the world objects the arcade draws outside the
// scenery cells (object lists of the per-course actor table 236940, drawn by
// 1786c). The caller names the list pointers; which lists an actor draws is
// decided offline (tools/prepare_world_objects.py) from the original tables.
// Geometry is decoded with a visible scenery cell's render state, so every
// model comes out in its own local coordinates under the world light.
#pragma once
#include "export_course.hpp"
#include <cstdio>

namespace daytona::native {
struct ObjectExport {
    CourseExport course;                 // only its cell keys: finds the snapshot state
    std::vector<uint32_t> lists;         // data ROM addresses of 16-byte object lists
    struct Part { uint32_t model, uv, headers, count; };
    std::vector<std::vector<Part>> parts;
    std::unique_ptr<rt::Geo> inspection;
    rt::Geo::ObjectSource prototype{};

    void read_lists(const std::filesystem::path &path) {
        std::ifstream in(path);
        for (std::string word; in >> word;) lists.push_back(uint32_t(std::stoul(word, nullptr, 16)));
        if (lists.empty()) throw std::runtime_error("object export: no list pointers");
    }
    template<class Read> void load(Read read) {
        course.load(read);
        for (uint32_t list : lists) {
            if (list % 4 || list < 0x2000000 || list >= 0x4000000)
                throw std::runtime_error("object list outside the data ROM");
            std::vector<Part> entries;
            for (uint32_t at = list; read(at) != 0; at += 16) {
                if (entries.size() >= 64) throw std::runtime_error("unterminated object list");
                entries.push_back({read(at), read(at+4), read(at+8), read(at+12)});
            }
            parts.push_back(std::move(entries));
        }
    }
    static void observe(void *data, const rt::Geo::geo_state &state,
                        const rt::Geo::ObjectSource &source, std::span<const uint32_t>) {
        auto &self = *static_cast<ObjectExport *>(data);
        if (self.inspection || !self.course.keys.contains({source.address, source.texture_points, source.texture_headers})) return;
        self.inspection = std::make_unique<rt::Geo>(state);
        self.prototype = source;
    }
    void write(const std::filesystem::path &directory, const rt::Video &video) {
        if (!inspection) throw std::runtime_error("no scenery snapshot; choose a frame with visible scenery");
        ModelExport complete;
        auto probe = std::move(inspection);
        probe->record_pushes = true;
        probe->object_observer = ModelExport::observe;
        probe->object_observer_data = &complete;
        std::ostringstream table;
        table << "# list part model uv headers count instance(-1: no drawable face or not ROM)\n";
        for (size_t l = 0; l < lists.size(); ++l)
            for (size_t p = 0; p < parts[l].size(); ++p) {
                const auto &part = parts[l][p];
                long instance = -1;
                if (part.model >= 0x800000 && part.model < 0xc00000 && part.uv < 0x800000 && part.headers < 0x800000) {
                    auto command = prototype;
                    command.address = part.model; command.texture_points = part.uv;
                    command.texture_headers = part.headers; command.count = 0;
                    const auto before = complete.instances.size();
                    probe->inspect_object(command);
                    if (complete.instances.size() != before) instance = long(before);
                }
                table << std::hex << lists[l] << std::dec << ' ' << p << ' ' << part.model << ' ' << part.uv << ' '
                      << part.headers << ' ' << part.count << ' ' << instance << '\n';
            }
        complete.write(directory, video, true,
            "world objects outside the scenery cells; local coordinates; snapshot palette; placement and animation are offline data");
        std::ofstream out;
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.open(directory / "lists.txt"); out << table.str(); out.close();
        std::printf("native_world_objects=PASS lists=%zu instances=%zu meshes=%zu\n", lists.size(), complete.instances.size(), complete.meshes.size());
    }
};
}
