#include "../include/meshpack.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    nm_pack_t pack = {0};
    if (!nm_pack_load(&pack, argv[1])) {
        assert(!pack.data && !pack.header);
        nm_pack_unload(&pack);
        puts("REJECT: invalid DNM1/2/3/4/5");
        return 1;
    }
    assert(((uintptr_t)pack.data & 31u) == 0);
    if (pack.header->magic & NM_PACK_ALIGNED) {
        const void *tables[] = {pack.header, pack.meshes, pack.vertices, pack.faces,
            pack.materials, pack.instances, pack.position_refs, pack.groups,
            pack.normal_groups, pack.strips, pack.strip_indices, pack.lod_groups};
        for (unsigned i=0; i<sizeof(tables)/sizeof(*tables); ++i)
            assert(!tables[i] || !((uintptr_t)tables[i] & 31u));
    }
    void *original = pack.data;
    assert(!nm_pack_load(&pack, argv[1]) && pack.data == original);
    printf("PASS: DNM1/2/3/4/5 %u bytes, %u meshes, %u vertices, %u faces, %u materials, %u instances\n",
           pack.header->size, pack.header->meshes, pack.header->vertices, pack.header->faces,
           pack.header->materials, pack.header->instances);
    if (nm_pack_lod_count(&pack)) {
        unsigned levels[NM_LOD_LEVELS] = {0}, faces = 0, empty = 0;
        for (unsigned g = 0; g < nm_pack_lod_count(&pack); ++g) {
            const nm_lod_group_t *group = pack.lod_groups + g;
            ++levels[group->levels - 1];
            for (unsigned level = 0; level < group->levels; ++level) {
                faces += group->face_count[level];
                empty += !group->face_count[level];
            }
        }
        printf("PASS: level-of-detail groups %u (%u/%u/%u with one/two/three levels), %u faces in their ranges, %u empty ranges\n",
               nm_pack_lod_count(&pack), levels[0], levels[1], levels[2], faces, empty);
    }
    nm_pack_unload(&pack);
    assert(!pack.data && !pack.header);
    nm_pack_unload(&pack);
}
