// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer (HLE) and rasterizer front end, transplanted from
// MAME's src/mame/sega/model2_v.cpp at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notice above kept as the licence requires). The code below
// is MAME's with these changes only: raw pointers are bounds-checked cursors
// (GeoPtr, GeoPtr16); kept polygons go to a flat list (Geo::polys) instead of
// MAME's per-z buckets; logerror is dropped and fatalerror throws. See
// src/runtime/geo.h and THIRD_PARTY.md.

#include "runtime/geo.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace rt {

namespace {
inline float u2f(uint32_t v) { return std::bit_cast<float>(v); }
inline uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }
using u8 = uint8_t;
using u64 = uint64_t;
} // namespace

#define pz      p[0]
#define pu      p[1]
#define pv      p[2]

struct quad_m2 {
    GeoVertex v[4];
    uint16_t z = 0;
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
};

Geo::Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer)
    : polygon_rom_(polygons.size() / 4), texture_rom_(textures.size() / 2), buffer_(buffer) {
    if (polygon_rom_.empty() || (polygon_rom_.size() & (polygon_rom_.size() - 1)) || texture_rom_.empty() ||
        (texture_rom_.size() & (texture_rom_.size() - 1)))
        throw GeoFatal("bad polygon or texture ROM image");
    std::memcpy(polygon_rom_.data(), polygons.data(), polygon_rom_.size() * 4);
    std::memcpy(texture_rom_.data(), textures.data(), texture_rom_.size() * 2);
    raster_.texture_rom = texture_rom_.data();
    raster_.texture_rom_mask = uint32_t(texture_rom_.size() - 1);
    geo_.raster = &raster_;
    geo_.polygon_rom = polygon_rom_.data();
    geo_.polygon_rom_mask = uint32_t(polygon_rom_.size() - 1);
}

static inline void transform_point(GeoVertex *point, float *matrix)
{
	float tx = (point->x * matrix[0]) + (point->y * matrix[3]) + (point->pz * matrix[6]) + (matrix[9]);
	float ty = (point->x * matrix[1]) + (point->y * matrix[4]) + (point->pz * matrix[7]) + (matrix[10]);
	float tz = (point->x * matrix[2]) + (point->y * matrix[5]) + (point->pz * matrix[8]) + (matrix[11]);

	point->x = tx;
	point->y = ty;
	point->pz = tz;
}

static inline void transform_vector(GeoVertex *vector, float *matrix)
{
	float tx = (vector->x * matrix[0]) + (vector->y * matrix[3]) + (vector->pz * matrix[6]);
	float ty = (vector->x * matrix[1]) + (vector->y * matrix[4]) + (vector->pz * matrix[7]);
	float tz = (vector->x * matrix[2]) + (vector->y * matrix[5]) + (vector->pz * matrix[8]);

	vector->x = tx;
	vector->y = ty;
	vector->pz = tz;
}

static inline void normalize_vector(GeoVertex *vector)
{
	const float n = sqrt((vector->x * vector->x) + (vector->y * vector->y) + (vector->pz * vector->pz));

	if (n)
	{
		float oon = 1.0f / n;
		vector->x *= oon;
		vector->y *= oon;
		vector->pz *= oon;
	}
}

static inline float dot_product(const GeoVertex &v1, const GeoVertex &v2)
{
	return (v1.x * v2.x) + (v1.y * v2.y) + (v1.pz * v2.pz);
}

static inline void vector_cross3(GeoVertex *dst, const GeoVertex *v0, const GeoVertex *v1, const GeoVertex *v2)
{
	GeoVertex p1, p2;

	p1.x = v1->x - v0->x;   p1.y = v1->y - v0->y;   p1.pz = v1->pz - v0->pz;
	p2.x = v2->x - v0->x;   p2.y = v2->y - v0->y;   p2.pz = v2->pz - v0->pz;

	dst->x = (p1.y * p2.pz) - (p1.pz * p2.y);
	dst->y = (p1.pz * p2.x) - (p1.x * p2.pz);
	dst->pz = (p1.x * p2.y) - (p1.y * p2.x);
}

static inline void apply_focus(Geo::geo_state *geo, GeoVertex *p0)
{
	p0->x *= geo->focus.x;
	p0->y *= geo->focus.y;
}


/* 1.8.23 float to 4.12 float converter, courtesy of Aaron Giles */
inline uint16_t Geo::float_to_zval(float floatval, int32_t z_adjust)
{
	int32_t fpint = f2u(floatval);
	int32_t exponent = ((fpint >> 23) & 0xff) - ((z_adjust >> 23) & 0xff);
	uint32_t mantissa = fpint & 0x7fffff;

	/* round the low bits and reduce to 12 */
	mantissa += 0x400;
	if (mantissa > 0x7fffff)
	{
		exponent++;
		mantissa = (mantissa & 0x7fffff) >> 1;
	}
	mantissa >>= 11;

	// if negative, clamp to 0
	if (fpint < 0)
		return 0x0000;

	// the rest depends on the exponent
	if (exponent < -12)
		return 0x0000; // less than -12 is too small, return 0
	else if (exponent < 0)
		return (mantissa | 0x1000) >> -exponent; // between -12 and 0 create a denormal with exponent of 0
	else if (exponent < 15)
		return ((exponent + 1) << 12) | mantissa; // between 0 and 14 create a FP value with exponent + 1
	else
		return 0xffff; // above 14 is too large
}

