/*
** Copyright (C) 2026 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/*
** Sample addressing versus frame addressing on multi-channel interleaved
** PCM files.
**
** These tests pin down the contract that the item (sample) based API
** (sf_read_short / sf_read_int / ...), the frame based API (sf_seek /
** sf_readf_*) and the byte based API (sf_read_raw) all drive one single
** shared read cursor, measured in frames :
**
**  - sample_frame_cursor_consistency_test :
**      Seeking to a frame and reading N frames must leave the cursor in
**      exactly the same place as reading N * channels items, and both
**      must deliver the same channel-ordered data.
**
**  - partial_frame_semantics_test :
**      A short read at the file boundary is a legal partial read and
**      always returns a whole number of frames. An item or byte count
**      that would land in the middle of a frame is an error, never a
**      partial frame : it returns zero, sets the error indicator and
**      leaves the cursor and the user buffer untouched.
**
**  - out_of_bounds_seek_test :
**      Seeking before the start or past the end of the file fails,
**      leaves the cursor unmoved and does not corrupt subsequent reads.
**
**  - shared_state_channel_order_test :
**      All positioning and reading interfaces share the same read
**      state. Interleaving them must preserve channel order : no
**      channel may be lost, duplicated or shifted, even across failed
**      seek/read attempts.
*/

#include "sfconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#else
#include "sf_unistd.h"
#endif

#include <sndfile.h>

#include "utils.h"

#define FRAME_COUNT		300
#define MAX_CHANNELS	4
#define BUFFER_LEN		4096
#define SENTINEL_BYTE	0x21
#define SENTINEL_INT	0x21212121
#define SENTINEL_SHORT	0x2121

static void	sample_frame_cursor_consistency_test	(int format, const char *ext) ;
static void	partial_frame_semantics_test			(void) ;
static void	out_of_bounds_seek_test					(void) ;
static void	shared_state_channel_order_test			(void) ;

int
main (void)
{	/* Run the consistency test on both little-endian and big-endian PCM. */
	sample_frame_cursor_consistency_test (SF_FORMAT_WAV | SF_FORMAT_PCM_16, "wav") ;
	sample_frame_cursor_consistency_test (SF_FORMAT_AIFF | SF_FORMAT_PCM_16, "aiff") ;

	partial_frame_semantics_test () ;
	out_of_bounds_seek_test () ;
	shared_state_channel_order_test () ;

	return 0 ;
} /* main */

/*============================================================================================
**	Helpers.
*/

/* Deterministic, channel-ordered sample value. Each (frame, channel) pair
** maps to a unique non-zero value, so any loss, duplication or reordering
** of channels is detected. The + 1 keeps every sample non-zero so that the
** zero-fill checks at end of file stay meaningful.
*/
static int
expected_sample (sf_count_t frame, int channel, int channels)
{	return (int) (frame * channels + channel + 1) ;
} /* expected_sample */

static void
make_test_file (const char *filename, int format, int channels)
{	static short data [FRAME_COUNT * MAX_CHANNELS] ;
	SF_INFO sfinfo ;
	SNDFILE *file ;
	sf_count_t k ;
	int ch ;

	for (k = 0 ; k < FRAME_COUNT ; k++)
		for (ch = 0 ; ch < channels ; ch++)
			data [k * channels + ch] = (short) expected_sample (k, ch, channels) ;

	sf_info_setup (&sfinfo, format, 48000, channels) ;

	file = test_open_file_or_die (filename, SFM_WRITE, &sfinfo, SF_FALSE, __LINE__) ;
	test_writef_short_or_die (file, 0, data, FRAME_COUNT, __LINE__) ;
	sf_close (file) ;
} /* make_test_file */

