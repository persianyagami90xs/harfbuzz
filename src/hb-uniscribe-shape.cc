/*
 * Copyright © 2011  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Google Author(s): Behdad Esfahbod
 */

#define _WIN32_WINNT 0x0500

#include "hb-private.hh"

#include "hb-uniscribe.h"

#include "hb-ot-tag.h"

#include "hb-font-private.hh"

#include "hb-buffer-private.hh"

#include <windows.h>
#include <usp10.h>



#ifndef HB_DEBUG_UNISCRIBE
#define HB_DEBUG_UNISCRIBE (HB_DEBUG+0)
#endif


/*
DWORD GetFontData(
  __in   HDC hdc,
  __in   DWORD dwTable,
  __in   DWORD dwOffset,
  __out  LPVOID lpvBuffer,
  __in   DWORD cbData
);
*/

static void
fallback_shape (hb_font_t   *font,
		hb_buffer_t *buffer)
{
  DEBUG_MSG (UNISCRIBE, NULL, "Fallback shaper invoked");
}

static void
populate_log_font (LOGFONTW  *lf,
		   HDC        hdc,
		   hb_font_t *font,
		   hb_blob_t *blob)
{
  memset (lf, 0, sizeof (*lf));
  int dpi = GetDeviceCaps (hdc, LOGPIXELSY);
  lf->lfHeight = MulDiv (font->x_scale, dpi, 72);

  WCHAR family_name[] = {'n','a','z','l','i'};
  for (unsigned int i = 0; family_name[i] && i < LF_FACESIZE - 1; i++)
    lf->lfFaceName[i] = family_name[i];
}

