/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdlib.h>

#include <cairo.h>
#include <gegl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "libgimpcolor/gimpcolor.h"
#include "libgimpmath/gimpmath.h"

#include "core-types.h"

#include "gegl/gimp-babl.h"

#include "gimpchannel.h"
#include "gimpimage.h"
#include "gimpimage-contiguous-region.h"
#include "gimppickable.h"


/*  local function prototypes  */

static gfloat   pixel_difference          (const gfloat        *col1,
                                           const gfloat        *col2,
                                           gboolean             antialias,
                                           gfloat               threshold,
                                           gint                 n_components,
                                           gboolean             has_alpha,
                                           gboolean             select_transparent,
                                           GimpSelectCriterion  select_criterion);
static gboolean find_contiguous_segment   (const gfloat        *col,
                                           GeglBuffer          *src_buffer,
                                           GeglBuffer          *mask_buffer,
                                           const Babl          *src_format,
                                           gint                 n_components,
                                           gboolean             has_alpha,
                                           gint                 width,
                                           gboolean             select_transparent,
                                           GimpSelectCriterion  select_criterion,
                                           gboolean             antialias,
                                           gfloat               threshold,
                                           gint                 initial_x,
                                           gint                 initial_y,
                                           gint                *start,
                                           gint                *end);
static void find_contiguous_region_helper (GeglBuffer          *src_buffer,
                                           GeglBuffer          *mask_buffer,
                                           const Babl          *format,
                                           gboolean             select_transparent,
                                           GimpSelectCriterion  select_criterion,
                                           gboolean             antialias,
                                           gfloat               threshold,
                                           gint                 x,
                                           gint                 y,
                                           const gfloat        *col);


/*  public functions  */

GimpChannel *
gimp_image_contiguous_region_by_seed (GimpImage           *image,
                                      GimpDrawable        *drawable,
                                      gboolean             sample_merged,
                                      gboolean             antialias,
                                      gfloat               threshold,
                                      gboolean             select_transparent,
                                      GimpSelectCriterion  select_criterion,
                                      gint                 x,
                                      gint                 y)
{
  GimpPickable *pickable;
  GeglBuffer   *src_buffer;
  GimpChannel  *mask;
  GeglBuffer   *mask_buffer;
  const Babl   *src_format;
  gfloat        start_col[MAX_CHANNELS];

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);

  if (sample_merged)
    pickable = GIMP_PICKABLE (gimp_image_get_projection (image));
  else
    pickable = GIMP_PICKABLE (drawable);

  gimp_pickable_flush (pickable);

  src_format = gimp_pickable_get_format (pickable);
  if (babl_format_is_palette (src_format))
    src_format = babl_format ("RGBA float");
  else
    src_format = gimp_babl_format (gimp_babl_format_get_base_type (src_format),
                                   GIMP_PRECISION_FLOAT,
                                   babl_format_has_alpha (src_format));

  src_buffer = gimp_pickable_get_buffer (pickable);

  mask = gimp_channel_new_mask (image,
                                gegl_buffer_get_width  (src_buffer),
                                gegl_buffer_get_height (src_buffer));

  mask_buffer = gimp_drawable_get_buffer (GIMP_DRAWABLE (mask));

  gegl_buffer_sample (src_buffer, x, y, NULL, start_col, src_format,
                      GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);

  if (babl_format_has_alpha (src_format))
    {
      if (select_transparent)
        {
          gint n_components = babl_format_get_n_components (src_format);

          /*  don't select transparent regions if the start pixel isn't
           *  fully transparent
           */
          if (start_col[n_components - 1] > 0)
            select_transparent = FALSE;
        }
    }
  else
    {
      select_transparent = FALSE;
    }

  find_contiguous_region_helper (src_buffer, mask_buffer, src_format,
                                 select_transparent, select_criterion,
                                 antialias, threshold,
                                 x, y, start_col);

  return mask;
}