/* All test files are PCM_16, so int reads deliver the 16 bit sample
** scaled up to the full 32 bit range.
*/
static void
check_int_frames_or_die (const int *data, sf_count_t start_frame, int frame_count, int channels, int line_num)
{	int k ;

	for (k = 0 ; k < frame_count * channels ; k++)
	{	int expected = expected_sample (start_frame + k / channels, k % channels, channels) * 0x10000 ;
		if (data [k] != expected)
		{	printf ("\n\nLine %d : data [%d] = %d, expected %d (frame %" PRId64 ", channel %d).\n\n",
					line_num, k, data [k], expected, start_frame + k / channels, k % channels) ;
			exit (1) ;
			} ;
		} ;
} /* check_int_frames_or_die */

static void
check_short_frames_or_die (const short *data, sf_count_t start_frame, int frame_count, int channels, int line_num)
{	int k ;

	for (k = 0 ; k < frame_count * channels ; k++)
	{	int expected = expected_sample (start_frame + k / channels, k % channels, channels) ;
		if (data [k] != expected)
		{	printf ("\n\nLine %d : data [%d] = %d, expected %d (frame %" PRId64 ", channel %d).\n\n",
					line_num, k, data [k], expected, start_frame + k / channels, k % channels) ;
			exit (1) ;
			} ;
		} ;
} /* check_short_frames_or_die */

static void
check_zero_or_die (const int *data, int count, int line_num)
{	int k ;

	for (k = 0 ; k < count ; k++)
		if (data [k] != 0)
		{	printf ("\n\nLine %d : data [%d] = %d, expected zero-fill.\n\n", line_num, k, data [k]) ;
			exit (1) ;
			} ;
} /* check_zero_or_die */

static void
check_int_sentinel_or_die (const int *data, int count, int line_num)
{	int k ;

	for (k = 0 ; k < count ; k++)
		if (data [k] != SENTINEL_INT)
		{	printf ("\n\nLine %d : data [%d] = 0x%x, buffer should be untouched (0x%x).\n\n",
					line_num, k, data [k], SENTINEL_INT) ;
			exit (1) ;
			} ;
} /* check_int_sentinel_or_die */

static void
check_short_sentinel_or_die (const short *data, int count, int line_num)
{	int k ;

	for (k = 0 ; k < count ; k++)
		if (data [k] != SENTINEL_SHORT)
		{	printf ("\n\nLine %d : data [%d] = 0x%x, buffer should be untouched (0x%x).\n\n",
					line_num, k, data [k], SENTINEL_SHORT) ;
			exit (1) ;
			} ;
} /* check_short_sentinel_or_die */

/*============================================================================================
**	Sample positioning, frame positioning and the read cursor must agree.
*/

