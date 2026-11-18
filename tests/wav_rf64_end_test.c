/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software ; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation ; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY ; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program ; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** Verify that the container headers of seekable WAV and RF64 PCM output
** stay consistent with the end of the valid audio data :
**
**  - normal close, early stop, empty files and a short final block must
**    all leave a header whose declared data length matches the frames
**    that can actually be read back after reopening ;
**  - a not-yet-flushed header while the file is still open is a
**    transient condition and must be distinguishable from a persistent
**    header error ;
**  - appending extends the valid end, overwriting in place keeps the
**    old tail, and only an explicit truncate shrinks the valid range ;
**  - header updates must not move the audio payload, and a sub-frame
**    tail must not be counted in the frame count / duration.
*/

#include "sfconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if (defined (WIN32) || defined (_WIN32))
#include <io.h>
#endif

#include	<sndfile.h>

#include	"utils.h"

#define	SAMPLE_RATE		8000
#define	CHANNELS		2
/* 16 bit PCM : 2 bytes per sample, one frame = one sample per channel. */
#define	BLOCK_ALIGN		(CHANNELS * 2)
#define	BUFFER_FRAMES	1024

/* Parsed view of the on-disk RIFF/RF64 container header. */
typedef struct
{	int64_t	riff_declared ;	/* declared RIFF size (RIFF field, or ds64 riffSize for RF64) */
	int64_t	data_declared ;	/* declared payload length of the 'data' chunk */
	int64_t	data_offset ;	/* absolute file offset of the 'data' chunk payload */
	int64_t	sample_count ;	/* ds64 sample count ; -1 for plain WAV (no ds64 chunk) */
	int64_t	riff_size_pos ;	/* file offset of the field holding the RIFF size */
	int64_t	data_size_pos ;	/* file offset of the field holding the data length */
	int		size_width ;	/* width in bytes of the two size fields above */
} ContainerInfo ;

static void	normal_close_test		(const char *filename, int format) ;
static void	early_stop_test			(const char *filename, int format) ;
static void	empty_file_test			(const char *filename, int format) ;
static void	partial_frame_tail_test	(const char *filename, int format) ;
static void	transient_header_test	(const char *filename, int format) ;
static void	append_extends_end_test	(const char *filename, int format) ;
static void	overwrite_keeps_tail_test (const char *filename, int format) ;
static void	explicit_truncate_test	(const char *filename, int format) ;

int
main (int argc, char *argv [])
{	int		do_all = 0 ;
	int		test_count = 0 ;
	char	name [64] ;
	const char * ext ;

	if (argc != 2)
	{	printf ("Usage : %s <test>\n", argv [0]) ;
		printf ("    Where <test> is one of the following:\n") ;
		printf ("           wav  - test WAV container header vs valid audio end\n") ;
		printf ("           rf64 - test RF64 container header vs valid audio end\n") ;
		printf ("           all  - perform all tests\n") ;
		exit (1) ;
		} ;

	do_all = ! strcmp (argv [1], "all") ;

	if (do_all || ! strcmp (argv [1], "wav"))
	{	ext = "wav" ;
		snprintf (name, sizeof name, "end_normal.%s", ext) ;
		normal_close_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_early.%s", ext) ;
		early_stop_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_empty.%s", ext) ;
		empty_file_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_subframe.%s", ext) ;
		partial_frame_tail_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_transient.%s", ext) ;
		transient_header_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_append.%s", ext) ;
		append_extends_end_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_overwrite.%s", ext) ;
		overwrite_keeps_tail_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_truncate.%s", ext) ;
		explicit_truncate_test (name, SF_FORMAT_WAV | SF_FORMAT_PCM_16) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "rf64"))
	{	ext = "rf64" ;
		snprintf (name, sizeof name, "end_normal.%s", ext) ;
		normal_close_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_early.%s", ext) ;
		early_stop_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_empty.%s", ext) ;
		empty_file_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_subframe.%s", ext) ;
		partial_frame_tail_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_transient.%s", ext) ;
		transient_header_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_append.%s", ext) ;
		append_extends_end_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_overwrite.%s", ext) ;
		overwrite_keeps_tail_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		snprintf (name, sizeof name, "end_truncate.%s", ext) ;
		explicit_truncate_test (name, SF_FORMAT_RF64 | SF_FORMAT_PCM_16) ;
		test_count++ ;
		} ;

	if (test_count == 0)
	{	printf ("************************************\n") ;
		printf ("*  No '%s' test defined.\n", argv [1]) ;
		printf ("************************************\n") ;
		return 1 ;
		} ;

	return 0 ;
} /* main */

