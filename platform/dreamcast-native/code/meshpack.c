#include "meshpack.h"
#include "heap.h"
#include "scene_loader.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nm_mem.h"

_Static_assert(sizeof(nm_header_t) == 64 && sizeof(nm_mesh_t) == 64, "DNM1 table layout");
_Static_assert(sizeof(nm_vertex_t) == 16 && sizeof(nm_face_t) == 32, "DNM1 geometry layout");
_Static_assert(offsetof(nm_face_t,group) == 28 && offsetof(nm_face_t,layer) == 30, "DNM6 layer shares the former face padding word");
_Static_assert(sizeof(nm_material_t) == 32 && sizeof(nm_instance_t) == 112, "DNM1 instance layout");
_Static_assert(sizeof(nm_face_group_t) == 24, "DNM3 group bounds layout");
_Static_assert(sizeof(nm_normal_group_t) == 64, "DNM4 normal group layout");
_Static_assert(sizeof(nm_strip_t) == 32, "DNM5 strip layout");
_Static_assert(sizeof(nm_lod_group_t) == 32 && offsetof(nm_lod_group_t,levels) == 10 &&
    offsetof(nm_lod_group_t,centre) == 12 && offsetof(nm_lod_group_t,switch_depth) == 24, "DNM6 level-of-detail group layout");
_Static_assert(offsetof(nm_instance_t,matrix)==16 && offsetof(nm_instance_t,focus)==64,
    "DNM matrix and focus offsets");
#ifdef __DREAMCAST__
_Static_assert(sizeof(shz_mat3x4_t)==48 && _Alignof(shz_mat3x4_t)==4 &&
    offsetof(nm_instance_t,transform)==16,"direct SH4ZAM matrix preserves DNM layout");
#endif