static void
sample_frame_cursor_consistency_test (int format, const char *ext)
{	static int ibuf [BUFFER_LEN], ibuf2 [BUFFER_LEN] ;
	static short sbuf [BUFFER_LEN] ;
	unsigned char raw [64] ;
	SF_INFO sfinfo ;
	SNDFILE *file ;
	sf_count_t count ;
	char filename [256] ;
	int channels, k ;

	for (channels = 1 ; channels <= MAX_CHANNELS ; channels++)
	{	snprintf (filename, sizeof (filename), "sfs_consistency_%d.%s", channels, ext) ;
		print_test_name (__func__, filename) ;

		make_test_file (filename, format, channels) ;

		sf_info_clear (&sfinfo) ;
		file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;
		exit_if_true (sfinfo.channels != channels,
				"\n\nLine %d : Channel count %d, expected %d.\n\n", __LINE__, sfinfo.channels, channels) ;
		exit_if_true (sfinfo.frames != FRAME_COUNT,
				"\n\nLine %d : Frame count %" PRId64 ", expected %d.\n\n", __LINE__, sfinfo.frames, FRAME_COUNT) ;

		/* The read cursor starts at frame zero. */
		test_read_write_position_or_die (file, __LINE__, 0, 0, -1) ;

		/* Frame-addressed positioning followed by a frame-addressed read. */
		test_seek_or_die (file, 17, SEEK_SET, 17, channels, __LINE__) ;
		count = sf_readf_int (file, ibuf, 10) ;
		exit_if_true (count != 10,
				"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 10.\n\n", __LINE__, count) ;
		test_read_write_position_or_die (file, __LINE__, 0, 27, -1) ;
		check_int_frames_or_die (ibuf, 17, 10, channels, __LINE__) ;

		/* A sample-addressed read of the same span from the same frame
		** position must land on the same cursor and deliver the same
		** channel-ordered data.
		*/
		test_seek_or_die (file, 17, SEEK_SET, 17, channels, __LINE__) ;
		count = sf_read_int (file, ibuf2, 10 * channels) ;
		exit_if_true (count != 10 * channels,
				"\n\nLine %d : sf_read_int returned %" PRId64 ", expected %d.\n\n", __LINE__, count, 10 * channels) ;
		test_read_write_position_or_die (file, __LINE__, 0, 27, -1) ;
		compare_int_or_die (ibuf, ibuf2, 10 * channels, __LINE__) ;

		/* SEEK_CUR and SEEK_END positioning track the same cursor. */
		test_seek_or_die (file, -5, SEEK_CUR, 22, channels, __LINE__) ;
		test_seek_or_die (file, -13, SEEK_END, FRAME_COUNT - 13, channels, __LINE__) ;
		test_seek_or_die (file, 3, SEEK_CUR, FRAME_COUNT - 10, channels, __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT - 10, -1) ;

		/* All read interfaces share the one read cursor. */
		test_seek_or_die (file, 100, SEEK_SET, 100, channels, __LINE__) ;

		count = sf_read_short (file, sbuf, channels) ;
		exit_if_true (count != channels,
				"\n\nLine %d : sf_read_short returned %" PRId64 ", expected %d.\n\n", __LINE__, count, channels) ;
		test_read_write_position_or_die (file, __LINE__, 0, 101, -1) ;
		check_short_frames_or_die (sbuf, 100, 1, channels, __LINE__) ;

		count = sf_read_int (file, ibuf, 2 * channels) ;
		exit_if_true (count != 2 * channels,
				"\n\nLine %d : sf_read_int returned %" PRId64 ", expected %d.\n\n", __LINE__, count, 2 * channels) ;
		test_read_write_position_or_die (file, __LINE__, 0, 103, -1) ;
		check_int_frames_or_die (ibuf, 101, 2, channels, __LINE__) ;

		count = sf_readf_int (file, ibuf, 1) ;
		exit_if_true (count != 1,
				"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 1.\n\n", __LINE__, count) ;
		test_read_write_position_or_die (file, __LINE__, 0, 104, -1) ;
		check_int_frames_or_die (ibuf, 103, 1, channels, __LINE__) ;

		/* The raw interface shares the same cursor. WAV PCM_16 data is
		** little-endian on disk, so the channel order inside the frame is
		** checked byte for byte, independently of host endianness.
		*/
		if ((format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV)
		{	test_seek_or_die (file, 200, SEEK_SET, 200, channels, __LINE__) ;
			memset (raw, 0, sizeof (raw)) ;
			count = sf_read_raw (file, raw, channels * (sf_count_t) sizeof (short)) ;
			exit_if_true (count != channels * (sf_count_t) sizeof (short),
					"\n\nLine %d : sf_read_raw returned %" PRId64 ", expected %d.\n\n",
					__LINE__, count, channels * (int) sizeof (short)) ;
			test_read_write_position_or_die (file, __LINE__, 0, 201, -1) ;
			for (k = 0 ; k < channels ; k++)
			{	int value = expected_sample (200, k, channels) ;
				exit_if_true (raw [2 * k] != (value & 0xFF) || raw [2 * k + 1] != ((value >> 8) & 0xFF),
						"\n\nLine %d : raw bytes of channel %d are out of order.\n\n", __LINE__, k) ;
				} ;
			} ;

		sf_close (file) ;
		unlink (filename) ;
		printf ("ok\n") ;
		} ;

	return ;
} /* sample_frame_cursor_consistency_test */

/*============================================================================================
**	Legal partial reads at the file boundary always return a whole number
**	of frames. A sample or byte offset that lands in the middle of a frame
**	is an error and must not masquerade as a partial frame.
*/

static void
partial_frame_semantics_test (void)
{	static const int channel_counts [] = { 2, 3 } ;
	static int ibuf [BUFFER_LEN] ;
	static short sbuf [BUFFER_LEN] ;
	unsigned char raw [64] ;
	SF_INFO sfinfo ;
	SNDFILE *file ;
	sf_count_t count ;
	char filename [256] ;
	unsigned ct ;
	int channels ;

	for (ct = 0 ; ct < ARRAY_LEN (channel_counts) ; ct++)
	{	channels = channel_counts [ct] ;

		snprintf (filename, sizeof (filename), "sfs_partial_%d.wav", channels) ;
		print_test_name (__func__, filename) ;

		make_test_file (filename, SF_FORMAT_WAV | SF_FORMAT_PCM_16, channels) ;

		/*------------------------------------------------------------------
		**	Legal partial reads near the file boundary.
		*/
		sf_info_clear (&sfinfo) ;
		file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;

		/* A short item read at the boundary returns a whole number of
		** frames, leaves the rest of the buffer untouched and is not an
		** error.
		*/
		test_seek_or_die (file, FRAME_COUNT - 1, SEEK_SET, FRAME_COUNT - 1, channels, __LINE__) ;
		memset (ibuf, SENTINEL_BYTE, sizeof (ibuf)) ;
		count = sf_read_int (file, ibuf, 4 * channels) ;
		exit_if_true (count != channels,
				"\n\nLine %d : sf_read_int returned %" PRId64 ", expected %d (one whole frame).\n\n",
				__LINE__, count, channels) ;
		exit_if_true (count % channels != 0,
				"\n\nLine %d : partial read of %" PRId64 " items is not frame aligned.\n\n", __LINE__, count) ;
		exit_if_true (sf_error (file) != SF_ERR_NO_ERROR,
				"\n\nLine %d : legal partial read raised an error.\n\n", __LINE__) ;
		check_int_frames_or_die (ibuf, FRAME_COUNT - 1, 1, channels, __LINE__) ;
		check_int_sentinel_or_die (ibuf + channels, 3 * channels, __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT, -1) ;

		/* Same for a short frame read. */
		test_seek_or_die (file, FRAME_COUNT - 2, SEEK_SET, FRAME_COUNT - 2, channels, __LINE__) ;
		memset (ibuf, SENTINEL_BYTE, sizeof (ibuf)) ;
		count = sf_readf_int (file, ibuf, 10) ;
		exit_if_true (count != 2,
				"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 2.\n\n", __LINE__, count) ;
		exit_if_true (sf_error (file) != SF_ERR_NO_ERROR,
				"\n\nLine %d : legal partial read raised an error.\n\n", __LINE__) ;
		check_int_frames_or_die (ibuf, FRAME_COUNT - 2, 2, channels, __LINE__) ;
		check_int_sentinel_or_die (ibuf + 2 * channels, 8 * channels, __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT, -1) ;

		/* Reading at end of file returns zero items and zero-fills. */
		memset (ibuf, SENTINEL_BYTE, sizeof (ibuf)) ;
		count = sf_read_int (file, ibuf, channels) ;
		exit_if_true (count != 0,
				"\n\nLine %d : sf_read_int at EOF returned %" PRId64 ", expected 0.\n\n", __LINE__, count) ;
		check_zero_or_die (ibuf, channels, __LINE__) ;

		sf_close (file) ;

		/*------------------------------------------------------------------
		**	Sample offsets that land in the middle of a frame are errors,
		**	not partial frames.
		*/
		sf_info_clear (&sfinfo) ;
		file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;

		test_seek_or_die (file, 50, SEEK_SET, 50, channels, __LINE__) ;

		/* An item count that ends in the middle of a frame. */
		memset (ibuf, SENTINEL_BYTE, sizeof (ibuf)) ;
		count = sf_read_int (file, ibuf, channels + 1) ;
		exit_if_true (count != 0,
				"\n\nLine %d : mid-frame sf_read_int returned %" PRId64 ", expected 0.\n\n", __LINE__, count) ;
		exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
				"\n\nLine %d : mid-frame sf_read_int did not raise an error.\n\n", __LINE__) ;
		check_int_sentinel_or_die (ibuf, 2 * channels, __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, 50, -1) ;

		/* A single-sample read of a multi-channel file. */
		memset (sbuf, SENTINEL_BYTE, sizeof (sbuf)) ;
		count = sf_read_short (file, sbuf, 1) ;
		exit_if_true (count != 0,
				"\n\nLine %d : mid-frame sf_read_short returned %" PRId64 ", expected 0.\n\n", __LINE__, count) ;
		exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
				"\n\nLine %d : mid-frame sf_read_short did not raise an error.\n\n", __LINE__) ;
		check_short_sentinel_or_die (sbuf, channels, __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, 50, -1) ;

		/* A byte count that ends in the middle of a frame. */
		memset (raw, SENTINEL_BYTE, sizeof (raw)) ;
		count = sf_read_raw (file, raw, sizeof (short)) ;
		exit_if_true (count != 0,
				"\n\nLine %d : mid-frame sf_read_raw returned %" PRId64 ", expected 0.\n\n", __LINE__, count) ;
		exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
				"\n\nLine %d : mid-frame sf_read_raw did not raise an error.\n\n", __LINE__) ;
		exit_if_true (raw [0] != SENTINEL_BYTE || raw [1] != SENTINEL_BYTE,
				"\n\nLine %d : mid-frame sf_read_raw modified the buffer.\n\n", __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, 50, -1) ;

		/* The failed reads above must not have consumed, duplicated or
		** reordered anything : the frame at the old cursor is still intact.
		*/
		count = sf_readf_int (file, ibuf, 1) ;
		exit_if_true (count != 1,
				"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 1.\n\n", __LINE__, count) ;
		check_int_frames_or_die (ibuf, 50, 1, channels, __LINE__) ;
		test_read_write_position_or_die (file, __LINE__, 0, 51, -1) ;

		sf_close (file) ;
		unlink (filename) ;
		printf ("ok\n") ;
		} ;

	return ;
} /* partial_frame_semantics_test */