/*==============================================================================
** Deterministic sample pattern (no randomness, no external input).
*/

static void
fill_pattern (short *data, int items, int seed)
{	int k ;

	for (k = 0 ; k < items ; k++)
		data [k] = (short) (((k * 31 + seed * 977) % 65536) - 32768) ;
} /* fill_pattern */

static void
compare_buffer_or_die (const short *expected, const short *actual, int items, int line_num)
{	int k ;

	for (k = 0 ; k < items ; k++)
		exit_if_true (expected [k] != actual [k],
				"\n\nLine %d : sample %d : expected %d, got %d.\n",
				line_num, k, expected [k], actual [k]) ;
} /* compare_buffer_or_die */

/*==============================================================================
** Minimal RIFF/RF64 header parser, independent of libsndfile, so the
** assertions check what is actually persisted on disk.
*/

static uint32_t
get_le32 (const unsigned char *b)
{	return (uint32_t) b [0] | ((uint32_t) b [1] << 8)
			| ((uint32_t) b [2] << 16) | ((uint32_t) b [3] << 24) ;
} /* get_le32 */

static uint64_t
get_le64 (const unsigned char *b)
{	return (uint64_t) get_le32 (b) | ((uint64_t) get_le32 (b + 4) << 32) ;
} /* get_le64 */

/* Returns 0 on success, -1 if the file is not (yet) a parseable RIFF/RF64 file. */
static int
parse_container (const char *filename, ContainerInfo *info)
{	FILE			*f ;
	unsigned char	hdr [12] ;
	int64_t			ds64_data = -1, ds64_data_pos = -1, ds64_riff_pos = -1 ;

	memset (info, 0, sizeof *info) ;
	info->sample_count = -1 ;

	if ((f = fopen (filename, "rb")) == NULL)
		return -1 ;

	if (fread (hdr, 1, 12, f) != 12)
	{	fclose (f) ;
		return -1 ;
		} ;
	if (memcmp (hdr, "RIFF", 4) != 0 && memcmp (hdr, "RF64", 4) != 0)
	{	fclose (f) ;
		return -1 ;
		} ;
	if (memcmp (hdr + 8, "WAVE", 4) != 0)
	{	fclose (f) ;
		return -1 ;
		} ;

	info->riff_declared = get_le32 (hdr + 4) ;
	info->riff_size_pos = 4 ;
	info->size_width = 4 ;

	for (;;)
	{	unsigned char	chdr [8] ;
		uint32_t		csize ;
		long			payload ;

		if (fread (chdr, 1, 8, f) != 8)
		{	fclose (f) ;
			return -1 ;	/* ran out of chunks without finding 'data' */
			} ;
		csize = get_le32 (chdr + 4) ;
		payload = ftell (f) ;

		if (memcmp (chdr, "ds64", 4) == 0)
		{	unsigned char body [28] ;

			if (csize < 28 || fread (body, 1, 28, f) != 28)
			{	fclose (f) ;
				return -1 ;
				} ;
			info->riff_declared = (int64_t) get_le64 (body) ;
			ds64_data = (int64_t) get_le64 (body + 8) ;
			info->sample_count = (int64_t) get_le64 (body + 16) ;
			ds64_riff_pos = payload ;
			ds64_data_pos = payload + 8 ;
			info->riff_size_pos = ds64_riff_pos ;
			info->size_width = 8 ;
			if (fseek (f, payload + (long) csize + (csize & 1), SEEK_SET))
			{	fclose (f) ;
				return -1 ;
				} ;
			continue ;
			} ;

		if (memcmp (chdr, "data", 4) == 0)
		{	info->data_offset = payload ;
			if (csize == 0xffffffff && ds64_data >= 0)
			{	info->data_declared = ds64_data ;
				info->data_size_pos = ds64_data_pos ;
				}
			else
			{	info->data_declared = (int64_t) csize ;
				info->data_size_pos = payload - 4 ;
				} ;
			fclose (f) ;
			return 0 ;
			} ;

		if (fseek (f, payload + (long) csize + (csize & 1), SEEK_SET))
		{	fclose (f) ;
			return -1 ;
			} ;
		} ;
} /* parse_container */