static int zeroes(const uint32_t *p, unsigned count) {
    for (unsigned i = 0; i < count; ++i) if (p[i]) return 0;
    return 1;
}
static int finite_values(const float *p, unsigned count) {
    for (unsigned i = 0; i < count; ++i) if (!isfinite(p[i])) return 0;
    return 1;
}
static const void *table(const uint8_t *data, size_t size, size_t *offset,
                         uint32_t count, size_t stride, unsigned alignment) {
    if (*offset > size) return NULL;
    while (*offset & (alignment-1)) {
        if (*offset == size || data[(*offset)++]) return NULL;
    }
    if (!count || count > (size - *offset) / stride) return NULL;
    const void *result = data + *offset;
    *offset += count * stride;
    return result;
}
static int validate(nm_pack_t *p, size_t size) {
    const uint8_t *data = p->data;
    const nm_header_t *h = p->header = p->data;
    const uint32_t magic = h->magic & ~NM_PACK_ALIGNED;
    const unsigned alignment = h->magic & NM_PACK_ALIGNED ? 32 : 1;
    /* DNM6 carries the DNM4/5 tables only when their counts are nonzero. */
    const int layered = magic == NM_PACK_MAGIC_LAYERED;
    const int stripped = magic == NM_PACK_MAGIC_STRIPS || (layered && h->padding[0]);
    const int normal_grouped = magic == NM_PACK_MAGIC_NORMAL_GROUPED || magic == NM_PACK_MAGIC_STRIPS ||
        (layered && h->reserved);
    const int grouped = magic == NM_PACK_MAGIC_GROUPED || normal_grouped;
    const unsigned layers = layered ? h->padding[2] : 1;
    const int baked = layered && (h->padding[3] & NM_PACK_BAKED_LIGHT);
    const int classed = layered && (h->padding[3] & NM_PACK_SCENERY_CLASSES);
    const unsigned lod_count = layered && (h->padding[3] & NM_PACK_LOD_GROUPS) ? h->padding[3] >> NM_PACK_LOD_COUNT_SHIFT : 0;
    if ((magic != NM_PACK_MAGIC && magic != NM_PACK_MAGIC_SHARED && !grouped && !layered) || h->size != size ||
        (grouped ? !h->reserved || h->reserved > h->faces || h->reserved > 65536 : h->reserved != 0) ||
        (stripped ? !h->padding[0] || !h->padding[1] : !zeroes(h->padding,2)) ||
        (layered ? !layers || layers > 255 ||
                   (h->padding[3] & 0xffffu & ~(NM_PACK_BAKED_LIGHT|NM_PACK_SCENERY_CLASSES|NM_PACK_LOD_GROUPS|NM_PACK_WORLD_OBJECTS|NM_PACK_LIVE_SPECULAR)) ||
                   ((h->padding[3] & NM_PACK_LIVE_SPECULAR) && !(h->padding[3] & NM_PACK_BAKED_LIGHT)) ||
                   /* The count lives with its flag: both or neither. */
                   (h->padding[3] & NM_PACK_LOD_GROUPS ? !lod_count : h->padding[3] >> NM_PACK_LOD_COUNT_SHIFT) :
                   !zeroes(h->padding+2,2)) ||
        h->materials > 65536) return 0;
    for (unsigned a = 0; a < 2; ++a)
        if (h->crtc[a] < -65536 || h->crtc[a] > 65536 ||
            h->render[a] < -65536 || h->render[a] > 65536) return 0;
    size_t offset = sizeof(*h);
    if (!(p->meshes = table(data, size, &offset, h->meshes, sizeof(*p->meshes), alignment)) ||
        !(p->vertices = table(data, size, &offset, h->vertices, sizeof(*p->vertices), alignment)) ||
        !(p->faces = table(data, size, &offset, h->faces, sizeof(*p->faces), alignment)) ||
        !(p->materials = table(data, size, &offset, h->materials, sizeof(*p->materials), alignment)) ||
        !(p->instances = table(data, size, &offset, h->instances, sizeof(*p->instances), alignment))) return 0;
    if ((magic==NM_PACK_MAGIC_SHARED || grouped || layered) &&
        !(p->position_refs=table(data,size,&offset,h->vertices,sizeof(*p->position_refs),alignment))) return 0;
    if (grouped || layered) {
        while (offset & 3) {
            if (offset == size || data[offset++]) return 0;
        }
    }
    if (grouped) {
        if (normal_grouped) {
            if (!(p->normal_groups=table(data,size,&offset,h->reserved,sizeof(*p->normal_groups),alignment))) return 0;
        } else if (!(p->groups=table(data,size,&offset,h->reserved,sizeof(*p->groups),alignment))) return 0;
    }
    if (stripped) {
        while (offset & 31) {
            if (offset == size || data[offset++]) return 0;
        }
        if (!(p->strips=table(data,size,&offset,h->padding[0],sizeof(*p->strips),alignment)) ||
            !(p->strip_indices=table(data,size,&offset,h->padding[1],sizeof(*p->strip_indices),alignment))) return 0;
    }
    if (lod_count) {
        while (offset & 3) {
            if (offset == size || data[offset++]) return 0;
        }
        if (!(p->lod_groups=table(data,size,&offset,lod_count,sizeof(*p->lod_groups),alignment))) return 0;
    }
    if (stripped || layered || alignment == 32) {
        const unsigned tail_alignment = alignment == 32 ? 32 : 4;
        while (offset & (tail_alignment-1)) {
            if (offset == size || data[offset++]) return 0;
        }
    }
    if (offset!=size) return 0;
    uint32_t vertex_end = 0, face_end = 0, group_end = 0;
    uint32_t strip_end = 0, strip_vertex_end = 0, lod_end = 0;
    for (uint32_t i = 0; i < h->meshes; ++i) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_mesh_t *m = &p->meshes[i];
        /* Level-of-detail groups of this mesh (meshpack.h, NM_PACK_LOD_GROUPS). */
        const uint32_t lod_mesh_first = lod_end, lod_mesh_count = lod_count ? m->padding[4] >> 16 : 0;
        if (lod_count) {
            uint32_t range_end = face_end;
            if ((m->padding[4] & 0xffffu) != (lod_mesh_count ? lod_end : 0) || lod_mesh_count > lod_count - lod_end) return 0;
            for (uint32_t g = 0; g < lod_mesh_count; ++g) {
                const nm_lod_group_t *group = p->lod_groups + lod_end + g;
                uint32_t faces = 0;
                if (!group->levels || group->levels > NM_LOD_LEVELS || group->spare || !finite_values(group->centre, 3)) return 0;
                for (unsigned level = 0; level < NM_LOD_LEVELS; ++level) {
                    if (level >= group->levels && group->face_count[level]) return 0;
                    faces += group->face_count[level];
                }
                for (unsigned level = 0; level + 1 < NM_LOD_LEVELS; ++level) {
                    const float depth = group->switch_depth[level];
                    uint32_t bits;
                    nm_memcpy(&bits, &depth, sizeof(bits));
                    if (level + 1 >= group->levels ? bits != 0 :
                        !isfinite(depth) || !(depth > 0) || (level && depth < group->switch_depth[level-1])) return 0;
                }
                for (unsigned axis = 0; axis < 3; ++axis)
                    if (group->centre[axis] < m->bounds[axis] || group->centre[axis] > m->bounds[axis+3]) return 0;
                /* In face order, inside the mesh, never overlapping; a group without any face says nothing. */
                if (!faces || group->face_first < range_end || group->face_first > face_end + m->face_count ||
                    faces > face_end + m->face_count - group->face_first) return 0;
                range_end = group->face_first + faces;
            }
            lod_end += lod_mesh_count;
        } else if (layered && m->padding[4]) return 0;
        if (!m->vertex_count || m->vertex_count > 65536 || !m->face_count || m->mode > 3 ||
            m->vertex_first != vertex_end || m->face_first != face_end ||
            m->vertex_count > h->vertices - vertex_end || m->face_count > h->faces - face_end ||
            !finite_values(m->bounds, 6) ||
            /* padding[4] belongs to the level-of-detail table when the pack has one (checked above). */
            !zeroes(m->padding + (stripped ? 4 : grouped ? 2 : 0), (stripped ? 1u : grouped ? 3u : 5u) - (lod_count ? 1u : 0u)) ||
            (stripped && !grouped && !zeroes(m->padding, 2))) return 0;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (m->bounds[axis] > m->bounds[axis + 3]) return 0;
        if (grouped) {
            if (m->padding[0] != group_end || !m->padding[1] || m->padding[1] > m->face_count ||
                m->padding[1] > h->reserved - group_end) return 0;
            group_end += m->padding[1];
            for (unsigned g=m->padding[0]; g<group_end; ++g) {
                const float *bounds=nm_pack_group_bounds(p,g);
                if (!finite_values(bounds,6)) return 0;
                for (unsigned a=0; a<3; ++a)
                    if (bounds[a]>bounds[a+3] || bounds[a]<m->bounds[a] || bounds[a+3]>m->bounds[a+3]) return 0;
                if (normal_grouped) {
                    const nm_normal_group_t *group=p->normal_groups+g;
                    uint32_t normal_length_bits;
                    nm_memcpy(&normal_length_bits,&group->normal_length,sizeof(normal_length_bits));
                    if (!finite_values(group->normal_center,4) || !finite_values(group->point_center,4) ||
                        group->normal_radius<0 || group->point_radius<0 ||
                        group->flags & ~NM_NORMAL_GROUP_CONE || normal_length_bits) return 0;
                }
            }
        }
        for (uint32_t v = 0; v < m->vertex_count; ++v) {
            const float *point = p->vertices[vertex_end + v].position;
            if (!finite_values(point, 3)) return 0;
            if (p->position_refs) {
                const unsigned representative=p->position_refs[vertex_end+v];
                if (representative>v || p->position_refs[vertex_end+representative]!=representative ||
                    memcmp(point,p->vertices[vertex_end+representative].position,3*sizeof(float))) return 0;
            }
            for (unsigned axis = 0; axis < 3; ++axis)
                if (point[axis] < m->bounds[axis] || point[axis] > m->bounds[axis + 3]) return 0;
        }
        for (uint32_t f = 0; f < m->face_count; ++f) {
            const nm_face_t *face = &p->faces[face_end + f];
            if ((face->count != 3 && face->count != 4) || face->material >= h->materials ||
                /* Baked static light: front luma, and the other side's for
                 * a two-sided face. Zero everywhere else. */
                (baked ? !(face->attribute & (1u << 17)) && face->padding : face->reserved || face->padding) ||
                face->layer >= layers ||
                (grouped ? face->group < m->padding[0] || face->group >= group_end : face->group != 0) || !(face->attribute & 3) ||
                !((face->attribute >> 8) & 3) ||
                face->count != (face->attribute & 1 ? 4 : 3) ||
                !finite_values(face->normal, 3)) return 0;
            for (unsigned v = 0; v < face->count; ++v) {
                if (face->indices[v] >= m->vertex_count) return 0;
                if (grouped) {
                    const float *point=p->vertices[vertex_end+face->indices[v]].position;
                    const float *bounds=nm_pack_group_bounds(p,face->group);
                    for (unsigned a=0; a<3; ++a)
                        if (point[a]<bounds[a] || point[a]>bounds[a+3]) return 0;
                }
            }
            if (face->count == 3 && face->indices[3]) return 0;
        }
        /* One bounds group per level-of-detail group. */
        for (uint32_t g = 0; g < lod_mesh_count; ++g) {
            const nm_lod_group_t *group = p->lod_groups + lod_mesh_first + g;
            const uint32_t faces = (uint32_t)group->face_count[0] + group->face_count[1] + group->face_count[2];
            for (uint32_t f = 1; f < faces; ++f)
                if (p->faces[group->face_first+f].group != p->faces[group->face_first].group) return 0;
        }
        if (stripped) {
            if (m->padding[2] != strip_end || m->padding[3] > h->padding[0]-strip_end) return 0;
            uint32_t source_face_end = face_end;
            for (uint32_t s=0; s<m->padding[3]; ++s) {
                const nm_strip_t *strip=p->strips+strip_end;
                if (strip->face_count<2 || strip->face_first<source_face_end ||
                    strip->face_first>face_end+m->face_count ||
                    strip->face_count>face_end+m->face_count-strip->face_first ||
                    strip->vertex_count<4 || strip->vertex_first!=strip_vertex_end ||
                    strip->vertex_count>h->padding[1]-strip_vertex_end ||
                    !zeroes(strip->padding,5)) return 0;
                const uint16_t *indices=p->strip_indices+strip_vertex_end;
                for (unsigned v=0; v<strip->vertex_count; ++v)
                    if (indices[v]>=m->vertex_count) return 0;
                unsigned triangle=0;
                const unsigned material=p->faces[strip->face_first].material;
                const unsigned layer=p->faces[strip->face_first].layer;
                for (unsigned f=0; f<strip->face_count; ++f) {
                    const nm_face_t *face=p->faces+strip->face_first+f;
                    if (face->material!=material || face->layer!=layer) return 0;
                    for (unsigned v=1; v+1<face->count; ++v,++triangle) {
                        if (triangle+2>=strip->vertex_count) return 0;
                        const nm_vertex_t *actual[3]={
                            p->vertices+vertex_end+indices[triangle+(triangle&1)],
                            p->vertices+vertex_end+indices[triangle+!(triangle&1)],
                            p->vertices+vertex_end+indices[triangle+2]};
                        const nm_vertex_t *expected[3]={
                            p->vertices+vertex_end+face->indices[0],
                            p->vertices+vertex_end+face->indices[v],
                            p->vertices+vertex_end+face->indices[v+1]};
                        /* The face's triangle, either winding (culling is off:
                         * a triangle is its three vertices, meshpack.h). */
                        unsigned rotation=0;
                        for (; rotation<6; ++rotation) {
                            const unsigned step = rotation<3 ? 1 : 2;
                            if (!memcmp(actual[0],expected[rotation%3],sizeof(nm_vertex_t)) &&
                                !memcmp(actual[1],expected[(rotation+step)%3],sizeof(nm_vertex_t)) &&
                                !memcmp(actual[2],expected[(rotation+2*step)%3],sizeof(nm_vertex_t))) break;
                        }
                        if (rotation==6) return 0;
                    }
                }
                if (triangle+2!=strip->vertex_count) return 0;
                /* A strip never leaves a level-of-detail range, nor enters one. */
                for (uint32_t g = 0; g < lod_mesh_count; ++g) {
                    const nm_lod_group_t *group = p->lod_groups + lod_mesh_first + g;
                    uint32_t first = group->face_first;
                    const uint32_t strip_last = strip->face_first + strip->face_count - 1;
                    for (unsigned level = 0; level <= NM_LOD_LEVELS; ++level) {
                        /* Boundaries: the group's start, then the end of each range. */
                        if (strip->face_first < first && strip_last >= first) return 0;
                        if (level < NM_LOD_LEVELS) first += group->face_count[level];
                    }
                }
                source_face_end=strip->face_first+strip->face_count;
                strip_vertex_end+=strip->vertex_count;
                ++strip_end;
            }
        }
        vertex_end += m->vertex_count; face_end += m->face_count;
    }
    if (vertex_end != h->vertices || face_end != h->faces || group_end != h->reserved || lod_end != lod_count ||
        (stripped && (strip_end!=h->padding[0] || strip_vertex_end!=h->padding[1]))) return 0;
    /* World objects (NM_PACK_WORLD_OBJECTS): contiguous behind the cell
     * instances; their meshes use object-stage materials only and the cell
     * meshes never do. A textured object material may carry a super-sample
     * switch depth (meshpack.h, nm_material_supersample_depth). */
    const int objects = layered && (h->padding[3] & NM_PACK_WORLD_OBJECTS);
    for (uint32_t i = 0; i < h->materials; ++i) {
        const nm_material_t *m = &p->materials[i];
        const float ss = nm_material_supersample_depth(m);
        if (!isfinite(m->diffuse) || !isfinite(m->ambient) || !isfinite(m->specular_scale) ||
            m->diffuse < 0 || m->diffuse > 255 || m->ambient < 0 || m->ambient > 255 ||
            m->specular_scale < 0 || m->specular_scale > 255 || m->specular_control > 255 ||
            (classed ? (m->padding[0] & ~(NM_SCENERY_STAGE_MASK|NM_SCENERY_SURFACE|NM_SCENERY_POINT)) ||
                       (m->padding[0] & NM_SCENERY_STAGE_MASK) > NM_SCENERY_STAGE_LAST ||
                       (m->padding[1] && (!objects || (m->padding[0] & NM_SCENERY_STAGE_MASK) != NM_SCENERY_STAGE_OBJECTS ||
                                          !(m->header[0] & 0x4000u) || !isfinite(ss) || !(ss > 0))) :
                       !zeroes(m->padding, 2))) return 0;
    }
    if (objects && (!baked || !classed)) return 0;
    uint32_t object_instances = 0;
    for (uint32_t i = 0; i < h->instances; ++i) {
        if (!(i & 63u)) nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        const nm_instance_t *inst = &p->instances[i];
        if (inst->mesh >= h->meshes || inst->window > 255 || inst->reverse > 1 ||
            !finite_values(inst->matrix, 12) || !finite_values(inst->focus, 2) ||
            !finite_values(inst->light, 3) || !zeroes(inst->padding + 1, 3)) return 0;
        const uint32_t word = inst->padding[0];
        if (!objects ? word != 0 : word ? !(word & NM_OBJECT_INSTANCE) ||
                (word & ~(NM_OBJECT_INSTANCE | 0xffu | NM_OBJECT_NEAR | NM_OBJECT_PLACED | NM_OBJECT_SPIN |
                    NM_OBJECT_MESHES | 0xf000u | 0xff0000u)) ||
                (NM_OBJECT_KIND(word) >= NM_OBJECT_KINDS && NM_OBJECT_KIND(word) != NM_OBJECT_CLOCK) ||
                (NM_OBJECT_KIND(word) == NM_OBJECT_CLOCK && (word & (NM_OBJECT_SPIN | NM_OBJECT_MESHES))) ||
                ((word & NM_OBJECT_SPIN) && !(word & NM_OBJECT_PLACED)) ||
                ((word & NM_OBJECT_MESHES) && inst->mesh + NM_OBJECT_MESH_COUNT(word) > h->meshes) ||
                (NM_OBJECT_KIND(word) == NM_OBJECT_ALWAYS && !(word & NM_OBJECT_MESHES) && NM_OBJECT_VARIANT(word)) :
                object_instances != 0) return 0;
        object_instances += word != 0;
        if (objects) {
            /* A super-sample switch depth: the same on every face of the mesh, which a PLACED object draws. */
            const uint32_t meshes = word & NM_OBJECT_MESHES ? NM_OBJECT_MESH_COUNT(word) : 1u;
            for (uint32_t k = 0; k < meshes; ++k) {
                const nm_mesh_t *m = &p->meshes[inst->mesh + k];
                const uint32_t ss = m->face_count ? p->materials[p->faces[m->face_first].material].padding[1] : 0;
                if (ss && !(word & NM_OBJECT_PLACED)) return 0;
                for (uint32_t f = 0; f < m->face_count; ++f) {
                    const nm_material_t *material = &p->materials[p->faces[m->face_first + f].material];
                    if (((material->padding[0] & NM_SCENERY_STAGE_MASK) == NM_SCENERY_STAGE_OBJECTS) != (word != 0) ||
                        material->padding[1] != ss) return 0;
                }
            }
        }
        for (unsigned axis = 0; axis < 4; ++axis)
            if (inst->viewport[axis] < -2048 || inst->viewport[axis] > 2047) return 0;
        for (unsigned axis = 0; axis < 2; ++axis)
            if (inst->center[axis] < -2048 || inst->center[axis] > 2047) return 0;
    }
    if (objects && !object_instances) return 0;
    return 1;
}

