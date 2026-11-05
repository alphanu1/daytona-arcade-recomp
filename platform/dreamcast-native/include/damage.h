#ifndef DAYTONA_NATIVE_DAMAGE_H
#define DAYTONA_NATIVE_DAMAGE_H
#include "nm_mem.h"
#include "contact.h"
#include "yaw.h"

enum { NM_CRASH_SOUND_START = 1, NM_CRASH_SOUND_SMALL = 2, NM_CRASH_SOUND_LARGE = 4 };
/* last_surface/signal/shake_ticks/sequence_counter are shared game state:
 * load/publish them across entities, not independent copies per car. Consume
 * auxiliary_request/sound_requests after each call; they are per-call intents. */
typedef struct {
    /* overlay_model is legacy naming for entity5c raw vertical-offset bits;
     * body_model is the distinct actual model identity at entity14. */
    uint32_t flags, damage, body_model, overlay_model, last_surface;
    int16_t spin_heading, spin_rotation, remaining, inhibit_ticks, angle[3], sound_ticks, signal;
    uint16_t row;
    uint8_t spin_ticks, side, override_count, crash_count, initial_inhibit;
    uint8_t orientation, sequence_counter, auxiliary_request, sound_requests;
    int8_t shake_ticks;
} nm_damage_state_t;
typedef struct {
    uint32_t selected, wheel_surface[4], global_flags;
    int32_t speed_index;
    int16_t heading;
    uint8_t curve, entity_id;
} nm_damage_input_t;

/*15eb8..15fc0 advance of one bounded authored sequence.
 * A call consumes at most two rows.
 * Authored animation, not a recorded vehicle trajectory. Events are intents;
 * no audio device or function pointer is called. Reject without publication. */
static inline int nm_crash_advance_rows(const nm_crash_row_t *rows, unsigned count,
        uint8_t entity_id, nm_damage_state_t *state) {
    if (!rows || !count || count>2048 || !state) return 0;
    nm_damage_state_t s = *state;
    s.auxiliary_request = s.sound_requests = 0;
    for (unsigned step = 0; step < 2; ++step) {
        if (!s.remaining) { s.overlay_model = 0; NM_COPY(state,&s); return 1; }
        if (s.remaining < 0 || (unsigned)s.remaining > count || s.row >= count ||
            (unsigned)s.remaining > count-s.row) return 0;
        --s.remaining;
        if (s.inhibit_ticks) s.inhibit_ticks = nm_yaw_angle((uint32_t)((int)s.inhibit_ticks-1));
        const int limit = entity_id ? 72 : 28;
        if (s.inhibit_ticks == limit) s.override_count = (uint8_t)limit;
        const nm_crash_row_t *row = rows+s.row++;
        s.overlay_model = row->model_key;
        for (unsigned a = 0; a < 3; ++a) {
            int angle = row->angle[a];
            if ((s.orientation&2) && a != 0) angle = -angle;
            if ((s.orientation&1) && a != 2) angle = -angle;
            s.angle[a] = nm_yaw_angle((uint32_t)angle);
        }
        if (!entity_id) {
            int ticks = (int)s.sound_ticks+1;
            if (row->marker > 256) {
                if (ticks >= 40) s.sound_requests |= ticks < 80 ? NM_CRASH_SOUND_SMALL : NM_CRASH_SOUND_LARGE;
                ticks = 0;
            }
            s.sound_ticks = nm_yaw_angle((uint32_t)ticks);
        }
        s.flags |= 1u<<10;
        if ((s.remaining&3) != 3) { NM_COPY(state,&s); return 1; }
    }
    return 0;
}

/* Existing boundary/player callers retain their DND sequence and contract. */
static inline int nm_crash_advance(const nm_drive_params_t *p, uint8_t entity_id,
        nm_damage_state_t *state) {
    return p && nm_crash_advance_rows(p->crash_rows,NM_CRASH_ROWS,entity_id,state);
}

