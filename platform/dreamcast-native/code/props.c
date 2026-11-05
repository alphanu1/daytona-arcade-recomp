/* Course props (cones): include/props.h. Every float operation is one TGP command's, in its order
 * (spec-cones.md section 6); compares of floats against records are compares of their bit patterns,
 * as the i960 makes them. */
#include "props.h"
#include "force_math.h"
#include <string.h>
#include "nm_mem.h"

static float bits_float(uint32_t bits) { return nm_force_float(bits); }

/* 223800: no overflow check (a full ring looks empty). */
static void enqueue(nm_props_t *s, uint8_t id) {
    s->queue[s->write] = id;
    s->write = (uint8_t)((s->write + 1u) % NM_PROPS_QUEUE);
}

/* 223620: the slope angles of ground quad `face` from (v0 - v2) x (v1 - v2). */
static int slope(nm_prop_t *c, const nm_drive_params_t *math, const nm_contact_pack_t *contact, uint32_t face) {
    if (face >= contact->quad_count) return 0;
    const float (*v)[3] = contact->quads[face].xyz;
    const float r4 = v[0][0] - v[2][0], r5 = v[0][1] - v[2][1], r6 = v[0][2] - v[2][2];
    const float r7 = v[1][0] - v[2][0], r8 = v[1][1] - v[2][1], r9 = v[1][2] - v[2][2];
    const float a = r5 * r9, b = r6 * r8, r10 = a - b;
    const float d = r6 * r7, e = r4 * r9, r11 = d - e;
    const float f = r8 * r4, g = r5 * r7, r12 = f - g;
    const int32_t g0 = nm_force_angle(math, r11, r12), g1 = nm_force_angle(math, r11, r10);
    if (g0 == INT32_MIN || g1 == INT32_MIN) return 0;
    c->h54 = (int16_t)(-g0);
    c->h56 = (int16_t)g1;
    return 1;
}

/* 11620 with g4 = 0: height, quad and its surface mask; the previous height is the cone's y. */
static int ground(nm_prop_t *c, const nm_drive_params_t *math, const nm_contact_pack_t *contact,
                  nm_contact_result_t *out) {
    *out = (nm_contact_result_t){.height = c->y, .surface = 0};
    return nm_contact_query(contact, math, c->x, c->z, out);
}

int nm_props_init(nm_props_t *s, const nm_prop_data_t *data, const nm_drive_params_t *math,
                  const nm_contact_pack_t *contact) {
    if (!s || !data || !math || !contact || data->cones > NM_PROPS_MAX) return 0;
    for (unsigned i = 0; i < data->cones; ++i)
        if (data->cone[i].kind >= NM_PROP_KINDS || data->kind[data->cone[i].kind].rest >= data->record_count) return 0;
    /* 222d30 zeroes the indices, masks, counter, sound and cooldown; the queue's bytes and the speeds keep
     * the scene before's (the indices make the queue empty; the manager's first tick sets the speeds). */
    uint8_t queue[NM_PROPS_QUEUE];
    const float speed2 = s->speed2, speed01 = s->speed01;
    const uint32_t hits = s->hits, sent = s->sent, last_tick = s->last_tick;
    const uint8_t course = s->course;
    nm_memcpy(queue, s->queue, sizeof(queue));
    nm_memset(s, 0, sizeof(*s));
    nm_memcpy(s->queue, queue, sizeof(queue));
    s->speed2 = speed2; s->speed01 = speed01;
    s->hits = hits; s->sent = sent; s->last_tick = last_tick; s->course = course;
    s->count = data->cones;
    for (unsigned i = 0; i < data->cones; ++i) {
        nm_prop_t *c = s->cone + i;
        /* 2002c: position and heading from the actor; 223144..2231bc. */
        c->x = data->cone[i].x; c->y = data->cone[i].y; c->z = data->cone[i].z;
        c->id = data->cone[i].id; c->kind = data->cone[i].kind;
        c->h52 = c->h58 = data->cone[i].heading;
        c->rec = data->kind[c->kind].rest;
        c->face = UINT32_MAX;
        if (!nm_contact_scenery_cell(c->x, c->z, &c->cell)) return 0;
        nm_contact_result_t q;
        if (!ground(c, math, contact, &q)) return 0;
        c->y = q.height;
        c->face = q.surface;
        if (!slope(c, math, contact, c->face)) return 0;
    }
    s->started = 1;
    return 1;
}

