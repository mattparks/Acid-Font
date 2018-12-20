#include "Outline.hpp"

#include <assert.h>
#include <string.h>
#include <vector>
#include "Geometry.hpp"

namespace acid
{
	static void add_outline_point(Outline *o, const Vector2 &point)
	{
		if (o->pointCapacity == o->pointCount)
		{
			dyn_array_grow((void **) &o->points, &o->pointCapacity, sizeof(Vector2));
		}

		o->points[o->pointCount] = point;
		o->pointCount++;
	}

	static void add_outline_contour(Outline *o, ContourRange *range)
	{
		if (o->contourCapacity == o->contourCount)
		{
			dyn_array_grow(&o->contours, &o->contourCapacity, sizeof(ContourRange));
		}

		o->contours[o->contourCount] = *range;
		o->contourCount++;
	}

	static void outline_add_odd_point(Outline *o)
	{
		if (o->pointCount % 2 != 0)
		{
			Vector2 p = {o->bbox.minX, o->bbox.minY};
			add_outline_point(o, p);
		}
	}

	static Vector2 convert_point(const FT_Vector *v)
	{
		return Vector2(
			static_cast<float>(v->x) / 64.0f,
			static_cast<float>(v->y) / 64.0f
		);
	}

	static int move_to_func(const FT_Vector *to, Outline *o)
	{
		Vector2 p = Vector2();

		if (o->contourCount > 0)
		{
			o->contours[o->contourCount - 1].end = o->pointCount - 1;
			add_outline_point(o, p);
		}

		assert(o->pointCount % 2 == 0);

		ContourRange range = {o->pointCount, std::numeric_limits<uint32_t>::max()};
		add_outline_contour(o, &range);

		p = convert_point(to);
		add_outline_point(o, p);
		return 0;
	}

	static int line_to_func(const FT_Vector *to, Outline *o)
	{
		uint32_t last = o->pointCount - 1;

		Vector2 to_p;
		to_p = convert_point(to);
		Vector2 p = o->points[last].Lerp(to_p, 0.5f);
		add_outline_point(o, p);
		add_outline_point(o, to_p);
		return 0;
	}

	static int conic_to_func(const FT_Vector *control, const FT_Vector *to, Outline *o)
	{
		Vector2 p;
		p = convert_point(control);
		add_outline_point(o, p);

		p = convert_point(to);
		add_outline_point(o, p);
		return 0;
	}

	static int cubic_to_func(const FT_Vector *control1, const FT_Vector *control2, const FT_Vector *to, Outline *o)
	{
		return line_to_func(to, o);
	}

	void outline_decompose(FT_Outline *outline, Outline *o)
	{
		memset(o, 0, sizeof(Outline));

		FT_BBox outline_bbox;
		FT_CHECK(FT_Outline_Get_BBox(outline, &outline_bbox));

		o->bbox.minX = static_cast<float>(outline_bbox.xMin) / 64.0f;
		o->bbox.minY = static_cast<float>(outline_bbox.yMin) / 64.0f;
		o->bbox.maxX = static_cast<float>(outline_bbox.xMax) / 64.0f;
		o->bbox.maxY = static_cast<float>(outline_bbox.yMax) / 64.0f;

		FT_Outline_Funcs funcs = {};
		funcs.move_to = reinterpret_cast<FT_Outline_MoveToFunc>(move_to_func);
		funcs.line_to = reinterpret_cast<FT_Outline_LineToFunc>(line_to_func);
		funcs.conic_to = reinterpret_cast<FT_Outline_ConicToFunc>(conic_to_func);
		funcs.cubic_to = reinterpret_cast<FT_Outline_CubicToFunc>(cubic_to_func);

		FT_CHECK(FT_Outline_Decompose(outline, &funcs, o));

		if (o->contourCount > 0)
		{
			o->contours[o->contourCount - 1].end = o->pointCount - 1;
		}
	}

	static uint32_t cell_add_range(uint32_t cell, uint32_t from, uint32_t to)
	{
		assert(from % 2 == 0 && to % 2 == 0);

		from /= 2;
		to /= 2;

		if (from >= std::numeric_limits<uint8_t>::max())
		{
			return 0;
		}

		if (to >= std::numeric_limits<uint8_t>::max())
		{
			return 0;
		}

		uint32_t length = to - from;

		if (length <= 3 && (cell & 0x03) == 0)
		{
			cell |= from << 8;
			cell |= length;
			return cell;
		}

		if (length > 7)
		{
			return 0;
		}

		if ((cell & 0x1C) == 0)
		{
			cell |= from << 16;
			cell |= length << 2;
			return cell;
		}

		if ((cell & 0xE0) == 0)
		{
			cell |= from << 24;
			cell |= length << 5;
			return cell;
		}

		return 0;
	}