static int32_t clip_polygon(GeoVertex *v, int32_t num_vertices, GeoVertex *vout, Geo::plane clip_plane)
{
	int32_t outcount = 0;

	const GeoVertex *cur = v;
	GeoVertex *out = vout;

	float curdot = dot_product(*cur, clip_plane.normal);
	int32_t curin = (curdot >= clip_plane.distance) ? 1 : 0;

	for (int32_t i = 0; i < num_vertices; i++)
	{
		const int32_t nextvert = (i + 1) % num_vertices;

		/* if the current point is inside the plane, add it */
		if (curin)
			out[outcount++] = *cur;

		const float nextdot = dot_product(v[nextvert], clip_plane.normal);
		const int32_t nextin = (nextdot >= clip_plane.distance) ? 1 : 0;

		/* Add a clipped vertex if one end of the current edge is inside the plane and the other is outside */
		if ((curin != nextin) && !std::isnan(curdot) && !std::isnan(nextdot))
		{
			const float scale = (clip_plane.distance - curdot) / (nextdot - curdot);

			out[outcount].x = cur->x + ((v[nextvert].x - cur->x) * scale);
			out[outcount].y = cur->y + ((v[nextvert].y - cur->y) * scale);
			out[outcount].pz = cur->pz + ((v[nextvert].pz - cur->pz) * scale);
			out[outcount].pu = cur->pu + ((v[nextvert].pu - cur->pu) * scale);
			out[outcount].pv = cur->pv + ((v[nextvert].pv - cur->pv) * scale);
			outcount++;
		}

		curdot = nextdot;
		curin = nextin;
		cur++;
	}

	return outcount;
}

inline bool Geo::check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z)
{
	/* if doubleside is disabled */
	if (((attr >> 17) & 1) == 0)
	{
		/* if it's the backface, cull it */
		if (raster->command_buffer[9] & 0x00800000)
			return true;
	}

	/* if the linktype is 0, then we can also cull it */
	if (((attr >> 8) & 3) == 0)
		return true;

	/* if the minimum z value is bigger than the master z clip value, don't render */
	if (raster->master_z_clip != 0xff && (int32_t)(1.0 / min_z) > raster->master_z_clip)
		return true;

	/* if the maximum z value is < 0 then we can safely clip the entire polygon */
	if (max_z < 0)
		return true;

	return false;
}

