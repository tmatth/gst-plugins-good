/* GStreamer plugin for forward error correction
 * Copyright (C) 2017 Pexip
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Author: Mikhail Fludkov <misha@pexip.com>
 */

#include "rtpstoragestream.h"

static RtpStorageItem *
rtp_storage_item_new (GstBuffer * buffer, guint8 pt, guint16 seq)
{
  RtpStorageItem *ret = g_slice_new0 (RtpStorageItem);
  ret->buffer = buffer;
  ret->pt = pt;
  ret->seq = seq;
  return ret;
}

static void
rtp_storage_item_free (RtpStorageItem * item)
{
  g_assert (item->buffer != NULL);
  gst_buffer_unref (item->buffer);
  g_slice_free (RtpStorageItem, item);
}

static gint
rtp_storage_item_compare (gconstpointer a, gconstpointer b, gpointer userdata)
{
  gint seq_diff = gst_rtp_buffer_compare_seqnum (
      ((RtpStorageItem const *) a)->seq, ((RtpStorageItem const *) b)->seq);

  if (seq_diff >= 0)
    return 0;

  return 1;
}

static void
rtp_storage_stream_resize (RtpStorageStream * stream, GstClockTime size_time)
{
  GList *it;
  guint i, too_old_buffers_num = 0;

  g_assert (GST_CLOCK_TIME_IS_VALID (stream->max_arrival_time));
  g_assert (GST_CLOCK_TIME_IS_VALID (size_time));
  g_assert_cmpint (size_time, >, 0);

  /* Iterating from oldest sequence numbers to newest */
  for (i = 0, it = stream->queue.tail; it; it = it->prev, ++i) {
    RtpStorageItem *item = it->data;
    GstClockTime arrival_time = GST_BUFFER_DTS_OR_PTS (item->buffer);
    if (GST_CLOCK_TIME_IS_VALID (arrival_time)) {
      if (stream->max_arrival_time - arrival_time > size_time) {
        too_old_buffers_num = i + 1;
      } else
        break;
    }
  }

  for (guint i = 0; i < too_old_buffers_num; ++i) {
    RtpStorageItem *item = g_queue_pop_tail (&stream->queue);
    rtp_storage_item_free (item);
  }
}

void
rtp_storage_stream_resize_and_add_item (RtpStorageStream * stream,
    GstClockTime size_time, GstBuffer * buffer, guint8 pt, guint16 seq)
{
  GstClockTime arrival_time = GST_BUFFER_DTS_OR_PTS (buffer);

  if (G_LIKELY (GST_CLOCK_TIME_IS_VALID (arrival_time))) {
    if (G_LIKELY (GST_CLOCK_TIME_IS_VALID (stream->max_arrival_time)))
      stream->max_arrival_time = MAX (stream->max_arrival_time, arrival_time);
    else
      stream->max_arrival_time = arrival_time;

    rtp_storage_stream_resize (stream, size_time);
    rtp_storage_stream_add_item (stream, buffer, pt, seq);
  } else {
    rtp_storage_stream_add_item (stream, buffer, pt, seq);
  }
}

RtpStorageStream *
rtp_storage_stream_new (guint32 ssrc)
{
  RtpStorageStream *ret = g_slice_new0 (RtpStorageStream);
  ret->max_arrival_time = GST_CLOCK_TIME_NONE;
  ret->ssrc = ssrc;
  g_mutex_init (&ret->stream_lock);
  return ret;
}

void
rtp_storage_stream_free (RtpStorageStream * stream)
{
  STREAM_LOCK (stream);
  while (stream->queue.length)
    rtp_storage_item_free (g_queue_pop_tail (&stream->queue));
  STREAM_UNLOCK (stream);
  g_mutex_clear (&stream->stream_lock);
  g_slice_free (RtpStorageStream, stream);
}

void
rtp_storage_stream_add_item (RtpStorageStream * stream, GstBuffer * buffer,
    guint8 pt, guint16 seq)
{
  RtpStorageItem *item = rtp_storage_item_new (buffer, pt, seq);
  GList *sibling = g_queue_find_custom (&stream->queue, item,
      (GCompareFunc) rtp_storage_item_compare);

  g_queue_insert_before (&stream->queue, sibling, item);
}

GstBufferList *
rtp_storage_stream_get_packets_for_recovery (RtpStorageStream * stream,
    guint8 pt_fec, guint16 lost_seq)
{
  guint ret_length = 0;
  GList *end = NULL;
  GList *start = NULL;
  gboolean saw_fec = TRUE;      /* To initialize the start pointer in the loop below */

  /* Looking for media stream chunk with FEC packets at the end, which could
   * can have the lost packet. For example:
   *
   *   |#10 FEC|  |#9 FEC|  |#8| ... |#6|  |#5 FEC|  |#4 FEC|  |#3 FEC|  |#2|  |#1|  |#0|
   *
   * Say @lost_seq = 7. Want to return bufferlist with packets [#6 : #10]. Other
   * packets are not relevant for recovery of packet 7.
   *
   * Or the lost packet can be in the storage. In that case single packet is returned.
   * It can happen if:
   * - it could have arrived right after it was considered lost (more of a corner case)
   * - it was recovered together with the other lost packet (most likely)
   */
  for (GList * it = stream->queue.tail; it; it = it->prev) {
    RtpStorageItem *item = it->data;
    gboolean found_end = FALSE;

    /* Is the buffer we lost in the storage? */
    if (item->seq == lost_seq) {
      start = it;
      end = it;
      ret_length = 1;
      break;
    }

    if (pt_fec == item->pt) {
      gint seq_diff = gst_rtp_buffer_compare_seqnum (lost_seq, item->seq);

      if (seq_diff >= 0) {
        if (it->prev) {
          gboolean media_next =
              pt_fec != ((RtpStorageItem *) it->prev->data)->pt;
          found_end = media_next;
        } else
          found_end = TRUE;
      }
      saw_fec = TRUE;
    } else if (saw_fec) {
      saw_fec = FALSE;
      start = it;
      ret_length = 0;
    }

    ++ret_length;
    if (found_end) {
      end = it;
      break;
    }
  }

  if (end && !start)
    start = end;

  if (start && end) {
    GstBufferList *ret = gst_buffer_list_new_sized (ret_length);

    for (GList * it = start; it != end->prev; it = it->prev)
      gst_buffer_list_add (ret,
          gst_buffer_ref (((RtpStorageItem *) it->data)->buffer));
    return ret;
  }

  return NULL;
}

GstBuffer *
rtp_storage_stream_get_redundant_packet (RtpStorageStream * stream,
    guint16 lost_seq)
{
  for (GList * it = stream->queue.head; it; it = it->next) {
    RtpStorageItem *item = it->data;
    if (item->seq == lost_seq)
      return gst_buffer_ref (item->buffer);
  }
  return NULL;
}