GimpChannel *
gimp_image_contiguous_region_by_color (GimpImage            *image,
                                       GimpDrawable         *drawable,
                                       gboolean              sample_merged,
                                       gboolean              antialias,
                                       gfloat                threshold,
                                       gboolean              select_transparent,
                                       GimpSelectCriterion  select_criterion,
                                       const GimpRGB        *color)
{
  /*  Scan over the image's active layer, finding pixels within the
   *  specified threshold from the given R, G, & B values.  If
   *  antialiasing is on, use the same antialiasing scheme as in
   *  fuzzy_select.  Modify the image's mask to reflect the
   *  additional selection
   */
  GeglBufferIterator *iter;
  GimpPickable       *pickable;
  GimpChannel        *mask;
  GeglBuffer         *src_buffer;
  GeglBuffer         *mask_buffer;
  gint                width, height;
  gboolean            has_alpha;
  gfloat              col[MAX_CHANNELS];

  g_return_val_if_fail (GIMP_IS_IMAGE (image), NULL);
  g_return_val_if_fail (GIMP_IS_DRAWABLE (drawable), NULL);
  g_return_val_if_fail (color != NULL, NULL);

  gimp_rgba_get_pixel (color, babl_format ("RGBA float"), col);

  if (sample_merged)
    pickable = GIMP_PICKABLE (gimp_image_get_projection (image));
  else
    pickable = GIMP_PICKABLE (drawable);

  gimp_pickable_flush (pickable);

  has_alpha = babl_format_has_alpha (gimp_pickable_get_format (pickable));

  src_buffer = gimp_pickable_get_buffer (pickable);
  width  = gegl_buffer_get_width (src_buffer);
  height = gegl_buffer_get_height (src_buffer);

  iter = gegl_buffer_iterator_new (src_buffer,
                                   NULL, 0, babl_format ("RGBA float"),
                                   GEGL_BUFFER_READ, GEGL_ABYSS_NONE);

  if (has_alpha)
    {
      if (select_transparent)
        {
          /*  don't select transparancy if "color" isn't fully transparent
           */
          if (col[3] > 0.0)
            select_transparent = FALSE;
        }
    }
  else
    {
      select_transparent = FALSE;
    }

  mask = gimp_channel_new_mask (image, width, height);

  mask_buffer = gimp_drawable_get_buffer (GIMP_DRAWABLE (mask));

  gegl_buffer_iterator_add (iter, mask_buffer,
                            NULL, 0, babl_format ("Y float"),
                            GEGL_BUFFER_WRITE, GEGL_ABYSS_NONE);

  while (gegl_buffer_iterator_next (iter))
    {
      const gfloat *src  = iter->data[0];
      gfloat       *dest = iter->data[1];

      while (iter->length--)
        {
          /*  Find how closely the colors match  */
          *dest = pixel_difference (col, src,
                                    antialias,
                                    threshold,
                                    has_alpha ? 4 : 3,
                                    has_alpha,
                                    select_transparent,
                                    select_criterion);

          src  += 4;
          dest += 1;
        }
    }

  return mask;
}


/*  private functions  */

static gfloat
pixel_difference (const gfloat        *col1,
                  const gfloat        *col2,
                  gboolean             antialias,
                  gfloat               threshold,
                  gint                 n_components,
                  gboolean             has_alpha,
                  gboolean             select_transparent,
                  GimpSelectCriterion  select_criterion)
{
  gfloat max = 0.0;

  /*  if there is an alpha channel, never select transparent regions  */
  if (! select_transparent && has_alpha && col2[n_components - 1] == 0.0)
    return 0.0;

  if (select_transparent && has_alpha)
    {
      max = fabs (col1[n_components - 1] - col2[n_components - 1]);
    }
  else
    {
      gfloat  diff;
      gint    b;

      if (has_alpha)
        n_components--;

      switch (select_criterion)
        {
        case GIMP_SELECT_CRITERION_COMPOSITE:
          for (b = 0; b < n_components; b++)
            {
              diff = fabs (col1[b] - col2[b]);
              if (diff > max)
                max = diff;
            }
          break;

        case GIMP_SELECT_CRITERION_R:
          max = fabs (col1[0] - col2[0]);
          break;

        case GIMP_SELECT_CRITERION_G:
          max = fabs (col1[1] - col2[1]);
          break;

        case GIMP_SELECT_CRITERION_B:
          max = fabs (col1[2] - col2[2]);
          break;

#if 0
        case GIMP_SELECT_CRITERION_H:
          av0 = (gint) col1[0];
          av1 = (gint) col1[1];
          av2 = (gint) col1[2];
          bv0 = (gint) col2[0];
          bv1 = (gint) col2[1];
          bv2 = (gint) col2[2];
          gimp_rgb_to_hsv_int (&av0, &av1, &av2);
          gimp_rgb_to_hsv_int (&bv0, &bv1, &bv2);
          /* wrap around candidates for the actual distance */
          {
            gint dist1 = abs (av0 - bv0);
            gint dist2 = abs (av0 - 360 - bv0);
            gint dist3 = abs (av0 - bv0 + 360);
            max = MIN (dist1, dist2);
            if (max > dist3)
              max = dist3;
          }
          break;

        case GIMP_SELECT_CRITERION_S:
          av0 = (gint) col1[0];
          av1 = (gint) col1[1];
          av2 = (gint) col1[2];
          bv0 = (gint) col2[0];
          bv1 = (gint) col2[1];
          bv2 = (gint) col2[2];
          gimp_rgb_to_hsv_int (&av0, &av1, &av2);
          gimp_rgb_to_hsv_int (&bv0, &bv1, &bv2);
          max = abs (av1 - bv1);
          break;

        case GIMP_SELECT_CRITERION_V:
          av0 = (gint) col1[0];
          av1 = (gint) col1[1];
          av2 = (gint) col1[2];
          bv0 = (gint) col2[0];
          bv1 = (gint) col2[1];
          bv2 = (gint) col2[2];
          gimp_rgb_to_hsv_int (&av0, &av1, &av2);
          gimp_rgb_to_hsv_int (&bv0, &bv1, &bv2);
          max = abs (av2 - bv2);
          break;
#endif
        }
    }

  if (antialias && threshold > 0.0)
    {
      gfloat aa = 1.5 - (max / threshold);

      if (aa <= 0.0)
        return 0.0;
      else if (aa < 0.5)
        return aa * 2.0;
      else
        return 1.0;
    }
  else
    {
      if (max > threshold)
        return 0.0;
      else
        return 1.0;
    }
}