template <unsigned NumVerts>
void Geo::model2_3d_process_polygon(raster_state *raster, uint32_t attr)
{
	quad_m2 object;
	GeoPtr16 th, tp;
	int32_t tho;
	uint32_t i;
	bool cull;
	float zvalue;
	float min_z, max_z;

	static_assert(NumVerts == 3 || NumVerts == 4, "Polygon must have 3 or 4 vertices");

	/* extract P0(n-1) */
	object.v[1].x = u2f(raster->command_buffer[2] << 8);
	object.v[1].y = u2f(raster->command_buffer[3] << 8);
	object.v[1].pz = u2f(raster->command_buffer[4] << 8);

	/* extract P1(n-1) */
	object.v[0].x = u2f(raster->command_buffer[5] << 8);
	object.v[0].y = u2f(raster->command_buffer[6] << 8);
	object.v[0].pz = u2f(raster->command_buffer[7] << 8);

	/* extract P0(n) */
	object.v[2].x = u2f(raster->command_buffer[11] << 8);
	object.v[2].y = u2f(raster->command_buffer[12] << 8);
	object.v[2].pz = u2f(raster->command_buffer[13] << 8);

	if (NumVerts == 4)
	{
		/* extract P1(n) */
		object.v[3].x = u2f(raster->command_buffer[14] << 8);
		object.v[3].y = u2f(raster->command_buffer[15] << 8);
		object.v[3].pz = u2f(raster->command_buffer[16] << 8);
	}
	else
	{
		/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
		raster->command_buffer[14] = raster->command_buffer[11];
		raster->command_buffer[15] = raster->command_buffer[12];
		raster->command_buffer[16] = raster->command_buffer[13];
	}

	/* always calculate the min z and max z value */
	min_z = object.v[0].pz;
	if (object.v[1].pz < min_z) min_z = object.v[1].pz;
	if (object.v[2].pz < min_z) min_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz < min_z) min_z = object.v[3].pz;

	max_z = object.v[0].pz;
	if (object.v[1].pz > max_z) max_z = object.v[1].pz;
	if (object.v[2].pz > max_z) max_z = object.v[2].pz;
	if (NumVerts == 4 && object.v[3].pz > max_z) max_z = object.v[3].pz;

	/* read in the texture information */

	/* texture point data */
	if (raster->command_buffer[0] & 0x800000)
		tp = GeoPtr16{raster->texture_ram, 0x10000, raster->command_buffer[0] & 0xffff};
	else
		tp = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1, raster->command_buffer[0] & raster->texture_rom_mask};

	object.v[0].pv = *tp++;
	object.v[0].pu = *tp++;
	object.v[1].pv = *tp++;
	object.v[1].pu = *tp++;
	object.v[2].pv = *tp++;
	object.v[2].pu = *tp++;
	if (NumVerts == 4)
	{
		object.v[3].pv = *tp++;
		object.v[3].pu = *tp++;
	}

	/* update the address */
	raster->command_buffer[0] += NumVerts * 2;

	/* texture header data */
	if (raster->command_buffer[1] & 0x800000)
		th = GeoPtr16{raster->texture_ram, 0x10000, raster->command_buffer[1] & 0xffff};
	else
		th = GeoPtr16{raster->texture_rom, raster->texture_rom_mask + 1, raster->command_buffer[1] & raster->texture_rom_mask};

	object.texheader[0] = *th++;
	object.texheader[1] = *th++;
	object.texheader[2] = *th++;
	object.texheader[3] = *th++;

	/* extract the texture header offset */
	tho = (attr >> 12) & 0x1f;

	/* adjust for sign */
	if (tho & 0x10)
		tho |= -16;

	/* update the address */
	raster->command_buffer[1] += tho * 4;

	/* set the luma value of this polygon */
	object.luma = (raster->command_buffer[9] >> 15) & 0xff;

	/* set the texture LOD of this polygon */
	object.texlod = ((raster->command_buffer[10] >> 8) & 0x7f80) - 0x3f80;
	object.texlod += raster->log_ram[raster->command_buffer[10] & 0x7fff];

	/* determine whether we can cull this polygon */
	cull = check_culling(raster,attr,min_z,max_z);

	/* set the object's z value */
	switch ((attr >> 10) & 3)
	{
		case 0: // old value
			zvalue = raster->polygon_z;
			break;
		case 1: // min z
			zvalue = min_z;
			break;
		case 2: // max z
			zvalue = max_z;
			break;
		case 3: // error
		default:
			zvalue = 1e10;
			break;
	}

	raster->polygon_z = zvalue;

	if (cull == false)
	{
		int32_t clipped_verts;
		GeoVertex verts_in[8], verts_out[8];

		for (int i = 0; i < NumVerts; i++)
			verts_in[i] = object.v[i];

		clipped_verts = NumVerts;

		/* do clipping */
		for (int i = 0; i < 4; i++)
		{
			clipped_verts = clip_polygon(verts_in, clipped_verts, verts_out, raster->clip_plane[raster->center_sel][i]);
			for (int j = 0; j < clipped_verts; j++)
				verts_in[j] = verts_out[j];
		}

		if (clipped_verts > 2)
		{
			/* adjust and set the object z-sort value */
			object.z = float_to_zval(zvalue, raster->z_adjust);

			/* go through the clipped vertex list, adding polygons */
			raster->poly_list_index++;
			if (raster->poly_list_index >= 32768) // MAME MAX_POLYGONS
				throw GeoFatal("SEGA 3D: Max polygon limit exceeded");
			polys.emplace_back();
			GeoPoly *poly = &polys.back();
			poly->reverse = raster->reverse;

			/* copy the object information */
			poly->z = object.z;
			poly->texheader[0] = object.texheader[0];
			poly->texheader[1] = object.texheader[1];
			poly->texheader[2] = object.texheader[2];
			poly->texheader[3] = object.texheader[3];
			poly->luma = object.luma;
			poly->texlod = object.texlod;

			/* set the viewport */
			poly->viewport[0] = raster->viewport[0];
			poly->viewport[1] = raster->viewport[1];
			poly->viewport[2] = raster->viewport[2];
			poly->viewport[3] = raster->viewport[3];

			/* set the center */
			poly->center[0] = raster->center[raster->center_sel][0];
			poly->center[1] = raster->center[raster->center_sel][1];

			/* set the window */
			poly->window = raster->cur_window;

			poly->num_vertices = clipped_verts;

			for (int i = 0; i < clipped_verts; i++)
				poly->v[i] = verts_out[i];

			// (MAME links it into a per-z bucket here; the display list keeps
			// the order added, from which the bucket order follows.)

			/* keep around the min and max z values for this frame */
			if (object.z < raster->min_z) raster->min_z = object.z;
			if (object.z > raster->max_z) raster->max_z = object.z;
		}
	}

	/* update linking */
	switch (((attr >> 8) & 3))
	{
		case 0:
		case 2:
		{
			/* reuse P0(n) and P1(n) */
			for (i = 0; i < 6; i++)                                        /* P0(n) -> P0(n-1) */
				raster->command_buffer[2+i] = raster->command_buffer[11+i]; /* P1(n) -> P1(n-1) */
		}
		break;

		case 1:
		{
			/* reuse P0(n-1) and P0(n) */
			for (i = 0; i < 3; i++)
				raster->command_buffer[5+i] = raster->command_buffer[11+i]; /* P0(n) -> P1(n-1) */
		}
		break;

		case 3:
		{
			/* reuse P1(n-1) and P1(n) */
			for (i = 0; i < 3; i++)
				raster->command_buffer[2+i] = raster->command_buffer[14+i]; /* P1(n) -> P1(n-1) */
		}
		break;
	}
}

/***********************************************************************************************/


/* 3D Rasterizer frame start: Resets frame variables */
void Geo::render_frame_start()
{
	raster_state *raster = &raster_;

	/* reset the polygon list index */
	raster->poly_list_index = 0;
	polys.clear();
	pushed.clear();

	/* reset the min-max sortable Z values */
	raster->min_z = 0xffff;
	raster->max_z = 0;

	/* reset the polygon z value */
	// Zero Gunner sets backgrounds with "previous z value" mode at the start of the display list,
	// needs this to be this big in order to work properly
	raster->polygon_z = 1e10;

	raster->cur_window = 0;
}

