/*  GRAPHITE2 LICENSING

    Copyright 2010, SIL International
    All rights reserved.

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should also have received a copy of the GNU Lesser General Public
    License along with this library in the file named "LICENSE".
    If not, write to the Free Software Foundation, 51 Franklin Street, 
    Suite 500, Boston, MA 02110-1335, USA or visit their web page on the 
    internet at http://www.fsf.org/licenses/lgpl.html.

Alternatively, the contents of this file may be used under the terms of the
Mozilla Public License (http://mozilla.org/MPL) or the GNU General Public
License, as published by the Free Software Foundation, either version 2
of the License or (at your option) any later version.
*/
#include "graphite2/Segment.h"
#include "inc/UtfCodec.h"
#include "inc/Segment.h"

using namespace graphite2;

namespace 
{

  gr_segment* makeAndInitialize(const Font *font, const Face *face, uint32 script, const Features* pFeats/*must not be NULL*/, gr_encform enc, const void* pStart, size_t nChars, int dir)
  {
      if (script == 0x20202020) script = 0;
      else if ((script & 0x00FFFFFF) == 0x00202020) script = script & 0xFF000000;
      else if ((script & 0x0000FFFF) == 0x00002020) script = script & 0xFFFF0000;
      else if ((script & 0x000000FF) == 0x00000020) script = script & 0xFFFFFF00;
      // if (!font) return NULL;
      Segment* pRes=new Segment(nChars, face, script, dir);

      pRes->read_text(face, pFeats, enc, pStart, nChars);
      if (!pRes->runGraphite())
      {
        delete pRes;
        return NULL;
      }
      // run the line break passes
      // run the substitution passes
      pRes->prepare_pos(font);
      // run the positioning passes
      pRes->finalise(font);

      return static_cast<gr_segment*>(pRes);
  }


}


template <typename utf_iter>
inline size_t count_unicode_chars(utf_iter first, const utf_iter last, const void **error)
{
	size_t n_chars = 0;
	uint32 usv = 0;

	if (last)
	{
		for (;first != last; ++first, ++n_chars)
			if ((usv = *first) == 0 || first.error()) break;
	}
	else
	{
		while ((usv = *first) != 0 && !first.error())
		{
			++first;
			++n_chars;
		}
	}

	if (error)	*error = first.error() ? first : 0;
	return n_chars;
}

extern "C" {

size_t gr_count_unicode_characters(gr_encform enc, const void* buffer_begin, const void* buffer_end/*don't go on or past end, If NULL then ignored*/, const void** pError)   //Also stops on nul. Any nul is not in the count
{
	assert(buffer_begin);

	switch (enc)
	{
	case gr_utf8:	return count_unicode_chars<utf8::const_iterator>(buffer_begin, buffer_end, pError); break;
	case gr_utf16:	return count_unicode_chars<utf16::const_iterator>(buffer_begin, buffer_end, pError); break;
	case gr_utf32:	return count_unicode_chars<utf32::const_iterator>(buffer_begin, buffer_end, pError); break;
	default:		return 0;
	}
}


gr_segment* gr_make_seg(const gr_font *font, const gr_face *face, gr_uint32 script, const gr_feature_val* pFeats, gr_encform enc, const void* pStart, size_t nChars, int dir)
{
	const gr_feature_val * tmp_feats = 0;
    if (pFeats == 0)
        pFeats = tmp_feats = static_cast<const gr_feature_val*>(face->theSill().cloneFeatures(0));
    gr_segment * seg = makeAndInitialize(font, face, script, pFeats, enc, pStart, nChars, dir);
    delete tmp_feats;

    return seg;
}


void gr_seg_destroy(gr_segment* p)
{
    delete p;
}


float gr_seg_advance_X(const gr_segment* pSeg/*not NULL*/)
{
    assert(pSeg);
    return pSeg->advance().x;
}


float gr_seg_advance_Y(const gr_segment* pSeg/*not NULL*/)
{
    assert(pSeg);
    return pSeg->advance().y;
}


unsigned int gr_seg_n_cinfo(const gr_segment* pSeg/*not NULL*/)
{
    assert(pSeg);
    return pSeg->charInfoCount();
}


const gr_char_info* gr_seg_cinfo(const gr_segment* pSeg/*not NULL*/, unsigned int index/*must be <number_of_CharInfo*/)
{
    assert(pSeg);
    return static_cast<const gr_char_info*>(pSeg->charinfo(index));
}

unsigned int gr_seg_n_slots(const gr_segment* pSeg/*not NULL*/)
{
    assert(pSeg);
    return pSeg->slotCount();
}

const gr_slot* gr_seg_first_slot(gr_segment* pSeg/*not NULL*/)
{
    assert(pSeg);
    return static_cast<const gr_slot*>(pSeg->first());
}

const gr_slot* gr_seg_last_slot(gr_segment* pSeg/*not NULL*/)
{
    assert(pSeg);
    return static_cast<const gr_slot*>(pSeg->last());
}

void gr_seg_justify(gr_segment* pSeg/*not NULL*/, gr_slot* pSlot/*not NULL*/, const gr_font *pFont, double width, enum gr_justFlags flags, gr_slot *pFirst, gr_slot *pLast)
{
    assert(pSeg);
    assert(pSlot);
    pSeg->justify(pSlot, pFont, float(width), justFlags(flags), pFirst, pLast);
}

} // extern "C"