/*15de0 opponent gate/start followed by existing15eb8 row update.
 * Caller selects the retained active rows unless a trigger is permitted to
 *restart; only then select authored variant0 (flag28 or course variant).
 * and shares sequence_counter across cars. Source15c0c is sound-only and is
 * intentionally absent from the silent target, including its sound cooldown.
 * condition is source signed byte d4; inhibit_ticks is signed halfword c8. */
static inline int nm_intro_crash_step(const nm_crash_row_t *rows,unsigned count,
        uint8_t entity_id,int8_t condition,nm_damage_state_t *state) {
    if(!rows || !count || count>2048 || !state) return 0;
    nm_damage_state_t s=*state;
    const unsigned trigger=s.flags&(1u<<6);
    s.flags&=~(1u<<6);
    if(trigger && !condition && !s.inhibit_ticks) {
        s.override_count=63;s.sound_ticks=-9999;
        s.orientation=s.sequence_counter&3;++s.sequence_counter;
        s.remaining=(int16_t)count;s.inhibit_ticks=(int16_t)((int)count-128);
        s.initial_inhibit=(uint8_t)s.inhibit_ticks;s.row=0;
    }
    if(!nm_crash_advance_rows(rows,count,entity_id,&s)) return 0;
    NM_COPY(state,&s);return 1;
}

/*15cc8/15cd4 ->15e54/15eb8, shared by boundary and vehicle collisions.
 * Both source directories select the same offline-verified variant0 rows. */
static inline int nm_crash_begin(const nm_drive_params_t *p, unsigned curve,
        uint8_t entity_id, uint32_t global_flags, nm_damage_state_t *state) {
    if (!p || !state || curve >= 2) return 0;
    nm_damage_state_t s = *state;
    s.damage |= 256; s.body_model = p->crash_model[curve][0];
    ++s.crash_count; s.sound_ticks = 0;
    s.orientation = s.sequence_counter&3; ++s.sequence_counter;
    s.remaining = NM_CRASH_ROWS; s.inhibit_ticks = NM_CRASH_ROWS-128;
    s.initial_inhibit = (uint8_t)s.inhibit_ticks; s.row = 0;
    if (!nm_crash_advance(p,entity_id,&s)) return 0;
    if (!(global_flags&0x1f800000u)) {
        s.auxiliary_request = 1; s.sound_requests |= NM_CRASH_SOUND_START;
    }
    NM_COPY(state,&s);
    return 1;
}

/* Complete16bd4, including15cc8/15cd4 ->15e54 initialization/first advance.
 * Repeated-surface suppression and timer/model writes are gameplay state.
 * Model keys require native catalog binding later; never dereference as ROM. */
static inline int nm_boundary_spin(const nm_drive_params_t *p, const nm_contact_pack_t *pack,
                                   const nm_damage_input_t *in, nm_damage_state_t *state) {
    if (!p || !pack || !in || !state) return 0;
    nm_damage_state_t s = *state;
    s.auxiliary_request = s.sound_requests = 0;
    if (!s.shake_ticks) s.shake_ticks = 24;
    if (s.last_surface == in->selected) { NM_COPY(state,&s); return 1; }
    if (!pack->quads || in->selected >= pack->quad_count || in->curve >= 2) return 0;
    s.last_surface = in->selected;
    unsigned wheel = 0;
    while (wheel < 4 && in->wheel_surface[wheel] != in->selected) ++wheel;
    if (wheel == 4) wheel = 0;
    s.flags = (s.flags&0xfc3fffffu) | (1u<<(25-wheel));
    s.damage |= (1u<<(3-wheel)) | (1u<<(7-wheel));
    s.side = 4; s.signal = 1;
    int angle = nm_yaw_angle((uint32_t)(int32_t)in->heading-pack->quads[in->selected].heading);
    if (angle <= 0) angle += 32768;
    if (angle >= 16384) angle -= 32768;
    s.spin_rotation = (int16_t)(angle + (angle < 0 ? -((-angle+3)/4) : angle/4));
    s.spin_heading = nm_yaw_angle((uint32_t)((int)in->heading-s.spin_rotation));
    s.spin_ticks = s.override_count = 32;
    if (!(s.damage&256)) s.body_model = p->crash_model[in->curve][s.damage&15];
    if (!s.inhibit_ticks &&
        (in->speed_index >= p->crash_threshold[in->curve][0][s.crash_count&15] ||
         in->speed_index >= p->crash_threshold[in->curve][1][s.crash_count&15])) {
        if (!nm_crash_begin(p,in->curve,in->entity_id,in->global_flags,&s)) return 0;
    }
    NM_COPY(state,&s);
    return 1;
}