/* 3D Rasterizer main data input port */
void Geo::model2_3d_push(raster_state *raster, uint32_t input)
{
	if (record_pushes) pushed.push_back(input);
	/* see if we have a command in progress */
	if (raster->cur_command != 0)
	{
		raster->command_buffer[raster->command_index++] = input;

		switch (raster->cur_command)
		{
			case 0x00:  /* NOP */
			break;

			case 0x01:  /* Polygon Data */
			{
				uint32_t  attr;

				/* start by looking if we have the basic input data */
				if (raster->command_index < 9)
					return;

				/* get the attributes */
				attr = raster->command_buffer[8];

				/* see if we're done */
				if ((attr & 3) == 0)
				{
					raster->cur_command = 0;
					return;
				}

				/* see if it's a quad or a triangle */
				if (attr & 1)
				{
					/* it's a quad, wait for the rest of the points */
					if (raster->command_index < 17)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<4>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
				else
				{
					/* it's a triangle, wait for the rest of the point */
					if (raster->command_index < 14)
						return;

					/* we have a full polygon info, fill up our polygon structure */
					model2_3d_process_polygon<3>(raster, attr);

					/* back up and wait for more data */
					raster->command_index = 8;
				}
			}
			break;

			case 0x03:  /* Window Data */
			{
				uint32_t  i;

				/* make sure we have all the data */
				if (raster->command_index < 6)
					return;

				/* coordinates are 12 bit signed */

				/* extract the viewport start x */
				raster->viewport[0] = (raster->command_buffer[0] >> 12) & 0xfff;

				if (raster->viewport[0] & 0x800)
					raster->viewport[0] = -(0x800 - (raster->viewport[0] & 0x7ff));

				/* extract the viewport start y */
				raster->viewport[1] = raster->command_buffer[0] & 0xfff;

				if (raster->viewport[1] & 0x800)
					raster->viewport[1] = -(0x800 - (raster->viewport[1] & 0x7ff));

				/* extract the viewport end x */
				raster->viewport[2] = (raster->command_buffer[1] >> 12) & 0xfff;

				if (raster->viewport[2] & 0x800)
					raster->viewport[2] = -(0x800 - (raster->viewport[2] & 0x7ff));

				/* extract the viewport end y */
				raster->viewport[3] = raster->command_buffer[1] & 0xfff;

				if (raster->viewport[3] & 0x800)
					raster->viewport[3] = -(0x800 - (raster->viewport[3] & 0x7ff));

				/* extract the centers */
				for (i = 0; i < 4; i++)
				{
					/* center x */
					raster->center[i][0] = (raster->command_buffer[2+i] >> 12) & 0xfff;

					if (raster->center[i][0] & 0x800)
						raster->center[i][0] = -(0x800 - (raster->center[i][0] & 0x7ff));

					/* center y */
					raster->center[i][1] = raster->command_buffer[2+i] & 0xfff;

					if (raster->center[i][1] & 0x800)
						raster->center[i][1] = -(0x800 - (raster->center[i][1] & 0x7ff));

					// calculate clipping planes
					float left_plane = float(raster->center[i][0] - raster->viewport[0]);
					float right_plane = float(raster->viewport[2] - raster->center[i][0]);
					float top_plane = float(raster->viewport[3] - raster->center[i][1]);
					float bottom_plane = float(raster->center[i][1] - raster->viewport[1]);

					raster->clip_plane[i][0].normal.x = 1.0f / std::hypot(1.0f, left_plane);
					raster->clip_plane[i][0].normal.y = 0.0f;
					raster->clip_plane[i][0].normal.pz = left_plane / std::hypot(1.0f, left_plane);

					raster->clip_plane[i][1].normal.x = -1.0f / std::hypot(-1.0f, right_plane);
					raster->clip_plane[i][1].normal.y = 0.0f;
					raster->clip_plane[i][1].normal.pz = right_plane / std::hypot(-1.0f, right_plane);

					raster->clip_plane[i][2].normal.x = 0.0f;
					raster->clip_plane[i][2].normal.y = -1.0f / std::hypot(-1.0f, top_plane);
					raster->clip_plane[i][2].normal.pz = top_plane / std::hypot(-1.0f, top_plane);

					raster->clip_plane[i][3].normal.x = 0.0f;
					raster->clip_plane[i][3].normal.y = 1.0f / std::hypot(1.0f, bottom_plane);
					raster->clip_plane[i][3].normal.pz = bottom_plane / std::hypot(1.0f, bottom_plane);
				}

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			case 0x04:  /* Texture/Log Data write */
			{
				/* make sure we have enough data */
				if (raster->command_index < 2)
					return;

				/* see if the count is non-zero */
				if (raster->command_buffer[1] > 0)
				{
					/* see if we have data available */
					if (raster->command_index >= 3)
					{
						/* get the address */
						uint32_t  address = raster->command_buffer[0];

						/* do the write */
						if (address & 0x800000)
							raster->texture_ram[address & 0xffff] = raster->command_buffer[2];
						else
							raster->log_ram[address & 0x7fff] = raster->command_buffer[2];

						/* increment the address and decrease the count */
						raster->command_buffer[0]++;
						raster->command_buffer[1]--;

						/* decrease the index, so we keep placing data in the same slot */
						raster->command_index--;
					}
				}

				/* see if we're done with this command */
				if (raster->command_buffer[1] == 0)
					raster->cur_command = 0;
			}
			break;

			case 0x08:  /* ZSort mode */
			{
				/* save the zsort mode value */
				raster->z_adjust = raster->command_buffer[0] << 8;

				/* done with this command */
				raster->cur_command = 0;
			}
			break;

			default:
			{
				throw GeoFatal("SEGA 3D: Unknown rasterizer command");
			}
		}
	}
	else
	{
		/* new command */
		raster->cur_command = input & 0x0f;
		raster->command_index = 0;

		/* see if it's object data */
		if (raster->cur_command == 1)
		{
			/* extract reverse bit */
			raster->reverse = (input >> 4) & 1;

			/* extract center select */
			raster->center_sel = (input >> 6) & 3;
		}
	}
}

/***********************************************************************************************/


/* Parse Polygons: Normals Present, No Specular case */
void Geo::geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);

		/* transform with the current matrix */
		transform_vector(&normal, geo->matrix);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			int32_t               luma;
			texture_parameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: Normals Present, Specular case */
void Geo::geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		/* read in the normal */
		normal.x = u2f(*input++);
		normal.y = u2f(*input++);
		normal.pz = u2f(*input++);

		/* transform with the current matrix */
		transform_vector(&normal, geo->matrix);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else /* triangle */
			{
				/* skip the next 3 points */
				input += 3;
			}
		}
		else /* we're done */
		{
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, No Specular case */
void Geo::geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal, p0, p1, p2, p3;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* Skip normal */
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			luminance = (luminance * texparam->diffuse) + texparam->ambient;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/* Parse Polygons: No Normals, Specular case */
void Geo::geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count)
{
	raster_state *raster = geo->raster;
	GeoVertex point, normal, p0, p1, p2, p3;
	uint32_t  attr, i;

	/* read the 1st point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p0.x = point.x; p0.y = point.y; p0.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* read the 2nd point */
	point.x = u2f(*input++);
	point.y = u2f(*input++);
	point.pz = u2f(*input++);

	/* transform with the current matrix */
	transform_point(&point, geo->matrix);

	/* save for normal calculation */
	p1.x = point.x; p1.y = point.y; p1.pz = point.pz;

	/* apply focus */
	apply_focus(geo, &point);

	/* push it to the 3d rasterizer */
	model2_3d_push(raster, f2u(point.x) >> 8);
	model2_3d_push(raster, f2u(point.y) >> 8);
	model2_3d_push(raster, f2u(point.pz) >> 8);

	/* loop through the following links */
	for (i = 0; i < count; i++)
	{
		/* read in the attributes */
		attr = *input++;

		/* push to the 3d rasterizer */
		model2_3d_push(raster, attr & 0x0003ffff);

		if ((attr & 3) != 0) /* quad or triangle */
		{
			float               dotl, dotp, luminance, distance, specular;
			float               coef, face;
			int32_t             luma;
			texture_parameter * texparam;

			/* Skip normal */
			input += 3;

			/* read in the next point */
			point.x = u2f(*input++);
			point.y = u2f(*input++);
			point.pz = u2f(*input++);

			/* transform with the current matrix */
			transform_point(&point, geo->matrix);

			/* save for normal calculation */
			p2.x = point.x; p2.y = point.y; p2.pz = point.pz;

			/* compute the normal */
			vector_cross3(&normal, &p0, &p1, &p2);

			/* normalize it */
			normalize_vector(&normal);

			/* calculate the dot product of the normal and the light vector */
			dotl = dot_product(normal, geo->light);

			/* calculate the dot product of the normal and the point */
			dotp = dot_product(normal, point);

			/* apply focus */
			apply_focus(geo, &point);

			/* determine whether this is the front or the back of the polygon */
			face = 0x100; /* rear */
			if (dotp >= 0) face = 0; /* front */

			/* get the texture parameters */
			texparam = &geo->texture_parameters[(attr>>18) & 0x1f];

			/* calculate luminance and specular */
			if ((dotl * dotp) < 0) luminance = 0;
			else luminance = fabs(dotl);

			specular = ((2*dotl) * normal.pz) - geo->light.pz;
			if (specular < 0) specular = 0;
			if (texparam->specular_control == 0) specular = 0;
			if ((texparam->specular_control >> 1) != 0) specular *= specular;
			if ((texparam->specular_control >> 2) != 0) specular *= specular;
			if (((texparam->specular_control+1) >> 3) != 0) specular *= specular;

			specular *= texparam->specular_scale;

			luminance = (luminance * texparam->diffuse) + texparam->ambient + specular;
			luminance = std::clamp(luminance, 0.0f, 255.0f);

			luma = (int32_t)luminance;

			/* add the face bit to the luma */
			luma += face;

			/* extract distance coefficient */
			coef = geo->coef_table[attr>>27];

			/* calculate texture level of detail */
			distance = coef * fabs(dotp) * geo->lod;

			/* push to the 3d rasterizer */
			model2_3d_push(raster, luma << 15);
			model2_3d_push(raster, f2u(distance) >> 8);
			model2_3d_push(raster, f2u(point.x) >> 8);
			model2_3d_push(raster, f2u(point.y) >> 8);
			model2_3d_push(raster, f2u(point.pz) >> 8);

			/* if it's a quad, push one more point */
			if (attr & 1)
			{
				/* read in the next point */
				point.x = u2f(*input++);
				point.y = u2f(*input++);
				point.pz = u2f(*input++);

				/* transform with the current matrix */
				transform_point(&point, geo->matrix);

				/* save for normal calculation */
				p3.x = point.x; p3.y = point.y; p3.pz = point.pz;

				/* apply focus */
				apply_focus(geo, &point);

				/* push to the 3d rasterizer */
				model2_3d_push(raster, f2u(point.x) >> 8);
				model2_3d_push(raster, f2u(point.y) >> 8);
				model2_3d_push(raster, f2u(point.pz) >> 8);
			}
			else
			{
				/* skip the next 3 points */
				input += 3;

				/* for triangles, the rope of P1(n) is achieved by P0(n-1) (linktype 3) */
				p3.x = p2.x; p3.y = p2.y; p3.pz = p2.pz;
			}
		}
		else /* we're done */
		{
			break;
		}

		/* link type */
		switch ((attr>>8) & 3)
		{
			case 0:
			case 2:
			{
				/* reuse P0(n) and P1(n) */
				p0.x = p2.x; p0.y = p2.y; p0.pz = p2.pz;
				p1.x = p3.x; p1.y = p3.y; p1.pz = p3.pz;
			}
			break;

			case 1:
			{
				/* reuse P0(n-1) and P0(n) */
				p1.x = p2.x; p1.y = p2.y; p1.pz = p2.pz;
			}
			break;

			case 3:
			{
				/* reuse P1(n-1) and P1(n) */
				p0.x = p3.x; p0.y = p3.y; p0.pz = p3.pz;
			}
			break;
		}
	}

	/* notify the 3d rasterizer we're done */
	model2_3d_push(raster, 0);
}