/*============================================================================================
**	Out-of-bounds positioning fails and leaves the shared read cursor
**	untouched.
*/

static void
out_of_bounds_seek_test (void)
{	static int ibuf [BUFFER_LEN] ;
	const int channels = 2 ;
	const char *filename = "sfs_out_of_bounds.wav" ;
	SF_INFO sfinfo ;
	SNDFILE *file ;
	sf_count_t count ;

	print_test_name (__func__, filename) ;

	make_test_file (filename, SF_FORMAT_WAV | SF_FORMAT_PCM_16, channels) ;

	sf_info_clear (&sfinfo) ;
	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;

	/* Position at a known good frame first. */
	test_seek_or_die (file, 42, SEEK_SET, 42, channels, __LINE__) ;

	/* Seeking past the end fails and leaves the cursor alone. */
	exit_if_true (sf_seek (file, FRAME_COUNT + 1, SEEK_SET) != -1,
			"\n\nLine %d : sf_seek past end of file should have failed.\n\n", __LINE__) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : sf_seek past end of file did not raise an error.\n\n", __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, 42, -1) ;

	/* Seeking before the start fails and leaves the cursor alone. */
	exit_if_true (sf_seek (file, -1, SEEK_SET) != -1,
			"\n\nLine %d : sf_seek before start of file should have failed.\n\n", __LINE__) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : sf_seek before start of file did not raise an error.\n\n", __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, 42, -1) ;

	/* SEEK_END and SEEK_CUR variants of out-of-bounds positioning. */
	exit_if_true (sf_seek (file, 1, SEEK_END) != -1,
			"\n\nLine %d : sf_seek (SEEK_END) past end of file should have failed.\n\n", __LINE__) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : sf_seek (SEEK_END) past end of file did not raise an error.\n\n", __LINE__) ;

	exit_if_true (sf_seek (file, -(FRAME_COUNT + 1), SEEK_END) != -1,
			"\n\nLine %d : sf_seek (SEEK_END) before start of file should have failed.\n\n", __LINE__) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : sf_seek (SEEK_END) before start of file did not raise an error.\n\n", __LINE__) ;

	exit_if_true (sf_seek (file, FRAME_COUNT, SEEK_CUR) != -1,
			"\n\nLine %d : sf_seek (SEEK_CUR) past end of file should have failed.\n\n", __LINE__) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : sf_seek (SEEK_CUR) past end of file did not raise an error.\n\n", __LINE__) ;

	test_read_write_position_or_die (file, __LINE__, 0, 42, -1) ;

	/* Seeking exactly to end of file is legal ; reads return nothing. */
	test_seek_or_die (file, FRAME_COUNT, SEEK_SET, FRAME_COUNT, channels, __LINE__) ;
	memset (ibuf, SENTINEL_BYTE, sizeof (ibuf)) ;
	count = sf_readf_int (file, ibuf, 4) ;
	exit_if_true (count != 0,
			"\n\nLine %d : sf_readf_int at EOF returned %" PRId64 ", expected 0.\n\n", __LINE__, count) ;
	check_zero_or_die (ibuf, 4 * channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT, -1) ;

	/* The failed seeks did not corrupt the shared read state. */
	test_seek_or_die (file, 42, SEEK_SET, 42, channels, __LINE__) ;
	count = sf_readf_int (file, ibuf, 2) ;
	exit_if_true (count != 2,
			"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 2.\n\n", __LINE__, count) ;
	check_int_frames_or_die (ibuf, 42, 2, channels, __LINE__) ;

	sf_close (file) ;
	unlink (filename) ;
	printf ("ok\n") ;

	return ;
} /* out_of_bounds_seek_test */