/* 11bd4's transform with the footprint corners: identity, 0x12 translate, 0x15 yaw, 0x16 roll, 0x1a
 * (include/wheels.h nm_wheels_transform, the same separate operations, zero terms included). */
static int footprint(const nm_drive_params_t *p, const nm_props_player_t *player, const float corners[4][3],
                     float out[4][3]) {
    const float cy = nm_drive_sine(p, (uint16_t)((int)player->yaw + 16384)), sy = nm_drive_sine(p, (uint16_t)player->yaw);
    const float cr = nm_drive_sine(p, (uint16_t)((int)player->roll + 16384)), sr = nm_drive_sine(p, (uint16_t)player->roll);
    static const float identity[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float matrix[3][3], origin[3];
    for (unsigned a = 0; a < 3; ++a) {
        const float first = cy * identity[0][a] + sy * identity[2][a];
        matrix[2][a] = cy * identity[2][a] - sy * identity[0][a];
        matrix[0][a] = cr * first - sr * identity[1][a];
        matrix[1][a] = cr * identity[1][a] + sr * first;
        origin[a] = 0.0f + player->position[0] * identity[0][a];
        origin[a] += player->position[1] * identity[1][a];
        origin[a] += player->position[2] * identity[2][a];
    }
    for (unsigned v = 0; v < 4; ++v) for (unsigned a = 0; a < 3; ++a) {
        float value = origin[a] + matrix[0][a] * corners[v][0];
        value += matrix[1][a] * corners[v][1];
        value += matrix[2][a] * corners[v][2];
        if (!isfinite(value)) return 0;
        out[v][a] = value;
    }
    return 1;
}

int nm_props_manager(nm_props_t *s, const nm_drive_params_t *math, const nm_prop_data_t *data,
                     const nm_props_player_t *player) {
    s->sound = 0;
    uint32_t lo = 0, hi = 0;
    unsigned r = s->read;
    for (unsigned k = 0; k < 2 && r != s->write; ++k) {   /* 222df8 / 222e24 */
        const uint8_t b = s->queue[r];
        if (b & 0x20) hi |= 1u << (b & 31); else lo |= 1u << (b & 31);
        r = (r + 1u) % NM_PROPS_QUEUE;
    }
    s->read = (uint8_t)r; s->lo = lo; s->hi = hi;
    s->speed2 = 2.0f * player->speed;   /* TGP 0x02 */
    s->speed01 = 0.05f * s->speed2;     /* 0x3d4ccccd */
    float c[4][3];
    if (!footprint(math, player, data->corners, c)) return 0;
    const float y = c[3][1];             /* TGP 0x31: corners 0, 1, 3, 2 at corner 3's height */
    const unsigned order[4] = {0, 1, 3, 2};
    for (unsigned k = 0; k < 4; ++k) {
        s->perimeter[k][0] = c[order[k]][0]; s->perimeter[k][1] = y; s->perimeter[k][2] = c[order[k]][2];
    }
    return 1;
}

int nm_props_cone(nm_props_t *s, unsigned i, const nm_prop_data_t *data, const nm_drive_params_t *math,
                  const nm_contact_pack_t *contact, const nm_props_player_t *player, const uint32_t bitmaps[16]) {
    if (i >= s->count) return 0;
    nm_prop_t *c = s->cone + i;
    const nm_prop_kind_t *kind = data->kind + c->kind;
    c->drawn = 0;
    if (!c->count && !((bitmaps[c->cell >> 5] >> (c->cell & 31)) & 1u)) return 1;   /* 2231c4..2231e4 */
    unsigned g8 = c->rec;
    const int inside = nm_contact_contains((const float (*)[3])s->perimeter, c->x, c->z);   /* TGP 0x32 */
    if (inside < 0 || g8 >= data->record_count) return 0;
    const nm_prop_record_t *r = data->records + g8;
    if (inside && !((int32_t)r->w4 > (int32_t)kind->limit)) {
        enqueue(s, c->id);
        ++s->hits;
        if (!((int32_t)r->w0 > (int32_t)nm_force_bits(s->speed01))) {   /* restart, 223254 */
            const unsigned fast = player->speed_index > 70u, n = kind->set_count[fast];   /* cmpobg */
            if (!n) return 0;
            const uint32_t k = s->counter++;
            const unsigned table = kind->set[fast][k % n];
            if (table >= data->table_count) return 0;
            uint32_t count = (uint32_t)data->tables[table].count - 1u;
            g8 = data->tables[table].first;
            if (c->hit) { count -= 4u; g8 += 4u; }   /* a re-hit starts at record 4 */
            if (g8 + (uint16_t)count >= data->record_count) return 0;
            c->count = (uint16_t)count; c->rec = (uint16_t)g8;
            r = data->records + g8;
            c->h5a = r->h8;
            c->h52 = (int16_t)(player->heading - r->h8);
        }
        r = data->records + g8;
        c->v60 = (int32_t)r->w0 < (int32_t)nm_force_bits(s->speed2) ? s->speed2 - bits_float(r->w0) : 0.0f;   /* 2232d4 */
        s->sound = kind->sound;
        c->hit = 1;
    }
    if (!c->count) c->draw_y = bits_float(r->w4) + c->y;   /* 223500: at rest, or finished */
    else {
        --c->count;
        c->rec = (uint16_t)++g8;
        r = data->records + g8;
        const uint32_t mask = c->id & 0x20 ? s->hi : s->lo;
        if ((mask >> (c->id & 31)) & 1u) {   /* 223338: released by the manager this tick */
            enqueue(s, c->id);
            if (!nm_contact_scenery_cell(c->x, c->z, &c->cell)) return 0;
            nm_contact_result_t q;
            if (!ground(c, math, contact, &q)) return 0;
            c->y = q.height;
            if (q.surface != c->face) {
                if (!slope(c, math, contact, q.surface)) return 0;
                c->face = q.surface;
                if (q.mask & 0x21u) {   /* surface type 0 or 5: mirrored about the quad's heading (TGP 0x30) */
                    const int32_t heading = (int32_t)contact->quads[q.surface].heading;
                    c->h52 = (int16_t)(2 * (heading - r->h8) - c->h52);
                    c->v60 = 0.8f * c->v60;   /* 0x3f4ccccd */
                }
            }
        }
        const uint16_t turn = (uint16_t)((uint32_t)(r->h8 - c->h5a) << 2);
        float v = c->v60 * nm_drive_sine(math, (uint16_t)(turn + 16384u));   /* TGP 0x1e: v cos(a) */
        if (nm_force_bits(v) >> 31) v = 0.0f;
        v = 0.99f * v;   /* 0x3f7d70a4 */
        c->v60 = v;
        c->h5a = r->h8;
        const float speed = v + bits_float(r->w0);
        c->draw_y = bits_float(r->w4) + c->y;
        float step[2];   /* TGP 0x47 */
        nm_force_cartesian(math, (uint16_t)(-(c->h52 + r->h8)), speed, step);
        c->x = c->x + step[0];
        c->z = c->z + step[1];
    }
    /* 223528..22360c: the drawn yaw follows h52 by at most 910 a tick. */
    int16_t yaw = c->h52;
    if (c->h52 != c->h58) {
        const int16_t d = (int16_t)(c->h52 - c->h58);
        yaw = d > 910 ? (int16_t)(c->h58 + 910) : d < -910 ? (int16_t)(c->h58 - 910) : c->h52;
        c->h58 = yaw;
    }
    c->yaw = yaw; c->he = r->he; c->hc = r->hc; c->ha = r->ha;
    c->drawn = 1;
    return isfinite(c->x) && isfinite(c->z) && isfinite(c->draw_y);
}

uint32_t nm_props_sound_manager(nm_props_t *s) {
    if (s->cooldown) { --s->cooldown; return 0; }
    if (!s->sound) return 0;
    s->cooldown = 3;
    ++s->sent;
    return s->sound;
}

int nm_props_tick(nm_props_t *s, const nm_prop_data_t *data, const nm_drive_params_t *math,
                  const nm_contact_pack_t *contact, const nm_props_player_t *player, const uint32_t bitmaps[16],
                  uint32_t *sound) {
    *sound = 0;
    if (!s->started) return nm_props_init(s, data, math, contact);   /* the init entries: no update */
    if (!nm_props_manager(s, math, data, player)) return 0;
    for (unsigned i = 0; i < s->count; ++i)
        if (!nm_props_cone(s, i, data, math, contact, player, bitmaps)) return 0;
    *sound = nm_props_sound_manager(s);
    return 1;
}