/*******************************************
 *
 *  Geometry Engine Commands
 *
 *******************************************/

/* Command 00: NOP */
GeoPtr Geo::geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* push the opcode to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);

	return input;
}

/* Command 01: Object Data */
GeoPtr Geo::geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  tpa = *input++;     /* Texture Point Address */
	uint32_t  tha = *input++;     /* Texture Header Address */
	uint32_t  oba = *input++;     /* Object Address */
	uint32_t  obc = *input++;     /* Object Count */

	GeoPtr obp;                /* Object Pointer */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, opcode >> 23);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* select where we're reading polygon information from */
	if (oba & 0x01000000)
	{
		/* Fast polygon RAM */
		obp = GeoPtr{geo->polygon_ram1, 0x8000, oba & 0x7fff};
	}
	else if (oba & 0x00800000)
	{
		/* Polygon ROM */
		obp = GeoPtr{geo->polygon_rom, geo->polygon_rom_mask + 1, oba & geo->polygon_rom_mask};
	}
	else
	{
		/* Slow Polygon RAM */
		obp = GeoPtr{geo->polygon_ram0, 0x8000, oba & 0x7fff};
	}

	// if count == 0 then rolls over to max size
	// Virtual On & Gunblade NY
	if (obc == 0)
		obc = 0xfffff;

	switch (geo->mode & 3)
	{
		/* Normals present, No Specular */
		case 0: geo_parse_np_ns(geo, obp, obc); break;

		/* Normals present, Specular */
		case 1: geo_parse_np_s(geo, obp, obc); break;

		/* No Normals present, No Specular */
		case 2: geo_parse_nn_ns(geo, obp, obc); break;

		/* No Normals present, Specular */
		case 3: geo_parse_nn_s(geo, obp, obc); break;
	}

	/* move by 4 parameters */
	return input;
}

