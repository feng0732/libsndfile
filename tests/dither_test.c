/*
** Copyright (C) 2003-2024 Erik de Castro Lopo <erikd@mega-nerd.com>
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


#include	"sfconfig.h"
#include	<stdio.h>
#include	<stdlib.h>
#if HAVE_UNISTD_H
#include	<unistd.h>
#else
#include	"sf_unistd.h"
#endif
#include	<string.h>
#include	<math.h>

#include	<sndfile.h>

#include	"utils.h"

#define	BUFFER_LEN		(1 << 14)
#define LOG_BUFFER_SIZE	1024

static void	dither_test (const char *filename, int filetype) ;
static void	write_semantics_tests (void) ;

/* Force the start of this buffer to be double aligned. Sparc-solaris will
** choke if its not.
*/
static	short	data_out [BUFFER_LEN] ;

int
main (int argc, char *argv [])
{	int		do_all = 0 ;
	int		test_count = 0 ;

	if (argc != 2)
	{	printf ("Usage : %s <test>\n", argv [0]) ;
		printf ("    Where <test> is one of the following:\n") ;
		printf ("           wav  - test WAV file peak chunk\n") ;
		printf ("           aiff - test AIFF file PEAK chunk\n") ;
		printf ("           all  - perform all tests\n") ;
		exit (1) ;
		} ;

	do_all = ! strcmp (argv [1], "all") ;

	if (do_all || ! strcmp (argv [1], "wav"))
	{	dither_test ("dither.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_U8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "aiff"))
	{	dither_test ("dither.aiff", SF_FORMAT_AIFF | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "au"))
	{	dither_test ("dither.au", SF_FORMAT_AU | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "svx"))
	{	dither_test ("dither.svx", SF_FORMAT_SVX | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "nist"))
	{	dither_test ("dither.nist", SF_FORMAT_NIST | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "paf"))
	{	dither_test ("dither.paf", SF_FORMAT_PAF | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "ircam"))
	{	dither_test ("dither.ircam", SF_FORMAT_IRCAM | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "voc"))
	{	dither_test ("dither.voc", SF_FORMAT_VOC | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "w64"))
	{	dither_test ("dither.w64", SF_FORMAT_W64 | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "mat4"))
	{	dither_test ("dither.mat4", SF_FORMAT_MAT4 | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "mat5"))
	{	dither_test ("dither.mat5", SF_FORMAT_MAT5 | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (do_all || ! strcmp (argv [1], "pvf"))
	{	dither_test ("dither.pvf", SF_FORMAT_PVF | SF_FORMAT_PCM_S8) ;
		test_count++ ;
		} ;

	if (test_count == 0)
	{	printf ("Mono : ************************************\n") ;
		printf ("Mono : *  No '%s' test defined.\n", argv [1]) ;
		printf ("Mono : ************************************\n") ;
		return 1 ;
		} ;

	if (do_all)
		write_semantics_tests () ;

	return 0 ;
} /* main */


/*============================================================================================
**	Here are the test functions.
*/

static void
dither_test (const char *filename, int filetype)
{	SNDFILE			*file ;
	SF_INFO			sfinfo ;
	SF_DITHER_INFO	dither ;

	print_test_name ("dither_test", filename) ;

	sfinfo.samplerate	= 44100 ;
	sfinfo.format		= filetype ;
	sfinfo.channels		= 1 ;
	sfinfo.frames		= 0 ;

	/* Some build configurations do not support every container/bit depth
	** combination exercised here; skip those instead of aborting the whole
	** test program. */
	file = sf_open (filename, SFM_WRITE, &sfinfo) ;
	if (file == NULL)
	{	puts ("skipped (format not supported by this build)") ;
		return ;
		} ;

	/* Check for old version of the dither API. */
	if (sf_command (file, SFC_SET_DITHER_ON_WRITE, NULL, SF_TRUE) == 0)
	{	printf ("\n\nLine %d: Should have an error here but don't.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	memset (&dither, 0, sizeof (dither)) ;
	dither.type = SFD_WHITE ;
	dither.level = 0 ;

	if (sf_command (file, SFC_SET_DITHER_ON_WRITE, &dither, sizeof (dither)) != 0)
	{	printf ("\n\nLine %d: sf_command (SFC_SET_DITHER_ON_WRITE) returned error : %s\n\n",
			__LINE__, sf_strerror (file)) ;
		exit (1) ;
		} ;

	/* Write data to file. */
	test_write_short_or_die (file, 0, data_out, BUFFER_LEN, __LINE__) ;
	test_seek_or_die (file, 0, SEEK_SET, 0, sfinfo.channels, __LINE__) ;

	sf_close (file) ;

	file = test_open_file_or_die (filename, SFM_READ, &sfinfo, SF_TRUE, __LINE__) ;

	if (sfinfo.frames != BUFFER_LEN)
	{	printf ("\n\nLine %d: Bad frame count %d (should be %d)\n\n", __LINE__, (int) sfinfo.frames, BUFFER_LEN) ;
		} ;

	sf_close (file) ;
	/*-unlink (filename) ;-*/

	puts ("ok") ;
} /* dither_test */

/*------------------------------------------------------------------------------------------
**	Helpers for the write semantics tests.
*/

static SNDFILE *
open_wav (const char *filename, SF_INFO *sfinfo, int subtype, int channels)
{
	memset (sfinfo, 0, sizeof (*sfinfo)) ;
	sfinfo->samplerate	= 48000 ;
	sfinfo->channels	= channels ;
	sfinfo->format		= SF_FORMAT_WAV | subtype ;

	return test_open_file_or_die (filename, SFM_WRITE, sfinfo, SF_TRUE, __LINE__) ;
} /* open_wav */

static void
set_dither_or_die (SNDFILE *file, int type, double level, int custom_level, int line)
{	SF_DITHER_INFO dither ;

	memset (&dither, 0, sizeof (dither)) ;
	dither.type = type | (custom_level ? SFD_CUSTOM_LEVEL : 0) ;
	dither.level = level ;

	if (sf_command (file, SFC_SET_DITHER_ON_WRITE, &dither, sizeof (dither)) != 0)
	{	printf ("\n\nLine %d: sf_command (SFC_SET_DITHER_ON_WRITE) failed : %s\n\n",
			line, sf_strerror (file)) ;
		exit (1) ;
		} ;
} /* set_dither_or_die */

static sf_count_t
read_raw_file (const char *filename, SF_INFO *sfinfo, void *buffer, int bytes_per_sample)
{	SNDFILE *file ;
	SF_INFO read_info ;

	memcpy (&read_info, sfinfo, sizeof (read_info)) ;
	file = test_open_file_or_die (filename, SFM_READ, &read_info, SF_TRUE, __LINE__) ;
	sf_count_t items = sf_read_raw (file, buffer, read_info.frames * read_info.channels * bytes_per_sample) ;
	sf_close (file) ;

	return items ;
} /* read_raw_file */

/* White dither on a genuine bit depth reduction (short -> 8 bit) must alter
** some samples while preserving the requested frame count. */
static void
test_white_alters_output (void)
{	const int		frames = BUFFER_LEN / 2, channels = 2 ;
	static short	input [BUFFER_LEN] ;
	static unsigned char ref [BUFFER_LEN], dithered [BUFFER_LEN] ;
	SF_INFO			info ;
	int				k, differences = 0, max_deviation = 0 ;

	print_test_name ("test_white_alters_output", "wav u8") ;

	for (k = 0 ; k < frames * channels ; k++)
		input [k] = (short) ((k * 257) % 2000 - 1000) ;

	SNDFILE *ref_file = open_wav ("dither_ref_u8.wav", &info, SF_FORMAT_PCM_U8, channels) ;
	test_writef_short_or_die (ref_file, 0, input, frames, __LINE__) ;
	sf_close (ref_file) ;

	SNDFILE *dith_file = open_wav ("dither_white_u8.wav", &info, SF_FORMAT_PCM_U8, channels) ;
	set_dither_or_die (dith_file, SFD_WHITE, 0.0, 0, __LINE__) ;
	test_writef_short_or_die (dith_file, 0, input, frames, __LINE__) ;
	sf_close (dith_file) ;

	if (read_raw_file ("dither_ref_u8.wav", &info, ref, 1) != frames * channels)
	{	puts ("\n\nBad reference read length.\n") ;
		exit (1) ;
		} ;
	if (read_raw_file ("dither_white_u8.wav", &info, dithered, 1) != frames * channels)
	{	puts ("\n\nBad dithered read length.\n") ;
		exit (1) ;
		} ;

	for (k = 0 ; k < frames * channels ; k++)
	{	int deviation = abs ((int) dithered [k] - (int) ref [k]) ;

		if (deviation != 0)
			differences++ ;
		if (deviation > max_deviation)
			max_deviation = deviation ;
		} ;

	if (differences == 0)
	{	printf ("\n\nLine %d: White dither did not alter any sample.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	if (differences == frames * channels)
	{	printf ("\n\nLine %d: White dither altered every single sample.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	/* A 1 LSB dither cannot push the quantized output far from the reference. */
	if (max_deviation > 2)
	{	printf ("\n\nLine %d: Unexpectedly large dither deviation %d.\n\n", __LINE__, max_deviation) ;
		exit (1) ;
		} ;

	unlink ("dither_ref_u8.wav") ;
	unlink ("dither_white_u8.wav") ;

	puts ("ok") ;
} /* test_white_alters_output */

/* After dither is switched off, later samples must be bit identical to a
** file that was never dithered. */
static void
test_disable_is_identical (void)
{	const int		frames = BUFFER_LEN / 2, channels = 2 ;
	static short	input [BUFFER_LEN] ;
	static unsigned char ref [BUFFER_LEN], toggled [BUFFER_LEN] ;
	SF_INFO			info ;
	SF_DITHER_INFO	off ;
	int				k ;

	print_test_name ("test_disable_is_identical", "wav u8") ;

	for (k = 0 ; k < frames * channels ; k++)
		input [k] = (short) (((k * 911) % 3000) - 1500) ;

	SNDFILE *ref_file = open_wav ("dither_off_ref.wav", &info, SF_FORMAT_PCM_U8, channels) ;
	test_writef_short_or_die (ref_file, 0, input, frames, __LINE__) ;
	sf_close (ref_file) ;

	SNDFILE *tog_file = open_wav ("dither_off_toggle.wav", &info, SF_FORMAT_PCM_U8, channels) ;
	set_dither_or_die (tog_file, SFD_TRIANGULAR_PDF, 0.0, 0, __LINE__) ;
	test_writef_short_or_die (tog_file, 0, input, frames / 4, __LINE__) ;

	memset (&off, 0, sizeof (off)) ;
	off.type = SFD_NO_DITHER ;
	if (sf_command (tog_file, SFC_SET_DITHER_ON_WRITE, &off, sizeof (off)) != 0)
	{	printf ("\n\nLine %d: failed to disable dither : %s\n\n", __LINE__, sf_strerror (tog_file)) ;
		exit (1) ;
		} ;

	test_writef_short_or_die (tog_file, 0, input + (frames / 4) * channels, frames - frames / 4, __LINE__) ;
	sf_close (tog_file) ;

	read_raw_file ("dither_off_ref.wav", &info, ref, 1) ;
	read_raw_file ("dither_off_toggle.wav", &info, toggled, 1) ;

	for (k = (frames / 4) * channels ; k < frames * channels ; k++)
		if (ref [k] != toggled [k])
		{	printf ("\n\nLine %d: sample %d differs after dither was disabled (%d != %d).\n\n",
					__LINE__, k, ref [k], toggled [k]) ;
			exit (1) ;
			} ;

	unlink ("dither_off_ref.wav") ;
	unlink ("dither_off_toggle.wav") ;

	puts ("ok") ;
} /* test_disable_is_identical */

/* When the source precision is not greater than the target bit depth, the
** samples must be passed through untouched even with dither enabled. */
static void
test_no_precision_loss (void)
{	const int		frames = BUFFER_LEN ;
	static short	short_input [BUFFER_LEN], short_ref [BUFFER_LEN], short_dithered [BUFFER_LEN] ;
	static int		int_input [BUFFER_LEN], int_ref [BUFFER_LEN], int_dithered [BUFFER_LEN] ;
	SF_INFO			info ;
	int				k ;

	print_test_name ("test_no_precision_loss", "wav 16/32 bit") ;

	for (k = 0 ; k < frames ; k++)
	{	short_input [k] = (short) ((k * 173) % 60000 - 30000) ;
		int_input [k] = k * 70003 ;
		} ;

	SNDFILE *ref_file = open_wav ("dither_pls_s16.wav", &info, SF_FORMAT_PCM_16, 1) ;
	test_writef_short_or_die (ref_file, 0, short_input, frames, __LINE__) ;
	sf_close (ref_file) ;

	SNDFILE *dith_file = open_wav ("dither_pls_s16d.wav", &info, SF_FORMAT_PCM_16, 1) ;
	set_dither_or_die (dith_file, SFD_WHITE, 0.0, 0, __LINE__) ;
	test_writef_short_or_die (dith_file, 0, short_input, frames, __LINE__) ;
	sf_close (dith_file) ;

	read_raw_file ("dither_pls_s16.wav", &info, short_ref, 2) ;
	read_raw_file ("dither_pls_s16d.wav", &info, short_dithered, 2) ;
	if (memcmp (short_ref, short_dithered, sizeof (short_ref)) != 0)
	{	printf ("\n\nLine %d: short -> 16 bit data modified despite no precision loss.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	ref_file = open_wav ("dither_pls_i32.wav", &info, SF_FORMAT_PCM_32, 1) ;
	test_writef_int_or_die (ref_file, 0, int_input, frames, __LINE__) ;
	sf_close (ref_file) ;

	dith_file = open_wav ("dither_pls_i32d.wav", &info, SF_FORMAT_PCM_32, 1) ;
	set_dither_or_die (dith_file, SFD_TRIANGULAR_PDF, 0.0, 0, __LINE__) ;
	test_writef_int_or_die (dith_file, 0, int_input, frames, __LINE__) ;
	sf_close (dith_file) ;

	read_raw_file ("dither_pls_i32.wav", &info, int_ref, 4) ;
	read_raw_file ("dither_pls_i32d.wav", &info, int_dithered, 4) ;
	if (memcmp (int_ref, int_dithered, sizeof (int_ref)) != 0)
	{	printf ("\n\nLine %d: int -> 32 bit data modified despite no precision loss.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	unlink ("dither_pls_s16.wav") ;
	unlink ("dither_pls_s16d.wav") ;
	unlink ("dither_pls_i32.wav") ;
	unlink ("dither_pls_i32d.wav") ;

	puts ("ok") ;
} /* test_no_precision_loss */

/* Floating point writes into integer PCM are dithered; writes into a float
** container are not quantized and stay untouched. */
static void
test_float_source (void)
{	const int		frames = BUFFER_LEN / 2, channels = 2 ;
	static float	float_input [BUFFER_LEN] ;
	static double	double_input [BUFFER_LEN] ;
	static short	short_ref [BUFFER_LEN], short_dithered [BUFFER_LEN] ;
	static float	float_ref [BUFFER_LEN], float_dithered [BUFFER_LEN] ;
	SF_INFO			info ;
	SNDFILE			*file ;
	int				k, differences = 0, max_deviation = 0 ;

	print_test_name ("test_float_source", "wav 16 bit / float") ;

	for (k = 0 ; k < frames * channels ; k++)
	{	float_input [k] = sinf (k * 0.001f) * 0.5f ;
		double_input [k] = sin (k * 0.0013) * 0.25 ;
		} ;

	file = open_wav ("dither_f_ref.wav", &info, SF_FORMAT_PCM_16, channels) ;
	test_writef_float_or_die (file, 0, float_input, frames, __LINE__) ;
	sf_close (file) ;

	file = open_wav ("dither_f_dith.wav", &info, SF_FORMAT_PCM_16, channels) ;
	set_dither_or_die (file, SFD_TRIANGULAR_PDF, 0.0, 0, __LINE__) ;
	test_writef_float_or_die (file, 0, float_input, frames, __LINE__) ;
	sf_close (file) ;

	read_raw_file ("dither_f_ref.wav", &info, short_ref, 2) ;
	read_raw_file ("dither_f_dith.wav", &info, short_dithered, 2) ;
	for (k = 0 ; k < frames * channels ; k++)
	{	int deviation = abs ((int) short_dithered [k] - (int) short_ref [k]) ;

		if (deviation != 0)
			differences++ ;
		if (deviation > max_deviation)
			max_deviation = deviation ;
		} ;

	if (differences == 0)
	{	printf ("\n\nLine %d: float -> 16 bit dither did not alter any sample.\n\n", __LINE__) ;
		exit (1) ;
		} ;
	if (max_deviation > 2)
	{	printf ("\n\nLine %d: float -> 16 bit dither deviation %d too large.\n\n", __LINE__, max_deviation) ;
		exit (1) ;
		} ;

	unlink ("dither_f_ref.wav") ;
	unlink ("dither_f_dith.wav") ;

	/* double -> 24 bit must also be dithered. */
	static unsigned char byte_ref [BUFFER_LEN * 3], byte_dithered [BUFFER_LEN * 3] ;

	file = open_wav ("dither_d24_ref.wav", &info, SF_FORMAT_PCM_24, channels) ;
	test_writef_double_or_die (file, 0, double_input, frames, __LINE__) ;
	sf_close (file) ;

	file = open_wav ("dither_d24_dith.wav", &info, SF_FORMAT_PCM_24, channels) ;
	set_dither_or_die (file, SFD_WHITE, 0.0, 0, __LINE__) ;
	test_writef_double_or_die (file, 0, double_input, frames, __LINE__) ;
	sf_close (file) ;

	read_raw_file ("dither_d24_ref.wav", &info, byte_ref, 3) ;
	read_raw_file ("dither_d24_dith.wav", &info, byte_dithered, 3) ;
	if (memcmp (byte_ref, byte_dithered, sizeof (byte_ref)) == 0)
	{	printf ("\n\nLine %d: double -> 24 bit dither did not alter any sample.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	unlink ("dither_d24_ref.wav") ;
	unlink ("dither_d24_dith.wav") ;

	/* float -> float container: no integer quantization, nothing to dither. */
	file = open_wav ("dither_float_ref.wav", &info, SF_FORMAT_FLOAT, channels) ;
	test_writef_float_or_die (file, 0, float_input, frames, __LINE__) ;
	sf_close (file) ;

	file = open_wav ("dither_float_dith.wav", &info, SF_FORMAT_FLOAT, channels) ;
	set_dither_or_die (file, SFD_WHITE, 0.0, 0, __LINE__) ;
	test_writef_float_or_die (file, 0, float_input, frames, __LINE__) ;
	sf_close (file) ;

	read_raw_file ("dither_float_ref.wav", &info, float_ref, 4) ;
	read_raw_file ("dither_float_dith.wav", &info, float_dithered, 4) ;
	if (memcmp (float_ref, float_dithered, sizeof (float_ref)) != 0)
	{	printf ("\n\nLine %d: float container data modified by dither.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	unlink ("dither_float_ref.wav") ;
	unlink ("dither_float_dith.wav") ;

	puts ("ok") ;
} /* test_float_source */

/* With identical input on every channel, the per-channel independent
** sequences must make the channel outputs diverge. */
static void
test_channel_independence (void)
{	const int		frames = BUFFER_LEN / 2, channels = 2 ;
	static short	input [BUFFER_LEN] ;
	static unsigned char output [BUFFER_LEN] ;
	SF_INFO			info ;
	int				k, channel_differences = 0 ;

	print_test_name ("test_channel_independence", "wav u8 stereo") ;

	for (k = 0 ; k < frames ; k++)
	{	input [2 * k] = (short) (500 + (k % 7) * 37) ;
		input [2 * k + 1] = input [2 * k] ;
		} ;

	SNDFILE *file = open_wav ("dither_chan.wav", &info, SF_FORMAT_PCM_U8, channels) ;
	set_dither_or_die (file, SFD_WHITE, 0.0, 0, __LINE__) ;
	test_writef_short_or_die (file, 0, input, frames, __LINE__) ;
	sf_close (file) ;

	read_raw_file ("dither_chan.wav", &info, output, 1) ;

	for (k = 0 ; k < frames ; k++)
		if (output [2 * k] != output [2 * k + 1])
			channel_differences++ ;

	if (channel_differences == 0)
	{	printf ("\n\nLine %d: channels were not dithered independently.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	unlink ("dither_chan.wav") ;

	puts ("ok") ;
} /* test_channel_independence */

/* Invalid dither types must be rejected at the sf_command boundary. */
static void
test_invalid_type (void)
{	SNDFILE			*file ;
	SF_INFO			info ;
	SF_DITHER_INFO	dither ;

	print_test_name ("test_invalid_type", "wav u8") ;

	file = open_wav ("dither_bad.wav", &info, SF_FORMAT_PCM_U8, 1) ;

	memset (&dither, 0, sizeof (dither)) ;
	dither.type = 499 ; /* Not a valid SFD_* value. */
	if (sf_command (file, SFC_SET_DITHER_ON_WRITE, &dither, sizeof (dither)) == 0)
	{	printf ("\n\nLine %d: invalid dither type was accepted.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	memset (&dither, 0, sizeof (dither)) ;
	dither.type = 503 ; /* Not a valid SFD_* value. */
	if (sf_command (file, SFC_SET_DITHER_ON_WRITE, &dither, sizeof (dither)) == 0)
	{	printf ("\n\nLine %d: invalid dither type was accepted.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	sf_close (file) ;
	unlink ("dither_bad.wav") ;

	puts ("ok") ;
} /* test_invalid_type */

/* Every write call must still return the number of frames requested. */
static void
test_frame_counts (void)
{	const int		total = BUFFER_LEN / 2, channels = 3 ;
	static float	input [BUFFER_LEN] ;
	SF_INFO			info ;
	SNDFILE			*file ;
	int				fragments [5] = {1, 7, 100, 333, total - 1 - 7 - 100 - 333} ;
	int				k, written = 0 ;

	print_test_name ("test_frame_counts", "wav 16 bit") ;

	for (k = 0 ; k < total * channels ; k++)
		input [k] = 0.3f * sinf (k * 0.01f) ;

	file = open_wav ("dither_frames.wav", &info, SF_FORMAT_PCM_16, channels) ;
	set_dither_or_die (file, SFD_WHITE, 0.0, 0, __LINE__) ;

	for (k = 0 ; k < 5 ; k++)
	{	sf_count_t result = sf_writef_float (file, input + written * channels, fragments [k]) ;

		if (result != fragments [k])
		{	printf ("\n\nLine %d: fragment %d wrote %ld of %d frames.\n\n",
					__LINE__, k, (long) result, fragments [k]) ;
			exit (1) ;
			} ;
		written += fragments [k] ;
		} ;

	if (written != total)
	{	printf ("\n\nLine %d: wrote %d of %d frames.\n\n", __LINE__, written, total) ;
		exit (1) ;
		} ;

	sf_close (file) ;

	SF_INFO read_info ;
	memcpy (&read_info, &info, sizeof (read_info)) ;
	file = test_open_file_or_die ("dither_frames.wav", SFM_READ, &read_info, SF_TRUE, __LINE__) ;
	if (read_info.frames != total)
	{	printf ("\n\nLine %d: file holds %ld frames, expected %d.\n\n",
				__LINE__, (long) read_info.frames, total) ;
		exit (1) ;
		} ;
	sf_close (file) ;

	unlink ("dither_frames.wav") ;

	puts ("ok") ;
} /* test_frame_counts */

/* A larger custom level must widen the spread of the quantized output. */
static void
test_custom_level (void)
{	const int		frames = BUFFER_LEN ;
	static short	input [BUFFER_LEN] ;
	static unsigned char level_one [BUFFER_LEN], level_four [BUFFER_LEN] ;
	SF_INFO			info ;
	int				k, min, max, spread_one, spread_four ;

	print_test_name ("test_custom_level", "wav u8") ;

	for (k = 0 ; k < frames ; k++)
		input [k] = 1000 ;

	SNDFILE *file = open_wav ("dither_lvl1.wav", &info, SF_FORMAT_PCM_U8, 1) ;
	set_dither_or_die (file, SFD_WHITE, 1.0, 1, __LINE__) ;
	test_writef_short_or_die (file, 0, input, frames, __LINE__) ;
	sf_close (file) ;

	file = open_wav ("dither_lvl4.wav", &info, SF_FORMAT_PCM_U8, 1) ;
	set_dither_or_die (file, SFD_WHITE, 4.0, 1, __LINE__) ;
	test_writef_short_or_die (file, 0, input, frames, __LINE__) ;
	sf_close (file) ;

	read_raw_file ("dither_lvl1.wav", &info, level_one, 1) ;
	read_raw_file ("dither_lvl4.wav", &info, level_four, 1) ;

	min = 256 ;
	max = -1 ;
	for (k = 0 ; k < frames ; k++)
	{	if (level_one [k] < min) min = level_one [k] ;
		if (level_one [k] > max) max = level_one [k] ;
		} ;
	spread_one = max - min ;

	min = 256 ;
	max = -1 ;
	for (k = 0 ; k < frames ; k++)
	{	if (level_four [k] < min) min = level_four [k] ;
		if (level_four [k] > max) max = level_four [k] ;
		} ;
	spread_four = max - min ;

	if (spread_four <= spread_one)
	{	printf ("\n\nLine %d: level 4 spread %d did not exceed level 1 spread %d.\n\n",
				__LINE__, spread_four, spread_one) ;
		exit (1) ;
		} ;

	unlink ("dither_lvl1.wav") ;
	unlink ("dither_lvl4.wav") ;

	puts ("ok") ;
} /* test_custom_level */

static void
write_semantics_tests (void)
{
	test_white_alters_output () ;
	test_disable_is_identical () ;
	test_no_precision_loss () ;
	test_float_source () ;
	test_channel_independence () ;
	test_invalid_type () ;
	test_frame_counts () ;
	test_custom_level () ;
} /* write_semantics_tests */