static void
parse_container_or_die (const char *filename, ContainerInfo *info, int line_num)
{	exit_if_true (parse_container (filename, info) != 0,
			"\n\nLine %d : '%s' is not a parseable RIFF/RF64 container.\n",
			line_num, filename) ;
} /* parse_container_or_die */

static void
put_le (unsigned char *b, int64_t value, int width)
{	int k ;

	for (k = 0 ; k < width ; k++)
		b [k] = (unsigned char) ((uint64_t) value >> (8 * k)) ;
} /* put_le */

/* Rewrite the declared data length on disk (and the enclosing RIFF size to
** match), then extend the file with the corresponding extra payload bytes.
** This deterministically creates a container whose declared data length is
** not a whole number of frames, something the write API refuses to produce.
*/
static void
patch_declared_data_length_or_die (const char *filename, int64_t new_declared, int line_num)
{	ContainerInfo	info ;
	unsigned char	buf [8] ;
	FILE			*f ;
	int64_t			delta, k ;

	parse_container_or_die (filename, &info, line_num) ;
	delta = new_declared - info.data_declared ;

	exit_if_true (delta < 0,
			"\n\nLine %d : '%s' : can only grow the declared data length.\n",
			line_num, filename) ;

	exit_if_true ((f = fopen (filename, "r+b")) == NULL,
			"\n\nLine %d : '%s' : fopen (r+b) failed.\n", line_num, filename) ;

	put_le (buf, new_declared, info.size_width) ;
	exit_if_true (fseek (f, info.data_size_pos, SEEK_SET) != 0
					|| fwrite (buf, 1, info.size_width, f) != (size_t) info.size_width,
			"\n\nLine %d : '%s' : failed to patch the data length field.\n",
			line_num, filename) ;

	put_le (buf, info.riff_declared + delta, info.size_width) ;
	exit_if_true (fseek (f, info.riff_size_pos, SEEK_SET) != 0
					|| fwrite (buf, 1, info.size_width, f) != (size_t) info.size_width,
			"\n\nLine %d : '%s' : failed to patch the RIFF size field.\n",
			line_num, filename) ;

	exit_if_true (fseek (f, 0, SEEK_END) != 0,
			"\n\nLine %d : '%s' : fseek to end failed.\n", line_num, filename) ;
	for (k = 0 ; k < delta ; k++)
		exit_if_true (fputc (0x5A, f) != 0x5A,
				"\n\nLine %d : '%s' : failed to append payload byte.\n",
				line_num, filename) ;

	fclose (f) ;
} /* patch_declared_data_length_or_die */

/*==============================================================================
** Reopen the file and check that the persisted header, the reported frame
** count (and hence the duration) and the frames that can actually be read
** all agree with the expected end of the valid audio.
*/

static void
verify_container_or_die (const char *filename, int64_t expected_frames, int line_num)
{	static short	buffer [BUFFER_FRAMES * CHANNELS] ;
	ContainerInfo	info ;
	SNDFILE			*file ;
	SF_INFO			sfinfo ;
	sf_count_t		total, count ;

	parse_container_or_die (filename, &info, line_num) ;

	exit_if_true (info.data_declared != expected_frames * BLOCK_ALIGN,
			"\n\nLine %d : '%s' : header declares %" PRId64 " data bytes, "
			"expected %" PRId64 " (%" PRId64 " frames).\n",
			line_num, filename, info.data_declared,
			expected_frames * BLOCK_ALIGN, expected_frames) ;

	exit_if_true (info.riff_declared + 8 != file_length (filename),
			"\n\nLine %d : '%s' : declared RIFF size %" PRId64 " + 8 != "
			"file length %" PRId64 ".\n",
			line_num, filename, info.riff_declared, file_length (filename)) ;

	exit_if_true (info.data_offset + info.data_declared != file_length (filename),
			"\n\nLine %d : '%s' : declared data ends at %" PRId64 " but the "
			"file is %" PRId64 " bytes long.\n",
			line_num, filename, info.data_offset + info.data_declared,
			file_length (filename)) ;

	/* RF64 : the ds64 sample count must agree with the data length. */
	if (info.sample_count >= 0)
		exit_if_true (info.sample_count != expected_frames,
				"\n\nLine %d : '%s' : ds64 declares %" PRId64 " samples, "
				"expected %" PRId64 ".\n",
				line_num, filename, info.sample_count, expected_frames) ;

	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, line_num) ;

	exit_if_true (sfinfo.frames != expected_frames,
			"\n\nLine %d : '%s' : reopened file reports %" PRId64 " frames, "
			"expected %" PRId64 ".\n",
			line_num, filename, sfinfo.frames, expected_frames) ;
	exit_if_true (sfinfo.samplerate != SAMPLE_RATE || sfinfo.channels != CHANNELS,
			"\n\nLine %d : '%s' : reopened file has samplerate %d / channels %d, "
			"expected %d / %d.\n",
			line_num, filename, sfinfo.samplerate, sfinfo.channels,
			SAMPLE_RATE, CHANNELS) ;

	/* The duration is the number of readable complete frames divided by
	** the sample rate ; count the frames that can really be read. */
	total = 0 ;
	while ((count = sf_readf_short (file, buffer, BUFFER_FRAMES)) > 0)
		total += count ;

	exit_if_true (total != expected_frames,
			"\n\nLine %d : '%s' : %" PRId64 " frames readable, expected "
			"%" PRId64 " (duration %.6f s vs %.6f s).\n",
			line_num, filename, total, expected_frames,
			(double) total / sfinfo.samplerate,
			(double) expected_frames / sfinfo.samplerate) ;

	sf_close (file) ;
} /* verify_container_or_die */

