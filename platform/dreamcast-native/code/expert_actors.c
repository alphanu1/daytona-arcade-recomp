/* Expert's world actors with state or a placement of their own: include/expert_actors.h. Every float operation is
 * one TGP command's, in its order; compares of floats are the i960's (cmpr: unordered is neither below nor above). */
#include "expert_actors.h"
#include "force_math.h"
#include <string.h>
#include "nm_mem.h"


static int cell_drawn(const uint32_t *bitmaps, unsigned near, unsigned cell) {
    return nm_clock_cell(bitmaps, near, cell);
}
static nm_xa_op_t op_t(float x, float y, float z) { return (nm_xa_op_t){NM_XA_T, 0, {x, y, z}}; }
static nm_xa_op_t op_r(unsigned axis, uint32_t angle) { return (nm_xa_op_t){(uint8_t)(NM_XA_RX + axis), (uint16_t)angle, {0, 0, 0}}; }
static nm_xa_op_t op_s(float s) { return (nm_xa_op_t){NM_XA_S, 0, {s, s, s}}; }
static void draw(nm_xa_draw_t *d, unsigned frame, unsigned count, const nm_xa_op_t *ops) {
    d->frame = (int16_t)frame; d->count = (uint8_t)count;
    nm_memcpy(d->op, ops, count * sizeof(*ops));
}

/* Command 43: the length of (a - c, d - b) (force_math.h nm_force_length; check_road.hpp's proximity oracle). */
static float distance43(const nm_drive_params_t *m, float a, float b, float c, float d) {
    return nm_force_length(m, a - c, d - b);
}
/* 2249b8 / 224a50: the player within 8 of (x, z): command 43 with its sign cleared, cmpr 8.0, bge. */
static int near_player(const nm_drive_params_t *m, float x, float z, const nm_xa_input_t *in) {
    const float d = nm_force_float(nm_force_bits(distance43(m, x, z, in->player_position[0], in->player_position[2])) & 0x7fffffffu);
    return !(d >= 8.0f);
}

/* 22430c's frame of a counter: (x & 7f) > 5f ? 31 : x & 1f (224374..224388). */
static unsigned bird_frame(uint32_t x) {
    const uint32_t r = x & 0x7fu;
    return (r > 0x5fu ? 31u : r) & 31u;
}

/* 223be0: the bearing. 0: refused. */
static int bearing(const nm_clock_actor_t *a, const nm_drive_params_t *m, const nm_xa_input_t *in, nm_xa_draw_t *d) {
    if (!cell_drawn(in->bitmaps, 0, a->cell)) return 1;
    const int back = (in->player_flags >> 28) & 1u;   /* bbs 28 */
    const float *p = a->param + (back ? 3 : 0);
    const int32_t sign = back ? -1 : 1;
    const float distance = distance43(m, in->camera[0], in->camera[1], p[0], p[2]);
    const int32_t angle = nm_force_angle(m, distance, p[1]);   /* command 0a (distance, y) */
    if (angle == INT32_MIN) return 0;
    draw(d, (unsigned)((angle * sign) >> 6) & 63u, 0, NULL);  /* muli, shri 6, and 3f */
    return 1;
}

/* 22432c. */
static void birds(const nm_clock_actor_t *a, const nm_xa_input_t *in, nm_xa_draw_t *d) {
    if (!cell_drawn(in->bitmaps, 0, a->cell)) return;
    const float x = a->position[0], y = a->position[1], z = a->position[2];
    const uint32_t c = in->counter, h = (uint32_t)(int32_t)a->heading;
    uint32_t angle = (c << 7) + h;
    unsigned frame = bird_frame(c);
    for (unsigned k = 0; k < 2; ++k) {
        const nm_xa_op_t ops[] = {op_t(x, y, z), op_r(1, angle + 0x8000u * k), op_t(15.0f, 0, 0)};
        draw(d + k, frame, 3, ops);
    }
    frame = bird_frame((c >> 1) + 14u);
    angle = (c << 7) + h;
    static const uint32_t turns[3] = {0x500u, 0x2800u, 0x3800u};   /* r7 += 500, 2300, 1000 */
    for (unsigned k = 0; k < 3; ++k) {
        const nm_xa_op_t ops[] = {op_t(x, y, z), op_t(5.0f, 2.0f, 0), op_r(1, angle + turns[k]), op_t(15.0f, 0, 0)};
        draw(d + 2 + k, frame, 4, ops);
    }
    if (!(in->flags & UINT32_C(0x10000000))) return;   /* 2245ec..224610 (501a80: never set here) */
    const uint32_t c3 = c + 31u;
    frame = (c3 & 63u) > 31u ? 31u : c3 & 63u;
    angle = (c3 << 7) + h;
    for (unsigned k = 0; k < 2; ++k) {
        const nm_xa_op_t ops[] = {op_t(x, y, z), op_t(5.0f, 2.0f, 0), op_t(-12.0f, -1.0f, 0),
                                  op_r(1, angle + 0x6000u + 0x800u * k), op_t(15.0f, 0, 0)};
        draw(d + 5 + k, frame, 5, ops);
    }
}