static gboolean
find_contiguous_segment (const gfloat        *col,
                         GeglBuffer          *src_buffer,
                         GeglBuffer          *mask_buffer,
                         const Babl          *src_format,
                         gint                 n_components,
                         gboolean             has_alpha,
                         gint                 width,
                         gboolean             select_transparent,
                         GimpSelectCriterion  select_criterion,
                         gboolean             antialias,
                         gfloat               threshold,
                         gint                 initial_x,
                         gint                 initial_y,
                         gint                *start,
                         gint                *end)
{
  gfloat s[MAX_CHANNELS];
  gfloat mask_row[width];
  gfloat diff;

  gegl_buffer_sample (src_buffer, initial_x, initial_y, NULL, s, src_format,
                      GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);

  diff = pixel_difference (col, s, antialias, threshold,
                           n_components, has_alpha, select_transparent,
                           select_criterion);

  /* check the starting pixel */
  if (! diff)
    return FALSE;

  mask_row[initial_x] = diff;

  *start = initial_x - 1;

  while (*start >= 0 && diff)
    {
      gegl_buffer_sample (src_buffer, *start, initial_y, NULL, s, src_format,
                          GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);

      diff = pixel_difference (col, s, antialias, threshold,
                               n_components, has_alpha, select_transparent,
                               select_criterion);

      mask_row[*start] = diff;

      if (diff)
        (*start)--;
    }

  diff = 1;
  *end = initial_x + 1;

  while (*end < width && diff)
    {
      gegl_buffer_sample (src_buffer, *end, initial_y, NULL, s, src_format,
                          GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);

      diff = pixel_difference (col, s, antialias, threshold,
                               n_components, has_alpha, select_transparent,
                               select_criterion);

      mask_row[*end] = diff;

      if (diff)
        (*end)++;
    }

  gegl_buffer_set (mask_buffer, GEGL_RECTANGLE (*start, initial_y,
                                                *end - *start, 1),
                   0, babl_format ("Y float"), &mask_row[*start],
                   GEGL_AUTO_ROWSTRIDE);

  /* XXX this should now be needed and is a performance killer */
  gegl_buffer_sample_cleanup (mask_buffer);

  return TRUE;
}

static void
find_contiguous_region_helper (GeglBuffer          *src_buffer,
                               GeglBuffer          *mask_buffer,
                               const Babl          *format,
                               gboolean             select_transparent,
                               GimpSelectCriterion  select_criterion,
                               gboolean             antialias,
                               gfloat               threshold,
                               gint                 x,
                               gint                 y,
                               const gfloat        *col)
{
  gint    start, end;
  gint    new_start, new_end;
  GQueue *coord_stack;

  coord_stack = g_queue_new ();

  /* To avoid excessive memory allocation (y, start, end) tuples are
   * stored in interleaved format:
   *
   * [y1] [start1] [end1] [y2] [start2] [end2]
   */
  g_queue_push_tail (coord_stack, GINT_TO_POINTER (y));
  g_queue_push_tail (coord_stack, GINT_TO_POINTER (x - 1));
  g_queue_push_tail (coord_stack, GINT_TO_POINTER (x + 1));

  do
    {
      y     = GPOINTER_TO_INT (g_queue_pop_head (coord_stack));
      start = GPOINTER_TO_INT (g_queue_pop_head (coord_stack));
      end   = GPOINTER_TO_INT (g_queue_pop_head (coord_stack));

      for (x = start + 1; x < end; x++)
        {
          gfloat val;

          gegl_buffer_sample (mask_buffer, x, y, NULL, &val,
                              babl_format ("Y float"),
                              GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);
          if (val != 0.0)
            continue;

          if (! find_contiguous_segment (col, src_buffer, mask_buffer,
                                         format,
                                         babl_format_get_n_components (format),
                                         babl_format_has_alpha (format),
                                         gegl_buffer_get_width (src_buffer),
                                         select_transparent, select_criterion,
                                         antialias, threshold, x, y,
                                         &new_start, &new_end))
            continue;

          if (y + 1 < gegl_buffer_get_height (src_buffer))
            {
              g_queue_push_tail (coord_stack, GINT_TO_POINTER (y + 1));
              g_queue_push_tail (coord_stack, GINT_TO_POINTER (new_start));
              g_queue_push_tail (coord_stack, GINT_TO_POINTER (new_end));
            }

          if (y - 1 >= 0)
            {
              g_queue_push_tail (coord_stack, GINT_TO_POINTER (y - 1));
              g_queue_push_tail (coord_stack, GINT_TO_POINTER (new_start));
              g_queue_push_tail (coord_stack, GINT_TO_POINTER (new_end));
            }
        }
    }
  while (! g_queue_is_empty (coord_stack));

  g_queue_free (coord_stack);
}