static void
verify_content_or_die (const char *filename, const short *expected, int64_t frames, int line_num)
{	static short	got [BUFFER_FRAMES * CHANNELS] ;
	SNDFILE		*file ;
	SF_INFO		sfinfo ;
	sf_count_t	done = 0, count ;

	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, line_num) ;

	while ((count = sf_readf_short (file, got, BUFFER_FRAMES)) > 0)
	{	compare_buffer_or_die (expected + done * CHANNELS, got,
				(int) (count * CHANNELS), line_num) ;
		done += count ;
		} ;

	exit_if_true (done != frames,
			"\n\nLine %d : '%s' : read %" PRId64 " frames while verifying "
			"content, expected %" PRId64 ".\n",
			line_num, filename, done, frames) ;

	sf_close (file) ;
} /* verify_content_or_die */

static void
open_write_or_die (const char *filename, int format, SNDFILE **file, SF_INFO *sfinfo, int line_num)
{	memset (sfinfo, 0, sizeof *sfinfo) ;
	sfinfo->samplerate	= SAMPLE_RATE ;
	sfinfo->channels	= CHANNELS ;
	sfinfo->format		= format ;

	*file = test_open_file_or_die (filename, SFM_WRITE, sfinfo, SF_TRUE, line_num) ;
} /* open_write_or_die */

/*==============================================================================
** Normal close : the header must describe exactly what was written.
*/

static void
normal_close_test (const char *filename, int format)
{	static short	data [1000 * CHANNELS] ;
	SNDFILE		*file ;
	SF_INFO		sfinfo ;

	print_test_name ("normal_close_test", filename) ;

	fill_pattern (data, 1000 * CHANNELS, 1) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	test_writef_short_or_die (file, 0, data, 600, __LINE__) ;
	test_writef_short_or_die (file, 1, data + 600 * CHANNELS, 400, __LINE__) ;
	sf_close (file) ;

	verify_container_or_die (filename, 1000, __LINE__) ;
	verify_content_or_die (filename, data, 1000, __LINE__) ;

	unlink (filename) ;
	puts ("ok") ;
} /* normal_close_test */

/*==============================================================================
** Early stop : writing is stopped (last block short) and the file closed.
** The header must reflect what was actually written, not what might have
** been planned.
*/

static void
early_stop_test (const char *filename, int format)
{	static short	data [640 * CHANNELS] ;
	SNDFILE		*file ;
	SF_INFO		sfinfo ;

	print_test_name ("early_stop_test", filename) ;

	fill_pattern (data, 640 * CHANNELS, 2) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	/* Two full 256 frame blocks, then a short final block of 128 frames. */
	test_writef_short_or_die (file, 0, data, 256, __LINE__) ;
	test_writef_short_or_die (file, 1, data + 256 * CHANNELS, 256, __LINE__) ;
	test_writef_short_or_die (file, 2, data + 512 * CHANNELS, 128, __LINE__) ;
	sf_close (file) ;

	verify_container_or_die (filename, 640, __LINE__) ;
	verify_content_or_die (filename, data, 640, __LINE__) ;

	unlink (filename) ;
	puts ("ok") ;
} /* early_stop_test */