/* Command 02: Direct Data */
GeoPtr Geo::geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  tpa = *input++;     /* Texture Point Address */
	uint32_t  tha = *input++;     /* Texture Header Address */

	/* push the initial set of data to the 3d rasterizer */
	model2_3d_push(raster, (opcode >> 23) - 1);
	model2_3d_push(raster, tpa);
	model2_3d_push(raster, tha);

	/* push the initial points */
	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	model2_3d_push(raster, (*input++) >> 8); /* x */
	model2_3d_push(raster, (*input++) >> 8); /* y */
	model2_3d_push(raster, (*input++) >> 8); /* z */

	/* read in the attributes */
	uint32_t  attr;
	while (((attr = *input++) & 3) != 0)
	{
		/* push attributes */
		model2_3d_push(raster, attr & 0x00ffffff);

		/* push luma */
		model2_3d_push(raster, (*input++) >> 8);

		/* push distance */
		model2_3d_push(raster, (*input++) >> 8);

		/* push the next point */
		model2_3d_push(raster, (*input++) >> 8); /* x */
		model2_3d_push(raster, (*input++) >> 8); /* y */
		model2_3d_push(raster, (*input++) >> 8); /* z */

		/* if it's a quad, output another point */
		if (attr & 1)
		{
			model2_3d_push(raster, (*input++) >> 8); /* x */
			model2_3d_push(raster, (*input++) >> 8); /* y */
			model2_3d_push(raster, (*input++) >> 8); /* z */
		}
	}

	/* we're done */
	model2_3d_push(raster, 0);

	return input;
}

/* Command 03: Window Data */
GeoPtr Geo::geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	raster->cur_window++;

	/*
	    we're going to move 6 coordinates to the 3d rasterizer:
	    - starting coordinate
	    - completion coordinate
	    - vanishing point 0 (eye mode 0)
	    - vanishing point 1 (eye mode 1)
	    - vanishing point 2 (eye mode 2)
	    - vanishing point 3 (eye mode 3)
	*/

	for (uint32_t i = 0; i < 6; i++)
	{
		/* read in the coordinate */
		uint32_t y = *input++;

		/* convert to the 3d rasterizer format (00XXXYYY) */
		uint32_t x = (y & 0x0fff0000) >> 4 ;
		y &= 0xfff;

		/* push it */
		model2_3d_push(raster, x | y);
	}

	return input;
}