typedef struct {
    nm_damage_state_t damage;
    uint32_t camera_words[6], auxiliary_flags, control;
    int32_t camera_mode;
    int16_t camera_angles[3], heading, decay;
    uint16_t path_offset, pivot_signal;
    uint8_t countdown, camera_transition;
} nm_damage_pre_state_t;
typedef struct {
    uint32_t global_flags, path_flags;
    int16_t phase, yaw;
    int8_t restored_camera_mode; /* Live selected-view descriptor byte, not a ROM pointer. */
    uint8_t variant, entity_id;
} nm_damage_pre_input_t;

/*16014 ->15d60 (including637c), also the intro leader's d25c/d260 prefix.
 * Camera/shared fields are caller-owned shared state, not copies per car.
 * One tick, atomic publication, no frame replay or board scheduler. Only the
 * DND14 boundary-crash sequence is supported, not15de0's other entry choices. */
static inline int nm_damage_intro_step(const nm_drive_params_t *p,
        const nm_damage_pre_input_t *in, nm_damage_pre_state_t *state) {
    if (!p || !in || !state) return 0;
    nm_damage_pre_state_t s; NM_COPY(&s,state);
    s.damage.auxiliary_request = s.damage.sound_requests = 0;
    const int delta = s.decay < -256 ? -256 : s.decay > 256 ? 256 : s.decay;
    s.decay = (int16_t)(s.decay-delta);
    if (s.countdown) --s.countdown; // Byte store wraps, including signed-negative inputs.
    if (s.damage.remaining) {
        s.control = 224;
        if (s.damage.remaining == 1 && !(in->global_flags&0x1f800000u)) {
            s.auxiliary_flags &= 0x7fffffffu;
            s.camera_mode = in->restored_camera_mode; s.camera_transition = 0;
            nm_memcpy(s.camera_words,p->camera_reset_words,sizeof(s.camera_words));
            nm_memcpy(s.camera_angles,p->camera_reset_angles,sizeof(s.camera_angles));
        }
        if (s.damage.inhibit_ticks == 1) {
            // The source's unaligned word store also clears the spin signal.
            s.pivot_signal = in->variant ? 42 : 41; s.damage.signal = 0;
            if (in->path_flags&1) {
                s.path_offset = (uint16_t)((int)in->phase*28); s.heading = in->yaw;
            }
        }
        if (!nm_crash_advance(p,in->entity_id,&s.damage)) return 0;
    }
    NM_COPY(state,&s);
    return 1;
}
/* Active-player d888..d894 adds15fc4 before the shared intro prefix.
 * Keep the whole three-call update atomic if the shared tail rejects. */
static inline int nm_damage_pre_step(const nm_drive_params_t *p,
        const nm_damage_pre_input_t *in, nm_damage_pre_state_t *state) {
    if (!p || !in || !state) return 0;
    nm_damage_pre_state_t s; NM_COPY(&s,state);
    //15fc4 compares the signed OLD byte before storing old-1.
    if (s.damage.spin_ticks) {
        if (s.damage.spin_ticks > 1 && s.damage.spin_ticks < 128) --s.damage.spin_ticks;
        else {
            s.damage.spin_ticks = 0; s.damage.last_surface = UINT32_MAX;
            if (in->path_flags&1) {
                s.path_offset = (uint16_t)((int)in->phase*28); s.heading = in->yaw;
            }
        }
    }
    if(!nm_damage_intro_step(p,in,&s)) return 0;
    NM_COPY(state,&s);return 1;
}
#endif