/*==============================================================================
** Empty file : nothing written, header must declare zero frames.
*/

static void
empty_file_test (const char *filename, int format)
{	ContainerInfo	info ;
	SNDFILE			*file ;
	SF_INFO			sfinfo ;

	print_test_name ("empty_file_test", filename) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	sf_close (file) ;

	verify_container_or_die (filename, 0, __LINE__) ;

	/* No payload bytes beyond the declared (empty) data chunk. */
	parse_container_or_die (filename, &info, __LINE__) ;
	exit_if_true (file_length (filename) != info.data_offset,
			"\n\nLine %d : '%s' : empty file is %" PRId64 " bytes but the "
			"data chunk starts at %" PRId64 ".\n",
			__LINE__, filename, file_length (filename), info.data_offset) ;

	unlink (filename) ;
	puts ("ok") ;
} /* empty_file_test */

/*==============================================================================
** Sub-frame tail : the write API refuses writes that are not a whole number
** of frames, and a file whose declared data length has a sub-frame remainder
** must be reopened with the complete-frame floor : the tail bytes must never
** be counted towards the frame count or the duration, and a header rewrite
** must neither absorb them into a frame nor move the payload.
*/

static void
partial_frame_tail_test (const char *filename, int format)
{	static short	data [511 * CHANNELS + 1] ;
	static short	got [BUFFER_FRAMES * CHANNELS] ;
	ContainerInfo	before, after ;
	SNDFILE			*file ;
	SF_INFO			sfinfo ;
	sf_count_t		count, total ;

	print_test_name ("partial_frame_tail_test", filename) ;

	fill_pattern (data, 511 * CHANNELS + 1, 3) ;

	/* A write of 511 frames plus one orphan sample must be rejected as a
	** whole : nothing of it may be persisted. */
	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	count = sf_write_short (file, data, 511 * CHANNELS + 1) ;
	exit_if_true (count != 0,
			"\n\nLine %d : '%s' : sub-frame write of %" PRId64 " items should "
			"fail, but %" PRId64 " items were written.\n",
			__LINE__, filename, (sf_count_t) (511 * CHANNELS + 1), count) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : '%s' : sub-frame write should raise an error.\n",
			__LINE__, filename) ;

	/* The rejected write leaves no trace : an aligned write right after
	** still produces a clean 511 frame file. */
	test_writef_short_or_die (file, 0, data, 511, __LINE__) ;
	sf_close (file) ;
	verify_container_or_die (filename, 511, __LINE__) ;

	/* Give the on-disk header a two byte (half frame) tail, as a crashed
	** or foreign writer could leave behind. */
	parse_container_or_die (filename, &before, __LINE__) ;
	patch_declared_data_length_or_die (filename, 511 * BLOCK_ALIGN + 2, __LINE__) ;

	/* Reopened : 511 frames (511/8000 s of audio).  The declared length
	** implies 511 complete frames ; the half frame tail must not extend
	** the duration. */
	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;
	exit_if_true (sfinfo.frames != 511,
			"\n\nLine %d : '%s' : reopened file reports %" PRId64 " frames "
			"(%.6f s), expected 511 (%.6f s) ; a sub-frame tail must not "
			"count towards the duration.\n",
			__LINE__, filename, sfinfo.frames,
			(double) sfinfo.frames / sfinfo.samplerate, 511.0 / SAMPLE_RATE) ;

	total = 0 ;
	while ((count = sf_readf_short (file, got, BUFFER_FRAMES)) > 0)
		total += count ;
	exit_if_true (total != 511,
			"\n\nLine %d : '%s' : %" PRId64 " frames readable, expected 511.\n",
			__LINE__, filename, total) ;
	sf_close (file) ;

	/* A read/write open forces a header rewrite on close : the update must
	** keep the declared tail where it is (no payload movement) and must
	** still not round the sub-frame tail up into the frame count. */
	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_RDWR, &sfinfo, SF_TRUE, __LINE__) ;
	sf_close (file) ;

	parse_container_or_die (filename, &after, __LINE__) ;
	exit_if_true (after.data_offset != before.data_offset,
			"\n\nLine %d : '%s' : data payload moved from %" PRId64 " to "
			"%" PRId64 " during header update.\n",
			__LINE__, filename, before.data_offset, after.data_offset) ;
	exit_if_true (after.data_declared / BLOCK_ALIGN != 511,
			"\n\nLine %d : '%s' : header update turned the declared length "
			"%" PRId64 " into %" PRId64 " frames, expected 511.\n",
			__LINE__, filename, after.data_declared,
			after.data_declared / BLOCK_ALIGN) ;

	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;
	exit_if_true (sfinfo.frames != 511,
			"\n\nLine %d : '%s' : after header update the file reports "
			"%" PRId64 " frames, expected 511.\n",
			__LINE__, filename, sfinfo.frames) ;
	sf_close (file) ;

	unlink (filename) ;
	puts ("ok") ;
} /* partial_frame_tail_test */