	static bool is_cell_filled(Outline *o, Rect *bbox)
	{
		Vector2 p = {
			(bbox->maxX + bbox->minX) / 2.0f,
			(bbox->maxY + bbox->minY) / 2.0f,
		};

		float mindist = std::numeric_limits<float>::max();
		float v = std::numeric_limits<float>::max();
		uint32_t j = std::numeric_limits<uint32_t>::max();

		for (uint32_t contour_index = 0; contour_index < o->contourCount; contour_index++)
		{
			uint32_t contour_begin = o->contours[contour_index].begin;
			uint32_t contour_end = o->contours[contour_index].end;

			for (uint32_t i = contour_begin; i < contour_end; i += 2)
			{
				Vector2 p0 = o->points[i];
				Vector2 p1 = o->points[i + 1];
				Vector2 p2 = o->points[i + 2];

				float t = line_calculate_t(p0, p2, p);

				Vector2 p02 = p0.Lerp(p2, t);

				float udist = p02.Distance(p);

				if (udist < mindist + 0.0001f)
				{
					float d = line_signed_distance(p0, p2, p);

					if (udist >= mindist && i > contour_begin)
					{
						float lastd = i == contour_end - 2 && j == contour_begin
						              ? line_signed_distance(p0, p2, o->points[contour_begin + 2])
						              : line_signed_distance(p0, p2, o->points[i - 2]);

						if (lastd < 0.0f)
						{
							v = std::max(d, v);
						}
						else
						{
							v = std::min(d, v);
						}
					}
					else
					{
						v = d;
					}

					mindist = std::min(mindist, udist);
					j = i;
				}
			}
		}

		return v > 0.0f;
	}

	static bool wipcell_add_bezier(Outline *o, Outline *u, uint32_t i, uint32_t j, uint32_t contour_index, WIPCell *cell)
	{
		bool ret = true;
		uint32_t ucontour_begin = u->contours[contour_index].begin;

		if (cell->to != std::numeric_limits<uint32_t>::max() && cell->to != j)
		{
			assert(cell->to < j);

			if (cell->from == ucontour_begin)
			{
				assert(cell->to % 2 == 0);
				assert(cell->from % 2 == 0);

				cell->startLength = (cell->to - cell->from) / 2;
			} else
			{
				cell->value = cell_add_range(cell->value, cell->from, cell->to);

				if (!cell->value)
				{
					ret = false;
				}
			}

			cell->from = j;
		} else
		{
			if (cell->from == std::numeric_limits<uint32_t>::max())
			{
				cell->from = j;
			}
		}

		cell->to = j + 2;
		return ret;
	}

	static bool
	wipcell_finish_contour(Outline *o, Outline *u, uint32_t contour_index, WIPCell *cell, uint32_t *max_start_len)
	{
		bool ret = true;
		uint32_t ucontour_begin = u->contours[contour_index].begin;
		uint32_t ucontour_end = u->contours[contour_index].end;

		if (cell->to < ucontour_end)
		{
			cell->value = cell_add_range(cell->value, cell->from, cell->to);

			if (!cell->value)
			{
				ret = false;
			}

			cell->from = std::numeric_limits<uint32_t>::max();
			cell->to = std::numeric_limits<uint32_t>::max();
		}

		assert(cell->to == std::numeric_limits<uint32_t>::max() || cell->to == ucontour_end);
		cell->to = std::numeric_limits<uint32_t>::max();

		if (cell->from != std::numeric_limits<uint32_t>::max() && cell->startLength != 0)
		{
			cell->value = cell_add_range(cell->value, cell->from, ucontour_end + cell->startLength * 2);

			if (!cell->value)
			{
				ret = false;
			}

			*max_start_len = std::max(*max_start_len, cell->startLength);
			cell->from = std::numeric_limits<uint32_t>::max();
			cell->startLength = 0;
		}

		if (cell->from != std::numeric_limits<uint32_t>::max())
		{
			cell->value = cell_add_range(cell->value, cell->from, ucontour_end);

			if (!cell->value)
			{
				ret = false;
			}

			cell->from = std::numeric_limits<uint32_t>::max();
		}

		if (cell->startLength != 0)
		{
			cell->value = cell_add_range(cell->value, ucontour_begin, ucontour_begin + cell->startLength * 2);

			if (!cell->value)
			{
				ret = false;
			}

			cell->startLength = 0;
		}

		assert(cell->from == std::numeric_limits<uint32_t>::max() && cell->to == std::numeric_limits<uint32_t>::max());
		return ret;
	}

