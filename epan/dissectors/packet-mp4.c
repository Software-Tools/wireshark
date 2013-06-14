/* packet-mp4.c
 * routines for dissection of MP4 files
 * Copyright 2013, Martin Kaiser <martin@kaiser.cx>
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* this dissector is based on
 * ISO/IEC 14496-12 (ISO base media file format) and
 * ISO/IEC 14496-14 (MP4 file format)
 *
 * at the moment, it dissects the basic box structure and the payload of
 * some simple boxes */


#include "config.h"

#include <glib.h>
#include <epan/packet.h>

#define MAKE_TYPE_VAL(a, b, c, d)   (a)<<24 | (b)<<16 | (c)<<8 | (d)

static int proto_mp4 = -1;

static gint ett_mp4 = -1;
static gint ett_mp4_box = -1;

static int hf_mp4_box_size = -1;
static int hf_mp4_box_type_str = -1;
static int hf_mp4_full_box_ver = -1;
static int hf_mp4_ftyp_brand = -1;
static int hf_mp4_ftyp_ver = -1;
static int hf_mp4_ftyp_add_brand = -1;
static int hf_mp4_mfhd_seq_num = -1;

/* a box must at least have a 32bit len field and a 32bit type */
#define MIN_BOX_LEN 8

/* the box type is stored as four text characters
   it is in network byte order and contains only printable characters
   for our internal handling, we convert this to a 32bit value */

#define BOX_TYPE_NONE  0x0 /* used for parent_box_type of a top-level box */
#define BOX_TYPE_FTYP  MAKE_TYPE_VAL('f', 't', 'y', 'p')
#define BOX_TYPE_MFHD  MAKE_TYPE_VAL('m', 'f', 'h', 'd')
#define BOX_TYPE_MVHD  MAKE_TYPE_VAL('m', 'v', 'h', 'd')
#define BOX_TYPE_MOOV  MAKE_TYPE_VAL('m', 'o', 'o', 'v')
#define BOX_TYPE_MOOF  MAKE_TYPE_VAL('m', 'o', 'o', 'f')
#define BOX_TYPE_STBL  MAKE_TYPE_VAL('s', 't', 'b', 'l')
#define BOX_TYPE_MDIA  MAKE_TYPE_VAL('m', 'd', 'i', 'a')
#define BOX_TYPE_TRAK  MAKE_TYPE_VAL('t', 'r', 'a', 'k')
#define BOX_TYPE_TRAF  MAKE_TYPE_VAL('t', 'r', 'a', 'f')
#define BOX_TYPE_MINF  MAKE_TYPE_VAL('m', 'i', 'n', 'f')
#define BOX_TYPE_MVEX  MAKE_TYPE_VAL('m', 'v', 'e', 'x')
#define BOX_TYPE_MEHD  MAKE_TYPE_VAL('m', 'e', 'h', 'd')
#define BOX_TYPE_TREX  MAKE_TYPE_VAL('t', 'r', 'e', 'x')

static const value_string box_types[] = {
    { BOX_TYPE_FTYP, "File Type Box" },
    { BOX_TYPE_MFHD, "Movie Fragment Header Box" },
    { BOX_TYPE_MVHD, "Movie Header Box" },
    { BOX_TYPE_MOOV, "Movie Box" },
    { BOX_TYPE_MOOF, "Movie Fragment Box" },
    { BOX_TYPE_STBL, "Sample to Group Box" },
    { BOX_TYPE_MDIA, "Media Box" },
    { BOX_TYPE_TRAK, "Track Box" },
    { BOX_TYPE_TRAF, "Track Fragment Box" },
    { BOX_TYPE_MINF, "Media Information Box" },
    { BOX_TYPE_MVEX, "Movie Extends Box" },
    { BOX_TYPE_MEHD, "Movie Extends Header Box" },
    { BOX_TYPE_TREX, "Track Extends Box" },
    { 0, NULL }
};

static int
dissect_mp4_mvhd_body(tvbuff_t *tvb, guint offset, guint len _U_,
        packet_info *pinfo _U_, proto_tree *tree)
{
    guint offset_start;

    offset_start = offset;
    proto_tree_add_item(tree, hf_mp4_full_box_ver,
            tvb, offset, 1, ENC_BIG_ENDIAN);
    offset++;
    offset+=3;

    return offset-offset_start;
}

static int
dissect_mp4_mfhd_body(tvbuff_t *tvb, guint offset, guint len _U_,
        packet_info *pinfo _U_, proto_tree *tree)
{
    guint offset_start;

    offset_start = offset;
    proto_tree_add_item(tree, hf_mp4_full_box_ver,
            tvb, offset, 1, ENC_BIG_ENDIAN);
    offset++;
    offset+=3;

    proto_tree_add_item(tree, hf_mp4_mfhd_seq_num,
            tvb, offset, 4, ENC_BIG_ENDIAN);
    offset+=4;

    return offset-offset_start;
}