/*==============================================================================
** Transient vs persistent : while the file is still open the on-disk header
** may lag behind (it is only refreshed on close).  That transient state is
** not an error ; only a header that is still wrong after close is a
** persistent failure.
*/

static void
transient_header_test (const char *filename, int format)
{	static short	data [500 * CHANNELS] ;
	ContainerInfo	transient ;
	SNDFILE			*file ;
	SF_INFO			sfinfo, current ;

	print_test_name ("transient_header_test", filename) ;

	fill_pattern (data, 500 * CHANNELS, 4) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	test_writef_short_or_die (file, 0, data, 500, __LINE__) ;

	/* The library itself is in a clean state ... */
	exit_if_true (sf_error (file) != SF_ERR_NO_ERROR,
			"\n\nLine %d : '%s' : unexpected error while writing : %s.\n",
			__LINE__, filename, sf_strerror (file)) ;

	/* ... and its live counters already know the true end of the audio. */
	memset (&current, 0, sizeof current) ;
	sf_command (file, SFC_GET_CURRENT_SF_INFO, &current, sizeof current) ;
	exit_if_true (current.frames != 500,
			"\n\nLine %d : '%s' : live frame count is %" PRId64 ", expected 500.\n",
			__LINE__, filename, current.frames) ;

	/* Push the audio bytes out ; the header is deliberately NOT refreshed. */
	sf_write_sync (file) ;

	/* The persisted header may still declare an earlier (transient) count.
	** That is tolerable as long as it is a plausible earlier state : never
	** negative, never beyond the final end of the audio. */
	if (parse_container (filename, &transient) == 0)
		exit_if_true (transient.data_declared < 0
						|| transient.data_declared > 500 * BLOCK_ALIGN,
				"\n\nLine %d : '%s' : transient header declares %" PRId64 " data "
				"bytes, outside [0, %d] ; that is corruption, not a pending "
				"header flush.\n",
				__LINE__, filename, transient.data_declared, 500 * BLOCK_ALIGN) ;

	sf_close (file) ;

	/* After close the header must be exactly right ; anything less is a
	** persistent error, not a transient one. */
	verify_container_or_die (filename, 500, __LINE__) ;
	verify_content_or_die (filename, data, 500, __LINE__) ;

	unlink (filename) ;
	puts ("ok") ;
} /* transient_header_test */

/*==============================================================================
** Append : seeking to the end and writing more extends the valid end of
** the audio ; the header must grow to match.
*/

static void
append_extends_end_test (const char *filename, int format)
{	static short	expected [700 * CHANNELS] ;
	ContainerInfo	before, after ;
	SNDFILE			*file ;
	SF_INFO			sfinfo ;

	print_test_name ("append_extends_end_test", filename) ;

	fill_pattern (expected, 400 * CHANNELS, 5) ;
	fill_pattern (expected + 400 * CHANNELS, 300 * CHANNELS, 6) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	test_writef_short_or_die (file, 0, expected, 400, __LINE__) ;
	sf_close (file) ;

	parse_container_or_die (filename, &before, __LINE__) ;

	/* Reopen read/write and append at the end of the valid audio. */
	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_RDWR, &sfinfo, SF_TRUE, __LINE__) ;
	test_seek_or_die (file, 0, SEEK_END, 400, CHANNELS, __LINE__) ;
	test_writef_short_or_die (file, 1, expected + 400 * CHANNELS, 300, __LINE__) ;
	sf_close (file) ;

	/* The valid end moved from 400 to 700 frames. */
	verify_container_or_die (filename, 700, __LINE__) ;

	/* Growing the header must not move the audio payload. */
	parse_container_or_die (filename, &after, __LINE__) ;
	exit_if_true (after.data_offset != before.data_offset,
			"\n\nLine %d : '%s' : data payload moved from %" PRId64 " to "
			"%" PRId64 " when the header grew.\n",
			__LINE__, filename, before.data_offset, after.data_offset) ;

	verify_content_or_die (filename, expected, 700, __LINE__) ;

	unlink (filename) ;
	puts ("ok") ;
} /* append_extends_end_test */