	static bool for_each_wipcell_add_bezier(Outline *o, Outline *u, uint32_t i, uint32_t j, uint32_t contour_index, WIPCell *cells)
	{
		Rect bezier_bbox = bezier2_bbox(&o->points[i]);

		float outline_bbox_w = o->bbox.maxX - o->bbox.minX;
		float outline_bbox_h = o->bbox.maxY - o->bbox.minY;

		auto min_x = static_cast<uint32_t>((bezier_bbox.minX - o->bbox.minX) / outline_bbox_w * o->cellCountX);
		auto min_y = static_cast<uint32_t>((bezier_bbox.minY - o->bbox.minY) / outline_bbox_h * o->cellCountY);
		auto max_x = static_cast<uint32_t>((bezier_bbox.maxX - o->bbox.minX) / outline_bbox_w * o->cellCountX);
		auto max_y = static_cast<uint32_t>((bezier_bbox.maxY - o->bbox.minY) / outline_bbox_h * o->cellCountY);

		if (max_x >= o->cellCountX)
		{
			max_x = o->cellCountX - 1;
		}

		if (max_y >= o->cellCountY)
		{
			max_y = o->cellCountY - 1;
		}

		bool ret = true;

		for (uint32_t y = min_y; y <= max_y; y++)
		{
			for (uint32_t x = min_x; x <= max_x; x++)
			{
				WIPCell *cell = &cells[y * o->cellCountX + x];

				if (bbox_bezier2_intersect(&cell->bbox, &o->points[i]))
				{
					ret &= wipcell_add_bezier(o, u, i, j, contour_index, cell);
				}
			}
		}

		return ret;
	}

	static bool for_each_wipcell_finish_contour(Outline *o, Outline *u, uint32_t contour_index, WIPCell *cells,
	                                            uint32_t *max_start_len)
	{
		bool ret = true;

		for (uint32_t y = 0; y < o->cellCountY; y++)
		{
			for (uint32_t x = 0; x < o->cellCountX; x++)
			{
				WIPCell *cell = &cells[y * o->cellCountX + x];
				ret &= wipcell_finish_contour(o, u, contour_index, cell, max_start_len);
			}
		}

		return ret;
	}

	static void copy_wipcell_values(Outline *u, WIPCell *cells)
	{
		u->cells = (uint32_t *) malloc(sizeof(uint32_t) * u->cellCountX * u->cellCountY);

		for (uint32_t y = 0; y < u->cellCountY; y++)
		{
			for (uint32_t x = 0; x < u->cellCountX; x++)
			{
				uint32_t i = y * u->cellCountX + x;
				u->cells[i] = cells[i].value;
			}
		}
	}

	static void init_wipcells(Outline *o, WIPCell *cells)
	{
		float w = o->bbox.maxX - o->bbox.minX;
		float h = o->bbox.maxY - o->bbox.minY;

		for (uint32_t y = 0; y < o->cellCountY; y++)
		{
			for (uint32_t x = 0; x < o->cellCountX; x++)
			{
				Rect bbox = {
					o->bbox.minX + ((float) x / o->cellCountX) * w,
					o->bbox.minY + ((float) y / o->cellCountY) * h,
					o->bbox.minX + ((float) (x + 1) / o->cellCountX) * w,
					o->bbox.minY + ((float) (y + 1) / o->cellCountY) * h,
				};

				uint32_t i = y * o->cellCountX + x;
				cells[i].bbox = bbox;
				cells[i].from = std::numeric_limits<uint32_t>::max();
				cells[i].to = std::numeric_limits<uint32_t>::max();
				cells[i].value = 0;
				cells[i].startLength = 0;
			}
		}
	}

	static uint32_t outline_add_filled_line(Outline *o)
	{
		outline_add_odd_point(o);

		uint32_t i = o->pointCount;
		float y = o->bbox.maxY + 1000.0f;
		Vector2 f0 = {o->bbox.minX, y};
		Vector2 f1 = {o->bbox.minX + 10.0f, y};
		Vector2 f2 = {o->bbox.minX + 20.0f, y};
		add_outline_point(o, f0);
		add_outline_point(o, f1);
		add_outline_point(o, f2);

		return i;
	}

