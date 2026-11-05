#ifndef DAYTONA_NATIVE_NAME_ENTRY_H
#define DAYTONA_NATIVE_NAME_ENTRY_H
#include <stdint.h>

/* Original227764/227820: steering selection and accelerator-edge acceptance.
 * No rendering, guest pointers, audio playback or backup writes. '}' is back,
 * '{' is finish. Three letters restrict steering to those last two choices.
 * The owner must consume a finish request through the separate227934 commit
 * operation; this is not a replacement for its special-name rules. */
typedef struct {
    uint32_t index, previous, count;
    uint8_t name[4], glyph;
} nm_name_entry_t;

static inline int nm_name_select(nm_name_entry_t *s,int8_t steering,uint32_t *command) {
    if(!s || !command || s->index>28 || s->count>3) return 0;
    /* SHRO is unsigned: steering=-128 produces0x1fffffff, then clamps to28.
     * SH4ZAM float clamp is not an equivalent integer/shift operation. */
    uint32_t index=(uint32_t)((int32_t)steering+127)>>3;
    if(index>28) index=28;
    if(s->count==3) {
        index=27+((uint32_t)((int32_t)steering+150)>>7);
        if(index>28) index=28;
    }
    s->previous=s->index;s->index=index;
    *command=index!=s->previous ? UINT32_C(0xae2155) : 0;
    s->glyph=index<26 ? (uint8_t)('A'+index) : index==26 ? '.' : index==27 ? '}' : '{';
    return 1;
}

/* Returns0 invalid,1 continue,2 finish requested. Caller records the silent
 * command. No-edge leaves state unchanged; commands are one-shot outputs. */
static inline int nm_name_accept(nm_name_entry_t *s,uint32_t edge,uint32_t *command) {
    if(!s || !command || s->index>28 || s->count>3 ||
       !((s->glyph>='A' && s->glyph<='Z') || s->glyph=='.' || s->glyph=='}' || s->glyph=='{') ||
       (s->count==3 && s->glyph!='}' && s->glyph!='{')) return 0;
    *command=0;
    if(!edge) return 1;
    if(s->glyph=='{') return 2;
    if(s->glyph=='}') {
        if(s->count) { s->name[--s->count]=0;*command=UINT32_C(0xae2161); }
        else for(unsigned i=0;i<4;++i) s->name[i]=0;
    } else {
        s->name[s->count++]=s->glyph;
        *command=s->glyph=='.' ? UINT32_C(0xae2162) : UINT32_C(0xae2139)+(s->glyph-'A');
        if(s->count==3) s->index=28; /* glyph updates on NEXT steering sample. */
    }
    return 1;
}

/* Offline-expanded227b1c tables. Names/replacements contain three low bytes;
 * byte3 is inherited, including when the selected record is published. */
typedef struct { uint32_t name, timer, command; } nm_name_rule_t;
typedef struct {
    nm_name_rule_t rules[40];
    uint32_t replacements[2][4], default_command;
} nm_name_rules_t;
typedef struct {
    uint8_t display[7], phase;
    uint32_t timer, command;
} nm_name_commit_t;

/* Entire227934+227b1c: substitution before display/record publication, then
 * actor phase4/6 and silent command. Record pointer is a native selected
 * record's name field, never a guest pointer or persistent-storage handle. */
static inline int nm_name_commit(nm_name_entry_t *s,const nm_name_rules_t *rules,
        uint32_t frame,uint32_t notification,uint32_t *record_name,nm_name_commit_t *out) {
    if(!s || !rules || !out || ((notification&1u) && !record_name)) return 0;
    uint32_t name=(uint32_t)s->name[0]|((uint32_t)s->name[1]<<8)|((uint32_t)s->name[2]<<16);
    unsigned index=0;
    while(index<40 && rules->rules[index].name!=name) ++index;
    out->timer=index<40 ? rules->rules[index].timer : 192;
    out->command=index<40 ? rules->rules[index].command : rules->default_command;
    out->phase=index<2 ? 6 : 4;
    if(index<2) name=rules->replacements[index][frame&3u];
    for(unsigned i=0;i<3;++i) {
        s->name[i]=(uint8_t)(name>>(8*i));
        out->display[1+2*i]=s->name[i];out->display[2+2*i]=0;
    }
    if(notification&1u) *record_name=(name&UINT32_C(0xffffff))|((uint32_t)s->name[3]<<24);
    return 1;
}
#endif