/* 22475c. */
static void big_bird(nm_xa_state_t *s, const nm_clock_actor_t *a, const nm_xa_input_t *in, nm_xa_draw_t *d) {
    if (in->start_edge) {
        const float grown = 0.5f + s->scale;          /* command 0 */
        if (!(grown > 100.0f)) s->scale = grown;      /* cmpr 100, bg */
    }
    if (!cell_drawn(in->bitmaps, 0, a->cell)) return;
    const nm_xa_op_t ops[] = {op_t(a->position[0], a->position[1], a->position[2]), op_r(1, (uint32_t)(int32_t)a->heading),
                              op_s(s->scale)};
    draw(d, in->counter & 31u, 3, ops);
}

/* 224854..224ccc, one tick of a horse. 0: refused. */
static int horse(nm_xa_horse_t *h, const nm_clock_actor_t *a, const nm_drive_params_t *m, const nm_contact_pack_t *contact,
                 const nm_xa_input_t *in, nm_xa_draw_t *d) {
    switch (h->state) {
    case NM_XA_HORSE_START:   /* 224854: the init entry again */
        h->state = NM_XA_HORSE_CIRCLE; h->drawn = 0; h->cell = a->cell;
        return 1;
    case NM_XA_HORSE_CIRCLE: {   /* 22487c */
        if (!cell_drawn(in->bitmaps, 0, h->cell)) return 1;
        const float wx = h->x + h->o[0], wz = h->z + h->o[2];   /* 2249b8: command 0, twice */
        if (near_player(m, wx, wz, in)) h->state = NM_XA_HORSE_FLEE_START;
        const uint32_t n = ++h->drawn;
        const uint32_t b = (n << 6) + (uint32_t)(int32_t)a->heading;
        const float ox = nm_drive_sine(m, (uint16_t)b) * 25.0f;            /* command 1d */
        const float oz = nm_drive_sine(m, (uint16_t)(b + 16384u)) * 35.0f; /* command 1e */
        h->o[0] = ox; h->o[1] = 0.0f; h->o[2] = oz;
        const nm_xa_op_t ops[] = {op_t(h->x, h->y, h->z), op_r(2, 0x222u), op_t(ox, 0.0f, oz), op_r(1, 0xc000u - b)};
        draw(d, n & 31u, 4, ops);
        return 1;
    }
    case NM_XA_HORSE_FLEE_START:   /* 224ab0: from where it was drawn */
        h->o[0] = h->x + h->o[0]; h->o[1] = h->y + h->o[1]; h->o[2] = h->z + h->o[2];
        /* fall through */
    case NM_XA_HORSE_LATCH:   /* 224b1c */
        h->heading = in->player_heading;
        h->speed = 1.5f * in->player_speed;   /* command 2 */
        h->state = NM_XA_HORSE_FLEE;
        /* fall through */
    case NM_XA_HORSE_FLEE: {   /* 224b60 */
        if (near_player(m, h->o[0], h->o[2], in)) h->state = NM_XA_HORSE_LATCH;   /* 224a50 */
        if (!nm_contact_scenery_cell(h->o[0], h->o[2], &h->cell)) return 0;     /* 16e10 */
        if (!cell_drawn(in->bitmaps, 0, h->cell)) {   /* 224c78 */
            ++h->drawn; h->state = NM_XA_HORSE_WAIT_START;
            return 1;
        }
        const float x = h->o[0], y = h->o[1], z = h->o[2];
        nm_contact_result_t ground = {.height = y, .surface = 0};
        if (!nm_contact_query(contact, m, x, z, &ground)) return 0;   /* 11620 */
        h->o[1] = ground.height;
        float step[2];   /* command 47 */
        nm_force_cartesian(m, (uint16_t)(0u - h->heading), h->speed, step);
        h->o[0] = step[0] + x; h->o[2] = step[1] + z;
        const uint32_t n = ++h->drawn;
        const nm_xa_op_t ops[] = {op_t(h->o[0], y, h->o[2]), op_r(1, h->heading)};
        draw(d, n & 31u, 2, ops);
        return isfinite(h->o[0]) && isfinite(h->o[2]);
    }
    case NM_XA_HORSE_WAIT_START:   /* 224c94 */
        h->wait = 0; h->state = NM_XA_HORSE_WAIT;
        /* fall through */
    case NM_XA_HORSE_WAIT:   /* 224ca8 */
        if (h->wait + 1u >= 2560u) h->state = NM_XA_HORSE_START;   /* (signed: the count stays small) */
        ++h->wait;
        return 1;
    }
    return 0;
}