int nm_pack_load(nm_pack_t *pack, const char *path) {
    const uint32_t endian = 1;
    if (!pack || pack->data || !path || *(const uint8_t *)&endian != 1) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "DNM1: open %s: %s\n", path, strerror(errno));
        return 0;
    }
    nm_header_t header;
    nm_pack_t candidate = {0};
    int success = 0;
    const char *stage = "buffer/header";
    /* Same bulk-read pattern as the existing texture loader, no tiny network reads. */
    if (setvbuf(file, NULL, _IOFBF, 64 * 1024) ||
        fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        ((header.magic & ~NM_PACK_ALIGNED) != NM_PACK_MAGIC &&
         (header.magic & ~NM_PACK_ALIGNED) != NM_PACK_MAGIC_SHARED &&
         (header.magic & ~NM_PACK_ALIGNED) != NM_PACK_MAGIC_GROUPED &&
         (header.magic & ~NM_PACK_ALIGNED) != NM_PACK_MAGIC_NORMAL_GROUPED &&
         (header.magic & ~NM_PACK_ALIGNED) != NM_PACK_MAGIC_STRIPS &&
         (header.magic & ~NM_PACK_ALIGNED) != NM_PACK_MAGIC_LAYERED) || header.size < sizeof(header) ||
        header.size > NM_PACK_MAX_BYTES) goto done;
    stage = "allocation";
    /* Keep cache-line-aligned table offsets aligned after loading too. The
     * validated NM_PACK_MAX_BYTES ceiling makes rounding safe; file size is unchanged. */
    candidate.data = nm_heap_alloc(path, 32, (header.size + 31u) & ~31u);
    if (!candidate.data) goto done;
    nm_memcpy(candidate.data, &header, sizeof(header));
    stage = "payload/EOF";
    if (fread((uint8_t *)candidate.data + sizeof(header), 1, header.size - sizeof(header), file)
            != header.size - sizeof(header) || fgetc(file) != EOF || ferror(file)) goto done;
    stage = "validation";
    success = validate(&candidate, header.size);
done:
    if (fclose(file)) { success = 0; stage = "close"; }
    if (success) *pack = candidate;
    else {
        fprintf(stderr, "DNM1: %s failed for %s\n", stage, path);
        free(candidate.data);
    }
    return success;
}

void nm_pack_unload(nm_pack_t *pack) {
    if (!pack) return;
    free(pack->data);
    *pack = (nm_pack_t){0};
}