/* Command 04: Texture Data Write */
GeoPtr Geo::geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	uint32_t count = *input++;

	/* push the count */
	model2_3d_push(raster, count);

	/* loop and send the data */
	for (uint32_t i = 0; i < count; i++)
		model2_3d_push(raster, *input++);

	return input;
}

/* Command 05: Polygon Data */
GeoPtr Geo::geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  address, count, i;
	GeoPtr p;

	(void)opcode;

	/* read in the address */
	address = *input++;

	/* prepare the pointer */
	if (address & 0x01000000)
	{
		/* Fast polygon RAM */
		p = GeoPtr{geo->polygon_ram1, 0x8000, address & 0x7fff};
	}
	else
	{
		/* Slow Polygon RAM */
		p = GeoPtr{geo->polygon_ram0, 0x8000, address & 0x7fff};
	}

	/* read the count */
	count = *input++;

	/* move the data */
	for (i = 0; i < count; i++)
		*p++ = *input++;

	return input;
}

/* Command 06: Texture Parameters */
GeoPtr Geo::geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  index, count, i, param;

	(void)opcode;

	/* read in the index */
	index = (*input++) >> 2;

	/* read in the conut */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		/* read in the texture parameters */
		param = *input++;

		geo->texture_parameters[index].diffuse = float(param & 0xff);
		geo->texture_parameters[index].ambient = float((param >> 8) & 0xff);
		geo->texture_parameters[index].specular_control = (param >> 24) & 0xff;
		geo->texture_parameters[index].specular_scale = float((param >> 16) & 0xff);

		/* read in the distance coefficient */
		geo->coef_table[index] = u2f(*input++);

		index = (index + 1) & 0x1f;
	}

	return input;
}

/* Command 07: Geo Mode */
GeoPtr Geo::geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read in the mode */
	geo->mode = *input++;

	return input;
}

/* Command 08: ZSort Mode */
GeoPtr Geo::geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	/* push the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the mode */
	model2_3d_push(raster, (*input++) >> 8);

	return input;
}

/* Command 09: Focal Distance */
GeoPtr Geo::geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read the x focus value */
	geo->focus.x = u2f(*input++);

	/* read the y focus value */
	geo->focus.y = u2f(*input++);

	return input;
}

/* Command 0A: Light Source Vector Write */
GeoPtr Geo::geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read the x light value */
	geo->light.x = u2f(*input++);

	/* read the y light value */
	geo->light.y = u2f(*input++);

	/* read the z light value */
	geo->light.pz = u2f(*input++);

	return input;
}

/* Command 0B: Transformation Matrix Write */
GeoPtr Geo::geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  i;

	(void)opcode;

	/* read in the transformation matrix */
	for (i = 0; i < 12; i++)
		geo->matrix[i] = u2f(*input++);

	return input;
}

/* Command 0C: Parallel Transfer Vector Write */
GeoPtr Geo::geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  i;

	(void)opcode;

	/* read in the translation vector */
	for (i = 0; i < 3; i++)
		geo->matrix[i+9] = u2f(*input++);

	return input;
}

/* Command 0D: Geo Data Memory Push (undocumented, unsupported) */
GeoPtr Geo::geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  address, count, i;

	/*
	    This command pushes data stored in the Geometry DSP's RAM
	    to the hardware 3D rasterizer. Since we don't emulate the
	    DSP, we don't know what the RAM contents are.

	    Eventually, we could check for the address, and if it
	    happens to point to a polygon ROM, we could potentially
	    emulate it partially.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the address */
	address = *input++;

	/* read in the count */
	count = *input++;


	(void)i;
/*
    for (i = 0; i < count; i++)
        model2_3d_push(0);
*/

	return input;
}

/* Command 0E: Geo Test */
GeoPtr Geo::geo_test(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t      data, blocks, address, count, checksum, i;

	(void)opcode;

	/* fifo test */
	data = 1;

	for (i = 0; i < 32; i++)
	{
		if (*input++ != data)
		{
			/* TODO: Set Red LED on */
				}

		data <<= 1;
	}

	/* get the number of checksums we have to run */
	blocks = *input++;

	for (i = 0; i < blocks; i++)
	{
		uint32_t  sum_even, sum_odd, j;

		/* read in the address */
		address = (*input++) & 0x7fffff;

		/* read in the count */
		count = *input++;

		/* read in the checksum */
		checksum = *input++;

		/* reset the checksum counters */
		sum_even = 0;
		sum_odd = 0;

		for (j = 0; j < count; j++)
		{
			data = geo->polygon_rom[address++];

			address &= geo->polygon_rom_mask;

			sum_even += data >> 16;
			sum_even &= 0xffff;

			sum_odd += data & 0xffff;
			sum_odd &= 0xffff;
		}

		sum_even += checksum >> 16;
		sum_even &= 0xffff;

		sum_odd += checksum & 0xffff;
		sum_odd &= 0xffff;

		if (sum_even != 0 || sum_odd != 0)
		{
			/* TODO: Set Green LED on */
				}
	}

	return input;
}

/* Command 0F: End */
GeoPtr Geo::geo_end(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;

	(void)opcode;

	/* signal the end of this data block the rasterizer */
	model2_3d_push(raster, 0xff000000);

	/* signal end by returning nullptr */
	return GeoPtr{};
}

/* Command 10: Dummy */
GeoPtr Geo::geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input)
{
//  uint32_t  data;
	(void)opcode;

	/* do the dummy read cycle */
//  data = *input++;
	input++;

	return input;
}