/*==============================================================================
** In-place overwrite : seeking back and overwriting must NOT truncate the
** file ; the old tail past the overwritten region stays valid.
*/

static void
overwrite_keeps_tail_test (const char *filename, int format)
{	static short	expected [600 * CHANNELS] ;
	ContainerInfo	before, after ;
	SNDFILE			*file ;
	SF_INFO			sfinfo ;

	print_test_name ("overwrite_keeps_tail_test", filename) ;

	/* Original content, frames [200, 300) replaced by a second pattern. */
	fill_pattern (expected, 600 * CHANNELS, 7) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	test_writef_short_or_die (file, 0, expected, 600, __LINE__) ;
	sf_close (file) ;

	parse_container_or_die (filename, &before, __LINE__) ;

	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_RDWR, &sfinfo, SF_TRUE, __LINE__) ;
	test_seek_or_die (file, 200, SEEK_SET, 200, CHANNELS, __LINE__) ;
	fill_pattern (expected + 200 * CHANNELS, 100 * CHANNELS, 8) ;
	test_writef_short_or_die (file, 1, expected + 200 * CHANNELS, 100, __LINE__) ;
	sf_close (file) ;

	/* Frames [300, 600) were not rewritten : the valid end must stay at
	** 600 frames, the file must not shrink to 300. */
	verify_container_or_die (filename, 600, __LINE__) ;

	/* The header rewrite on close must not have moved the payload. */
	parse_container_or_die (filename, &after, __LINE__) ;
	exit_if_true (after.data_offset != before.data_offset,
			"\n\nLine %d : '%s' : data payload moved from %" PRId64 " to "
			"%" PRId64 " during in-place overwrite.\n",
			__LINE__, filename, before.data_offset, after.data_offset) ;

	/* Old head, new middle, old tail. */
	verify_content_or_die (filename, expected, 600, __LINE__) ;

	unlink (filename) ;
	puts ("ok") ;
} /* overwrite_keeps_tail_test */

/*==============================================================================
** Explicit truncate : only an explicit truncate may shrink the valid audio
** range, and the header must then agree with the new, earlier end.
*/

static void
explicit_truncate_test (const char *filename, int format)
{	static short	expected [600 * CHANNELS] ;
	ContainerInfo	info ;
	SNDFILE			*file ;
	SF_INFO			sfinfo ;
	sf_count_t		len ;

	print_test_name ("explicit_truncate_test", filename) ;

	fill_pattern (expected, 600 * CHANNELS, 9) ;

	open_write_or_die (filename, format, &file, &sfinfo, __LINE__) ;
	test_writef_short_or_die (file, 0, expected, 600, __LINE__) ;
	sf_close (file) ;

	memset (&sfinfo, 0, sizeof sfinfo) ;
	file = test_open_file_or_die (filename, SFM_RDWR, &sfinfo, SF_TRUE, __LINE__) ;

	len = 250 ;
	exit_if_true (sf_command (file, SFC_FILE_TRUNCATE, &len, sizeof len) != 0,
			"\n\nLine %d : '%s' : SFC_FILE_TRUNCATE failed.\n",
			__LINE__, filename) ;
	sf_close (file) ;

	/* The valid range shrank to 250 frames ... */
	verify_container_or_die (filename, 250, __LINE__) ;

	/* ... and the file really ends there. */
	parse_container_or_die (filename, &info, __LINE__) ;
	exit_if_true (file_length (filename) != info.data_offset + 250 * BLOCK_ALIGN,
			"\n\nLine %d : '%s' : truncated file is %" PRId64 " bytes, expected "
			"%" PRId64 ".\n",
			__LINE__, filename, file_length (filename),
			info.data_offset + 250 * BLOCK_ALIGN) ;

	/* The surviving prefix is untouched. */
	verify_content_or_die (filename, expected, 250, __LINE__) ;

	unlink (filename) ;
	puts ("ok") ;
} /* explicit_truncate_test */