	static uint32_t make_cell_from_single_edge(uint32_t e)
	{
		assert(e % 2 == 0);
		return e << 7 | 1;
	}

	static void set_filled_cells(Outline *u, WIPCell *cells, uint32_t filled_cell)
	{
		for (uint32_t y = 0; y < u->cellCountY; y++)
		{
			for (uint32_t x = 0; x < u->cellCountX; x++)
			{
				uint32_t i = y * u->cellCountX + x;
				WIPCell *cell = &cells[i];

				if (cell->value == 0 && is_cell_filled(u, &cell->bbox))
				{
					cell->value = filled_cell;
				}
			}
		}
	}

	static bool try_to_fit_in_cell_count(Outline *o)
	{
		bool ret = true;

		auto cells = std::vector<WIPCell>(o->cellCountX * o->cellCountY);
		init_wipcells(o, cells.data());

		Outline u = {};
		u.bbox = o->bbox;
		u.cellCountX = o->cellCountX;
		u.cellCountY = o->cellCountY;

		for (uint32_t contour_index = 0; contour_index < o->contourCount; contour_index++)
		{
			uint32_t contour_begin = o->contours[contour_index].begin;
			uint32_t contour_end = o->contours[contour_index].end;

			outline_add_odd_point(&u);

			ContourRange urange = {u.pointCount, u.pointCount + contour_end - contour_begin};
			add_outline_contour(&u, &urange);

			for (uint32_t i = contour_begin; i < contour_end; i += 2)
			{
				Vector2 p0 = o->points[i];
				Vector2 p1 = o->points[i + 1];

				uint32_t j = u.pointCount;
				add_outline_point(&u, p0);
				add_outline_point(&u, p1);

				ret &= for_each_wipcell_add_bezier(o, &u, i, j, contour_index, cells.data());
			}

			uint32_t max_start_len = 0;
			ret &= for_each_wipcell_finish_contour(o, &u, contour_index, cells.data(), &max_start_len);

			uint32_t continuation_end = contour_begin + max_start_len * 2;

			for (uint32_t i = contour_begin; i < continuation_end; i += 2)
			{
				add_outline_point(&u, o->points[i]);
				add_outline_point(&u, o->points[i + 1]);
			}

			Vector2 plast = o->points[continuation_end];
			add_outline_point(&u, plast);
		}

		if (!ret)
		{
			outline_destroy(&u);
			return ret;
		}

		uint32_t filled_line = outline_add_filled_line(&u);
		uint32_t filled_cell = make_cell_from_single_edge(filled_line);
		set_filled_cells(&u, cells.data(), filled_cell);

		copy_wipcell_values(&u, cells.data());

		outline_destroy(o);
		*o = u;
		return ret;
	}

	static uint32_t uint32_to_pow2(uint32_t v)
	{
		v--;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		v++;
		return v;
	}

	void outline_make_cells(Outline *o)
	{
		if (o->pointCount > OUTLINE_MAX_POINTS)
		{
			return;
		}

		float w = o->bbox.maxX - o->bbox.minX;
		float h = o->bbox.maxY - o->bbox.minY;

		uint32_t c = uint32_to_pow2((uint32_t) sqrtf(o->pointCount * 0.75f));

		o->cellCountX = c;
		o->cellCountY = c;

		if (h > w * 1.8f)
		{
			o->cellCountX /= 2;
		}

		if (w > h * 1.8f)
		{
			o->cellCountY /= 2;
		}

		while (true)
		{
			if (try_to_fit_in_cell_count(o))
			{
				break;
			}

			if (o->cellCountX > 64 || o->cellCountY > 64)
			{
				o->cellCountX = 0;
				o->cellCountY = 0;
				return;
			}

			if (o->cellCountX == o->cellCountY)
			{
				if (w > h)
				{
					o->cellCountX *= 2;
				} else
				{
					o->cellCountY *= 2;
				}
			} else
			{
				if (o->cellCountX < o->cellCountY)
				{
					o->cellCountX *= 2;
				} else
				{
					o->cellCountY *= 2;
				}
			}
		}
	}