void
hb_uniscribe_shape (hb_font_t          *font,
		    hb_buffer_t        *buffer,
		    const hb_feature_t *features,
		    unsigned int        num_features)
{
  HRESULT hr;

  if (unlikely (!buffer->len)) {
  fallback:
    fallback_shape (font, buffer);
  }

retry:

  unsigned int scratch_size;
  char *scratch = (char *) buffer->get_scratch_buffer (&scratch_size);

  /* Allocate char buffers; they all fit */

#define ALLOCATE_ARRAY(Type, name, len) \
  Type *name = (Type *) scratch; \
  scratch += len * sizeof (name[0]); \
  scratch_size -= len * sizeof (name[0]);

#define utf16_index() var1.u32

  WCHAR *pchars = (WCHAR *) scratch;
  unsigned int chars_len = 0;
  for (unsigned int i = 0; i < buffer->len; i++) {
    hb_codepoint_t c = buffer->info[i].codepoint;
    buffer->info[i].utf16_index() = chars_len;
    if (likely (c < 0x10000))
      pchars[chars_len++] = c;
    else if (unlikely (c >= 0x110000))
      pchars[chars_len++] = 0xFFFD;
    else {
      pchars[chars_len++] = 0xD800 + ((c - 0x10000) >> 10);
      pchars[chars_len++] = 0xDC00 + ((c - 0x10000) & ((1 << 10) - 1));
    }
  }

  ALLOCATE_ARRAY (WCHAR, wchars, chars_len);
  ALLOCATE_ARRAY (WORD, log_clusters, chars_len);
  ALLOCATE_ARRAY (SCRIPT_CHARPROP, char_props, chars_len);

  /* On Windows, we don't care about alignment...*/
  unsigned int glyphs_size = scratch_size / (sizeof (WORD) +
					     sizeof (SCRIPT_GLYPHPROP) +
					     sizeof (int) +
					     sizeof (GOFFSET) +
					     sizeof (uint32_t));

  ALLOCATE_ARRAY (WORD, glyphs, glyphs_size);
  ALLOCATE_ARRAY (SCRIPT_GLYPHPROP, glyph_props, glyphs_size);
  ALLOCATE_ARRAY (int, advances, glyphs_size);
  ALLOCATE_ARRAY (GOFFSET, offsets, glyphs_size);
  ALLOCATE_ARRAY (uint32_t, vis_clusters, glyphs_size);


#define FALLBACK(...) \
  HB_STMT_START { \
    DEBUG_MSG (UNISCRIBE, NULL, __VA_ARGS__); \
    goto fallback; \
  } HB_STMT_END;


#define MAX_ITEMS 10

  SCRIPT_ITEM items[MAX_ITEMS + 1];
  SCRIPT_STATE bidi_state = {0};
  ULONG script_tags[MAX_ITEMS];
  int item_count;

  bidi_state.uBidiLevel = HB_DIRECTION_IS_FORWARD (buffer->props.direction) ? 0 : 1;
  bidi_state.fOverrideDirection = 1;

  hr = ScriptItemizeOpenType (wchars,
			      chars_len,
			      MAX_ITEMS,
			      NULL,
			      &bidi_state,
			      items,
			      script_tags,
			      &item_count);
  if (unlikely (FAILED (hr)))
    FALLBACK ("ScriptItemizeOpenType() failed: %d", hr);

#undef MAX_ITEMS

  int *range_char_counts = NULL;
  TEXTRANGE_PROPERTIES **range_properties = NULL;
  int range_count = 0;
  if (num_features) {
    /* XXX setup ranges */
  }

  hb_blob_t *blob = hb_face_get_blob (font->face);
  unsigned int blob_length;
  const char *blob_data = hb_blob_get_data (blob, &blob_length);
  if (unlikely (!blob_length)) {
    hb_blob_destroy (blob);
    FALLBACK ("Empty font blob");
  }

  DWORD num_fonts_installed;
  HANDLE fh = AddFontMemResourceEx ((void *) blob_data, blob_length, 0, &num_fonts_installed);
  if (unlikely (!fh)) {
    hb_blob_destroy (blob);
    FALLBACK ("AddFontMemResourceEx() failed");
  }

  /* FREE stuff, specially when taking fallback... */

  HDC hdc = GetDC (NULL); /* XXX The DC should be cached on the face I guess? */

  LOGFONTW log_font;
  populate_log_font (&log_font, hdc, font, blob);

  HFONT hfont = CreateFontIndirectW (&log_font);
  SelectObject (hdc, hfont);

  SCRIPT_CACHE script_cache = NULL;
  OPENTYPE_TAG language_tag = hb_ot_tag_from_language (buffer->props.language);

  unsigned int glyphs_offset = 0;
  unsigned int glyphs_len;
  for (unsigned int i = 0; i < item_count; i++)
  {
      unsigned int chars_offset = items[i].iCharPos;
      unsigned int item_chars_len = items[i + 1].iCharPos - chars_offset;
      OPENTYPE_TAG script_tag = script_tags[i]; /* XXX buffer->props.script */

      hr = ScriptShapeOpenType (hdc,
				&script_cache,
				&items[i].a,
				script_tag,
				language_tag,
				range_char_counts,
				range_properties,
				range_count,
				wchars + chars_offset,
				item_chars_len,
				glyphs_size - glyphs_offset,
				/* out */
				log_clusters + chars_offset,
				char_props + chars_offset,
				glyphs + glyphs_offset,
				glyph_props + glyphs_offset,
				(int *) &glyphs_len);

      if (unlikely (items[i].a.fNoGlyphIndex))
	FALLBACK ("ScriptShapeOpenType() set fNoGlyphIndex");
      if (unlikely (hr == E_OUTOFMEMORY))
      {
        buffer->ensure (buffer->allocated * 2);
	if (buffer->in_error)
	  FALLBACK ("Buffer resize failed");
	goto retry;
      }
      if (unlikely (FAILED (hr)))
	FALLBACK ("ScriptShapeOpenType() failed: %d", hr);

      hr = ScriptPlaceOpenType (hdc,
				&script_cache,
				&items[i].a,
				script_tag,
				language_tag,
				range_char_counts,
				range_properties,
				range_count,
				wchars + chars_offset,
				log_clusters + chars_offset,
				char_props + chars_offset,
				item_chars_len,
				glyphs + glyphs_offset,
				glyph_props + glyphs_offset,
				glyphs_len,
				/* out */
				advances + glyphs_offset,
				offsets + glyphs_offset,
				NULL);
      if (unlikely (FAILED (hr)))
	FALLBACK ("ScriptPlaceOpenType() failed: %d", hr);

      glyphs_offset += glyphs_len;
  }
  glyphs_len = glyphs_offset;

  ReleaseDC (NULL, hdc);
  DeleteObject (hfont);
  RemoveFontMemResourceEx (fh);

  /* Ok, we've got everything we need, now compose output buffer,
   * very, *very*, carefully! */

  /* Calculate visual-clusters.  That's what we ship. */
  for (unsigned int i = 0; i < buffer->len; i++)
    vis_clusters[log_clusters[buffer->info[i].utf16_index()]] = buffer->info[i].cluster;
  for (unsigned int i = 1; i < glyphs_len; i++)
    if (!glyph_props[i].sva.fClusterStart)
    vis_clusters[i] = vis_clusters[i - 1];

#undef utf16_index

  buffer->ensure (glyphs_len);
  if (buffer->in_error)
    FALLBACK ("Buffer in error");

#undef FALLBACK

  /* Set glyph infos */
  for (unsigned int i = 0; i < glyphs_len; i++)
  {
    hb_glyph_info_t *info = &buffer->info[i];

    info->codepoint = glyphs[i];
    info->cluster = vis_clusters[i];

    /* The rest is crap.  Let's store position info there for now. */
    info->mask = advances[i];
    info->var1.u32 = offsets[i].du;
    info->var2.u32 = offsets[i].dv;
  }

  /* Set glyph positions */
  buffer->clear_positions ();
  for (unsigned int i = 0; i < glyphs_len; i++)
  {
    hb_glyph_info_t *info = &buffer->info[i];
    hb_glyph_position_t *pos = &buffer->pos[i];

    /* TODO vertical */
    pos->x_advance = info->mask;
    pos->x_offset = info->var1.u32;
    pos->y_offset = info->var2.u32;
  }

  /* Wow, done! */
  return;
}


