#ifndef DAYTONA_NATIVE_COURSE_ASSETS_H
#define DAYTONA_NATIVE_COURSE_ASSETS_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The course a build races until the course select exists (make NATIVE_COURSE): the slot 501460,
 * 0 Beginner, 1 Expert, 2 Advanced, the index of every per-course ROM table. */
#ifndef NM_COURSE
#define NM_COURSE 0
#endif

/* Courses whose world takes the whole prebuilt world stream tables (render/models/model_submission.h NM_SQ_STREAM), a
 * bit per slot (make NATIVE_WORLD_STREAM_COURSES). Expert's world (26,487 faces) would need 2.5 MB of stream records,
 * more than the heap can give with a margin (tools/heap_budget.py --session): it takes the record cache
 * (model_submission.h NM_SQ_STREAM_POOL, make NATIVE_WORLD_STREAM_POOL), the records of the meshes the frames walk;
 * the batches of a mesh without them take the queue path, which submits the same words (tests/stream_sim,
 * SIM_NO_STREAM=1, SIM_STREAM_POOL) for more SH4 time. */
#ifndef NM_COURSE_WORLD_STREAMS
#define NM_COURSE_WORLD_STREAMS 5u
#endif
static inline int nm_course_world_streams(unsigned course) { return course<3 && ((NM_COURSE_WORLD_STREAMS>>course)&1u); }

/* File names of a course's packs in the asset set. Course 0 keeps the names it has always had
 * (scenery.dnv, world.dnm, world.dnt, sky.dns); the others are courseN.dnc, courseN.dnr,
 * courseN-route0..7.dnp, courseN.dnv, courseN-world.dnm, courseN-world.dnt and courseN-sky.dns, and
 * courseN-actors.dna for a world with frame-counter animations.
 * drive.dnd is shared. ROOT with or without a final '/'. */
enum { NM_COURSE_DRIVE, NM_COURSE_CONTACT, NM_COURSE_ROAD, NM_COURSE_ROUTE, NM_COURSE_VISIBILITY,
       NM_COURSE_WORLD, NM_COURSE_WORLD_MATERIALS, NM_COURSE_SKY, NM_COURSE_ACTORS };

static inline int nm_course_asset(char *out,size_t size,const char *root,unsigned course,unsigned kind,unsigned route) {
    if(!out || !size || !root || course>2 || route>7) return 0;
    const size_t length=strlen(root);
    const char *slash=length && root[length-1]=='/' ? "" : "/";
    int n=-1;
    switch(kind) {
    case NM_COURSE_DRIVE: n=snprintf(out,size,"%s%sdrive.dnd",root,slash);break;
    case NM_COURSE_CONTACT: n=snprintf(out,size,"%s%scourse%u.dnc",root,slash,course);break;
    case NM_COURSE_ROAD: n=snprintf(out,size,"%s%scourse%u.dnr",root,slash,course);break;
    case NM_COURSE_ROUTE: n=snprintf(out,size,"%s%scourse%u-route%u.dnp",root,slash,course,route);break;
    case NM_COURSE_VISIBILITY:
        n=course ? snprintf(out,size,"%s%scourse%u.dnv",root,slash,course) : snprintf(out,size,"%s%sscenery.dnv",root,slash);
        break;
    case NM_COURSE_WORLD:
        n=course ? snprintf(out,size,"%s%scourse%u-world.dnm",root,slash,course) : snprintf(out,size,"%s%sworld.dnm",root,slash);
        break;
    case NM_COURSE_WORLD_MATERIALS:
        n=course ? snprintf(out,size,"%s%scourse%u-world.dnt",root,slash,course) : snprintf(out,size,"%s%sworld.dnt",root,slash);
        break;
    case NM_COURSE_SKY:
        n=course ? snprintf(out,size,"%s%scourse%u-sky.dns",root,slash,course) : snprintf(out,size,"%s%ssky.dns",root,slash);
        break;
    case NM_COURSE_ACTORS: /* the frame-counter animations (include/clock_actors.h): Advanced's only so far */
        n=snprintf(out,size,"%s%scourse%u-actors.dna",root,slash,course);
        break;
    default: return 0;
    }
    return n>0 && (size_t)n<size;
}
#endif