static int
dissect_mp4_ftyp_body(tvbuff_t *tvb, guint offset, guint len,
        packet_info *pinfo _U_, proto_tree *tree)
{
    guint offset_start;

    offset_start = offset;
    proto_tree_add_item(tree, hf_mp4_ftyp_brand,
            tvb, offset, 4, ENC_ASCII|ENC_NA);
    offset += 4;
    proto_tree_add_item(tree, hf_mp4_ftyp_ver,
            tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    while (offset-offset_start<len) {
        proto_tree_add_item(tree, hf_mp4_ftyp_add_brand,
                tvb, offset, 4, ENC_ASCII|ENC_NA);
        offset += 4;
    }

    return offset-offset_start;
}

static gint
dissect_mp4_box(guint32 parent_box_type _U_,
        tvbuff_t *tvb, guint offset, packet_info *pinfo, proto_tree *tree)
{
    guint       offset_start;
    guint       box_len;
    guint32     box_type;
    guint8     *box_type_str;
    proto_item *pi;
    proto_tree *box_tree;
    gint        ret;


    offset_start = offset;

    /* the following mechanisms are not supported for now
       - extended size (size==1, largesize parameter)
       - size==0, indicating that the box extends to the end of the file
       - extended box types */

    box_len = tvb_get_ntohl(tvb, offset);
    if (box_len<MIN_BOX_LEN)
        return -1;

    box_type = tvb_get_ntohl(tvb, offset+4);
    box_type_str = tvb_get_ephemeral_string(tvb, offset+4, 4);

    pi = proto_tree_add_text(tree, tvb, offset, box_len, "%s (%s)",
            val_to_str_const(box_type, box_types, "unknown"), box_type_str);
    box_tree = proto_item_add_subtree(pi, ett_mp4_box);

    proto_tree_add_item(box_tree, hf_mp4_box_size,
            tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(box_tree, hf_mp4_box_type_str,
            tvb, offset, 4, ENC_ASCII|ENC_NA);
    offset += 4;

    /* XXX - check parent box if supplied */
    switch (box_type) {
        case BOX_TYPE_FTYP:
            dissect_mp4_ftyp_body(tvb, offset, box_len-8, pinfo, box_tree);
            break;
        case BOX_TYPE_MVHD:
            dissect_mp4_mvhd_body(tvb, offset, box_len-8, pinfo, box_tree);
            break;
        case BOX_TYPE_MFHD:
            dissect_mp4_mfhd_body(tvb, offset, box_len-8, pinfo, box_tree);
            break;
        case BOX_TYPE_MOOV:
        case BOX_TYPE_MOOF:
        case BOX_TYPE_STBL:
        case BOX_TYPE_MDIA:
        case BOX_TYPE_TRAK:
        case BOX_TYPE_TRAF:
        case BOX_TYPE_MINF:
        case BOX_TYPE_MVEX:
            while (offset-offset_start<box_len) {
                ret = dissect_mp4_box(box_type, tvb, offset, pinfo, box_tree);
                if (ret<=0)
                    break;
                offset += ret;
            }
            break;
        default:
            break;
    }

    return box_len;
}


static int
dissect_mp4(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
    guint       offset = 0;
    guint32     box_type;
    proto_item *pi;
    proto_tree *mp4_tree;
    gint        ret;

    /* to make sure that we have an mp4 file, we check that it starts with
        a box of a known type
       this should be safe as long as the dissector is only called for
        the video/mp4 mime type
       when we read mp4 files directly, we might need stricter checks here */
    if (tvb_reported_length(tvb)<MIN_BOX_LEN)
        return 0;
    box_type = tvb_get_ntohl(tvb, 4);
    if (try_val_to_str(box_type, box_types) == NULL)
        return 0;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MP4");
    col_clear(pinfo->cinfo, COL_INFO);

    pi = proto_tree_add_protocol_format(tree, proto_mp4,
            tvb, 0, tvb_reported_length(tvb), "MP4");
    mp4_tree = proto_item_add_subtree(pi, ett_mp4);

    while (tvb_reported_length_remaining(tvb, offset) > 0) {
        ret = dissect_mp4_box(BOX_TYPE_NONE, tvb, offset, pinfo, mp4_tree);
        if (ret<=0)
            break;
        offset += ret;
    }

    return offset;
}

void
proto_register_mp4(void)
{
    static hf_register_info hf[] = {
        { &hf_mp4_box_size,
            { "Box size", "mp4.box.size", FT_UINT32, BASE_DEC,
                NULL, 0, NULL, HFILL } },
        { &hf_mp4_box_type_str,
            { "Box type", "mp4.box.type_str", FT_STRING, BASE_NONE,
                NULL, 0, NULL, HFILL } },
        { &hf_mp4_full_box_ver,
            { "Box version", "mp4.full_box.version", FT_UINT8, BASE_DEC,
                NULL, 0, NULL, HFILL } },
        { &hf_mp4_ftyp_brand,
            { "Brand", "mp4.ftyp.brand", FT_STRING, BASE_NONE,
                NULL, 0, NULL, HFILL } },
        { &hf_mp4_ftyp_ver,
            { "Version", "mp4.ftyp.version", FT_UINT32, BASE_DEC,
                NULL, 0, NULL, HFILL } },
        { &hf_mp4_ftyp_add_brand,
            { "Additional brand", "mp4.ftyp.additional_brand", FT_STRING,
                BASE_NONE, NULL, 0, NULL, HFILL } },
        { &hf_mp4_mfhd_seq_num,
            { "Sequence number", "mp4.mfhd.sequence_number", FT_UINT32,
                BASE_DEC, NULL, 0, NULL, HFILL } }
    };

    static gint *ett[] = {
        &ett_mp4,
        &ett_mp4_box
    };

    proto_mp4 = proto_register_protocol("MP4 / ISOBMFF file format", "mp4", "mp4");

    proto_register_field_array(proto_mp4, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
}

void
proto_reg_handoff_mp4(void)
{
    dissector_handle_t mp4_handle = new_create_dissector_handle(dissect_mp4, proto_mp4);
    dissector_add_string("media_type", "video/mp4", mp4_handle);
}


/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