/* Command 14: Log Data Write */
GeoPtr Geo::geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	raster_state *raster = geo->raster;
	uint32_t  i, count;

	/* start by pushing the opcode */
	model2_3d_push(raster, opcode >> 23);

	/* push the starting address/dsp id */
	model2_3d_push(raster, *input++);

	/* get the count */
	count = *input++;

	/* push the count */
	model2_3d_push(raster, count << 2);

	/* loop and send the data */
	for (i = 0; i < count; i++)
	{
		uint32_t  data = *input++;

		model2_3d_push(raster, data & 0xff);
		model2_3d_push(raster, (data >> 8) & 0xff);
		model2_3d_push(raster, (data >> 16) & 0xff);
		model2_3d_push(raster, (data >> 24) & 0xff);
	}

	return input;
}

/* Command 16: LOD */
GeoPtr Geo::geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	(void)opcode;

	/* read in the LOD */
	geo->lod = u2f(*input++);

	return input;
}

/* Command 1D: Code Upload  (undocumented, unsupported) */
GeoPtr Geo::geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input)
{
	uint32_t  count, i;

	/*
	    This command uploads code to program memory and
	    optionally runs it. Probably used for debugging.

	    No games are known to use this command yet.
	*/


	(void)opcode;

	/* read in the flags */
//  flags = *input++;
	input++;

	/* read in the count */
	count = *input++;

	for (i = 0; i < count; i++)
	{
		[[maybe_unused]] u64  code;

		/* read the top part of the opcode */
		code = *input++;

		code <<= 32;

		/* the bottom part comes in two pieces */
		code |= *input++;
		code |= (*input++) << 16;
	}

	/*
	    Bit 10 of flags indicate whether to run iummediately after upload
	*/

/*
    if (flags & 0x400)
        code_jump();
*/

	return input;
}

/* Command 1E: Code Jump (undocumented, unsupported) */
GeoPtr Geo::geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input)
{
//  uint32_t  address;

	/*
	    This command jumps to a specified address in program
	    memory. Code can be uploaded with function 1D.
	    Probably used for debugging.

	    No games are known to use this command yet.
	*/


	(void)opcode;

//  address = *input++ & 0x3ff;
	input++;

/*
    code_jump(address)
*/
	return input;
}

GeoPtr Geo::geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code)
{
	switch ((opcode >> 23) & 0x1f)
	{
		case 0x00: input = geo_nop(geo, opcode, input);                   break;
		case 0x01: input = geo_object_data(geo, opcode, input);           break;
		case 0x02: input = geo_direct_data(geo, opcode, input);           break;
		case 0x03: input = geo_window_data(geo, opcode, input);           break;
		case 0x04: input = geo_texture_data(geo, opcode, input);          break;
		case 0x05: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x06: input = geo_texture_parameters(geo, opcode, input);    break;
		case 0x07: input = geo_mode(geo, opcode, input);                  break;
		case 0x08: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x09: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x0a: input = geo_light_source(geo, opcode, input);          break;
		case 0x0b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x0c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x0d: input = geo_data_mem_push(geo, opcode, input);         break;
		case 0x0e: input = geo_test(geo, opcode, input);                  break;
		case 0x0f: input = geo_end(geo, opcode, input); *end_code = true; break;
		case 0x10: input = geo_dummy(geo, opcode, input);                 break;
		case 0x11: input = geo_object_data(geo, opcode, input);           break;
		case 0x12: input = geo_direct_data(geo, opcode, input);           break;
		case 0x13: input = geo_window_data(geo, opcode, input);           break;
		case 0x14: input = geo_log_data(geo, opcode, input);              break;
		case 0x15: input = geo_polygon_data(geo, opcode, input);          break;
		case 0x16: input = geo_lod(geo, opcode, input);                   break;
		case 0x17: input = geo_mode(geo, opcode, input);                  break;
		case 0x18: input = geo_zsort_mode(geo, opcode, input);            break;
		case 0x19: input = geo_focal_distance(geo, opcode, input);        break;
		case 0x1a: input = geo_light_source(geo, opcode, input);          break;
		case 0x1b: input = geo_matrix_write(geo, opcode, input);          break;
		case 0x1c: input = geo_translate_write(geo, opcode, input);       break;
		case 0x1d: input = geo_code_upload(geo, opcode, input);           break;
		case 0x1e: input = geo_code_jump(geo, opcode, input);             break;
		case 0x1f: input = geo_end(geo, opcode, input); *end_code = true; break;
	}

	return input;
}

void Geo::parse(uint32_t read_start)
{
	uint32_t  address = (read_start & 0x1ffff)/4;
	GeoPtr input = buf(address);
	uint32_t  opcode;
	uint32_t  op_count = 0;
	bool end_code = false;

	// reset raster frame variables
	render_frame_start();

	while (end_code == false && input.i < 0x20000/4 && op_count++ < 0x8000)
	{
		/* read in the opcode */
		opcode = *input++;

		/* if it's a jump opcode, do the jump */
		if (opcode & 0x80000000)
		{
			/* get the address */
			address = (opcode & 0x1ffff) / 4;

			/* update our pointer */
			input = buf(address);

			/* go again */
			continue;
		}

		/* process it */
		input = geo_process_command(&geo_, opcode, input, &end_code);
	}
}

/***********************************************************************************************/

} // namespace rt