/*============================================================================================
**	All positioning and reading interfaces share the same read state.
**	Interleaving them must preserve channel order, even across failed
**	seek/read attempts.
*/

static void
shared_state_channel_order_test (void)
{	static int ibuf [BUFFER_LEN] ;
	static short sbuf [BUFFER_LEN] ;
	unsigned char raw [64] ;
	const int channels = 3 ;
	const char *filename = "sfs_shared_state.wav" ;
	SF_INFO sfinfo ;
	SNDFILE *file ;
	sf_count_t count ;
	int k ;

	print_test_name (__func__, filename) ;

	make_test_file (filename, SF_FORMAT_WAV | SF_FORMAT_PCM_16, channels) ;

	sf_info_clear (&sfinfo) ;
	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_FALSE, __LINE__) ;

	/* Frame positioning, then a frame read. */
	test_seek_or_die (file, 10, SEEK_SET, 10, channels, __LINE__) ;
	count = sf_readf_int (file, ibuf, 1) ;
	exit_if_true (count != 1,
			"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 1.\n\n", __LINE__, count) ;
	check_int_frames_or_die (ibuf, 10, 1, channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, 11, -1) ;

	/* Relative positioning, then a sample-addressed read. */
	test_seek_or_die (file, 5, SEEK_CUR, 16, channels, __LINE__) ;
	count = sf_read_int (file, ibuf, 2 * channels) ;
	exit_if_true (count != 2 * channels,
			"\n\nLine %d : sf_read_int returned %" PRId64 ", expected %d.\n\n", __LINE__, count, 2 * channels) ;
	check_int_frames_or_die (ibuf, 16, 2, channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, 18, -1) ;

	/* A different sample type shares the same cursor. */
	count = sf_read_short (file, sbuf, channels) ;
	exit_if_true (count != channels,
			"\n\nLine %d : sf_read_short returned %" PRId64 ", expected %d.\n\n", __LINE__, count, channels) ;
	check_short_frames_or_die (sbuf, 18, 1, channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, 19, -1) ;

	/* So does the raw interface. */
	memset (raw, 0, sizeof (raw)) ;
	count = sf_read_raw (file, raw, channels * (sf_count_t) sizeof (short)) ;
	exit_if_true (count != channels * (sf_count_t) sizeof (short),
			"\n\nLine %d : sf_read_raw returned %" PRId64 ", expected %d.\n\n",
			__LINE__, count, channels * (int) sizeof (short)) ;
	test_read_write_position_or_die (file, __LINE__, 0, 20, -1) ;
	for (k = 0 ; k < channels ; k++)
	{	int value = expected_sample (19, k, channels) ;
		exit_if_true (raw [2 * k] != (value & 0xFF) || raw [2 * k + 1] != ((value >> 8) & 0xFF),
				"\n\nLine %d : raw bytes of channel %d are out of order.\n\n", __LINE__, k) ;
		} ;

	/* Positioning from the end of the file. */
	test_seek_or_die (file, -20, SEEK_END, FRAME_COUNT - 20, channels, __LINE__) ;
	count = sf_readf_int (file, ibuf, 3) ;
	exit_if_true (count != 3,
			"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 3.\n\n", __LINE__, count) ;
	check_int_frames_or_die (ibuf, FRAME_COUNT - 20, 3, channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT - 17, -1) ;

	/* A mid-frame sample offset must not desynchronise the shared cursor. */
	count = sf_read_int (file, ibuf, 2 * channels + 1) ;
	exit_if_true (count != 0,
			"\n\nLine %d : mid-frame sf_read_int returned %" PRId64 ", expected 0.\n\n", __LINE__, count) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : mid-frame sf_read_int did not raise an error.\n\n", __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT - 17, -1) ;

	/* Nor must an out-of-bounds seek. */
	exit_if_true (sf_seek (file, FRAME_COUNT + 100, SEEK_SET) != -1,
			"\n\nLine %d : sf_seek past end of file should have failed.\n\n", __LINE__) ;
	exit_if_true (sf_error (file) == SF_ERR_NO_ERROR,
			"\n\nLine %d : sf_seek past end of file did not raise an error.\n\n", __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT - 17, -1) ;

	/* The very next frame is read with channels in order : no loss,
	** duplication or shift caused by the failed operations above.
	*/
	count = sf_readf_int (file, ibuf, 1) ;
	exit_if_true (count != 1,
			"\n\nLine %d : sf_readf_int returned %" PRId64 ", expected 1.\n\n", __LINE__, count) ;
	check_int_frames_or_die (ibuf, FRAME_COUNT - 17, 1, channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT - 16, -1) ;

	/* And the rest of the file streams past in order. */
	count = sf_read_int (file, ibuf, channels * 16) ;
	exit_if_true (count != channels * 16,
			"\n\nLine %d : sf_read_int returned %" PRId64 ", expected %d.\n\n", __LINE__, count, channels * 16) ;
	check_int_frames_or_die (ibuf, FRAME_COUNT - 16, 16, channels, __LINE__) ;
	test_read_write_position_or_die (file, __LINE__, 0, FRAME_COUNT, -1) ;

	sf_close (file) ;
	unlink (filename) ;
	printf ("ok\n") ;

	return ;
} /* shared_state_channel_order_test */