/* 224cec. */
static void pair(nm_xa_state_t *s, const nm_clock_actor_t *a, const nm_xa_input_t *in, nm_xa_draw_t *d) {
    if (!(in->flags & UINT32_C(0x10000000))) return;
    const float x = a->position[0], y = a->position[1], z = s->pair_z;
    const unsigned frame = in->counter & 31u;
    const nm_xa_op_t first[] = {op_t(x, y, z), op_r(1, 0x8000u)};
    draw(d, frame, 2, first);
    const float r6 = 1.0f + z;   /* command 0 */
    const nm_xa_op_t second[] = {op_t(x, y, z), op_r(1, 0x8000u), op_t(r6, 0.0f, r6)};
    draw(d + 1, frame, 3, second);
    s->pair_z = z - 0.2f;   /* command 1 (0x3e4ccccd) */
}

/* 222288 (and 2223a8). */
static void statue(nm_xa_state_t *s, nm_xa_shared_t *shared, const nm_clock_actor_t *a, const nm_xa_input_t *in,
                   nm_xa_draw_t *d) {
    const int podium = (in->flags & UINT32_C(0x10000000)) && (uint32_t)(in->phase_timer - 1016) <= 0xc3u;
    if (!podium && (uint32_t)((uint32_t)in->player_profile - 242u) > 0x4eu) {   /* 222388: leave */
        shared->inside &= (uint16_t)~1u; s->statue_count = 0;
        return;
    }
    if (!(shared->inside & 1u)) ++shared->entries;
    shared->inside |= 1u;
    if (!in->player_speed_index && in->start_edge) ++s->statue_count;
    nm_xa_op_t ops[3];
    unsigned count = 0;
    ops[count++] = op_t(a->position[0], a->position[1], a->position[2]);
    if (in->flags & UINT32_C(0x400000)) {
        if (in->reverse == 1u) { ops[count++] = op_r(1, 0xe000u); ops[count++] = op_r(0, 0x8000u); }
        else if (!((shared->entries + 1u) & 63u)) { ops[count++] = op_r(1, 0x2000u); ops[count++] = op_r(0, 0x8000u); }
        else if (s->statue_count <= 15u) ops[count++] = op_r(1, (uint32_t)s->statue_count << 12);
        else if (s->statue_count <= 31u) { ops[count++] = op_r(1, (uint32_t)s->statue_count << 12); ops[count++] = op_r(0, 0x8000u); }
    }
    draw(d, 0, count, ops);
}

int nm_xa_tick(nm_xa_state_t *s, nm_xa_shared_t *shared, const void *dna, const nm_drive_params_t *math,
               const nm_contact_pack_t *contact, const nm_xa_input_t *in) {
    if (!s || !shared || !dna || !math || !contact || !in || !in->bitmaps) return 0;
    const nm_clock_actor_t *actors = nm_clock_actors(dna);
    const unsigned count = nm_clock_actor_count(dna);
    for (unsigned g = 0; g < NM_CLOCK_GROUPS_MAX; ++g) s->draw[g].frame = -1;
    if (!s->started) {   /* 2002c and the init entries: no update */
        unsigned horses = 0;
        nm_memset(s->horse, 0, sizeof(s->horse));
        for (unsigned i = 0; i < count; ++i) {
            const nm_clock_actor_t *a = actors + i;
            if (a->kind == NM_CLOCK_BIG_BIRD) s->scale = 26.0f;   /* 224748: 0x41d00000 */
            if (a->kind == NM_CLOCK_PAIR) s->pair_z = a->position[2];
            if (a->kind == NM_CLOCK_STATUE) s->statue_count = 0;  /* 222280 */
            if (a->kind == NM_CLOCK_HORSE) {
                if (horses >= sizeof(s->horse) / sizeof(s->horse[0])) return 0;
                nm_xa_horse_t *h = s->horse + horses++;
                h->x = a->position[0]; h->y = a->position[1]; h->z = a->position[2];
                h->state = NM_XA_HORSE_CIRCLE; h->cell = a->cell;   /* 224854 */
            }
        }
        s->started = 1;
        return 1;
    }
    unsigned horses = 0;
    for (unsigned i = 0; i < count; ++i) {
        const nm_clock_actor_t *a = actors + i;
        nm_xa_draw_t *d = s->draw + a->first_group;
        switch (a->kind) {
        case NM_CLOCK_BEARING: if (!bearing(a, math, in, d)) return 0; break;
        case NM_CLOCK_BIRDS: birds(a, in, d); break;
        case NM_CLOCK_BIG_BIRD: big_bird(s, a, in, d); break;
        case NM_CLOCK_HORSE: if (!horse(s->horse + horses++, a, math, contact, in, d)) return 0; break;
        case NM_CLOCK_PAIR: pair(s, a, in, d); break;
        case NM_CLOCK_STATUE: statue(s, shared, a, in, d); break;
        default: return 0;
        }
    }
    return 1;
}