	void outline_subdivide(Outline *o)
	{
		Outline u = {};
		u.bbox = o->bbox;

		for (uint32_t contour_index = 0; contour_index < o->contourCount; contour_index++)
		{
			uint32_t contour_begin = o->contours[contour_index].begin;
			uint32_t contour_end = o->contours[contour_index].end;

			outline_add_odd_point(&u);

			ContourRange urange = {u.pointCount, std::numeric_limits<uint32_t>::max()};
			add_outline_contour(&u, &urange);

			for (uint32_t i = contour_begin; i < contour_end; i += 2)
			{
				Vector2 p0 = o->points[i];

				Vector2 newp[3];
				bezier2_split_3p(newp, &o->points[i], 0.5f);

				add_outline_point(&u, p0);
				add_outline_point(&u, newp[0]);
				add_outline_point(&u, newp[1]);
				add_outline_point(&u, newp[2]);
			}

			u.contours[contour_index].end = u.pointCount;
			add_outline_point(&u, o->points[contour_end]);
		}

		outline_destroy(o);
		*o = u;
	}

	void outline_fix_thin_lines(Outline *o)
	{
		Outline u = {};
		u.bbox = o->bbox;

		for (uint32_t contour_index = 0; contour_index < o->contourCount; contour_index++)
		{
			uint32_t contour_begin = o->contours[contour_index].begin;
			uint32_t contour_end = o->contours[contour_index].end;

			outline_add_odd_point(&u);

			ContourRange urange = {u.pointCount, std::numeric_limits<uint32_t>::max()};
			add_outline_contour(&u, &urange);

			for (uint32_t i = contour_begin; i < contour_end; i += 2)
			{
				Vector2 p0 = o->points[i];
				Vector2 p1 = o->points[i + 1];
				Vector2 p2 = o->points[i + 2];

				Vector2 mid = p0.Lerp(p2, 0.5f);
				Vector2 midp1 = p1 - mid;

				Vector2 bezier[] = {
					{p0.m_x, p0.m_y},
					{p1.m_x, p1.m_y},
					{p2.m_x, p2.m_y}
				};

				bezier[1] += midp1;
				bool subdivide = false;

				for (uint32_t j = contour_begin; j < contour_end; j += 2)
				{
					if (i == contour_begin && j == contour_end - 2)
					{
						continue;
					}

					if (i == contour_end - 2 && j == contour_begin)
					{
						continue;
					}

					if (j + 2 >= i && j <= i + 2)
					{
						continue;
					}

					Vector2 q0 = o->points[j];
					Vector2 q2 = o->points[j + 2];

					if (bezier2_line_is_intersecting(bezier, q0, q2))
					{
						subdivide = true;
					}
				}

				if (subdivide)
				{
					Vector2 newp[3];
					bezier2_split_3p(newp, &o->points[i], 0.5f);

					add_outline_point(&u, p0);
					add_outline_point(&u, newp[0]);
					add_outline_point(&u, newp[1]);
					add_outline_point(&u, newp[2]);
				} else
				{
					add_outline_point(&u, p0);
					add_outline_point(&u, p1);
				}
			}

			u.contours[contour_index].end = u.pointCount;
			add_outline_point(&u, o->points[contour_end]);
		}

		outline_destroy(o);
		*o = u;
	}

	void outline_convert(FT_Outline *outline, Outline *o, char c)
	{
		if (c == '&')
		{
			printf("");
		}

		outline_decompose(outline, o);
		outline_fix_thin_lines(o);
		outline_make_cells(o);
	}

	void outline_destroy(Outline *o)
	{
		if (o->contours)
		{
			free(o->contours);
		}

		if (o->points)
		{
			free(o->points);
		}

		if (o->cells)
		{
			free(o->cells);
		}

		memset(o, 0, sizeof(Outline));
	}

	void outline_cbox(Outline *o, Rect *cbox)
	{
		if (o->pointCount == 0)
		{
			return;
		}

		cbox->minX = o->points[0].m_x;
		cbox->minY = o->points[0].m_y;
		cbox->maxX = o->points[0].m_x;
		cbox->maxY = o->points[0].m_y;

		for (uint32_t i = 1; i < o->pointCount; i++)
		{
			float x = o->points[i].m_x;
			float y = o->points[i].m_y;

			cbox->minX = std::min(cbox->minX, x);
			cbox->minY = std::min(cbox->minY, y);
			cbox->maxX = std::max(cbox->maxX, x);
			cbox->maxY = std::max(cbox->maxY, y);
		}
	}

	static uint16_t gen_u16_value(float x, float min, float max)
	{
		return static_cast<uint16_t>((x - min) / (max - min) * std::numeric_limits<uint16_t>::max());
	}
}
