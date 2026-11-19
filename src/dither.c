/*
** Copyright (C) 2003-2024 Erik de Castro Lopo <erikd@mega-nerd.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include	"sfconfig.h"

#include	<stdlib.h>
#include	<limits.h>

#include	"sndfile.h"
#include	"sfendian.h"
#include	"common.h"

/*============================================================================
**	Dither is only applied on write, and only when the source precision is
**	greater than the target PCM bit depth:
**
**										Input
**					|	short		int			float		double
**			--------+-----------------------------------------------
**		O	8 bit	|	X			X			X			X
**		u	16 bit	|	none		X			X			X
**		t	24 bit	|	none		X			X			X
**		p	32 bit	|	none		none		X			X
**		u	float	|	none		none		none		none
**		t	double	|	none		none		none		none
**
**	Dither is generated independently for each channel and added to the
**	signal immediately before the final quantization, scaled to the
**	least significant bit of the target PCM bit depth.
*/

#define	SFE_DITHER_BAD_PTR	666

/* Default peak-to-peak amplitude, in units of the target LSB. */
#define	DITHER_DEFAULT_LEVEL	1.0

typedef struct
{	/* Saved, undithered codec write function pointers. */
	sf_count_t	(*write_short)	(SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
	sf_count_t	(*write_int)		(SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
	sf_count_t	(*write_float)	(SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
	sf_count_t	(*write_double)	(SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

	/* Dither parameters copied from SF_DITHER_INFO. */
	int			dither_type ;
	double		dither_level ;

	/* Effective bit depth of the source data for each write entry point. */
	int			short_bits, int_bits, float_bits, double_bits ;

	/* Target PCM bit depth. */
	int			target_bits ;

	/* One random generator state per channel so that each channel
	** uses an independent dither sequence. */
	uint32_t	rand_state [SF_MAX_CHANNELS] ;

	double buffer [SF_BUFFER_LEN / sizeof (double)] ;
} DITHER_DATA ;

static sf_count_t dither_write_short	(SF_PRIVATE *psf, const short *ptr, sf_count_t len) ;
static sf_count_t dither_write_int		(SF_PRIVATE *psf, const int *ptr, sf_count_t len) ;
static sf_count_t dither_write_float	(SF_PRIVATE *psf, const float *ptr, sf_count_t len) ;
static sf_count_t dither_write_double	(SF_PRIVATE *psf, const double *ptr, sf_count_t len) ;

static void dither_short_array	(DITHER_DATA *pdither, const short *in, short *out, int frames, int channels) ;
static void dither_int_array	(DITHER_DATA *pdither, const int *in, int *out, int frames, int channels) ;
static void dither_float_array	(DITHER_DATA *pdither, const float *in, float *out, int frames, int channels) ;
static void dither_double_array	(DITHER_DATA *pdither, const double *in, double *out, int frames, int channels) ;

/*------------------------------------------------------------------------------
**	Determine the bit depth of the integer PCM container. Returns zero for
**	codecs whose write path does not perform an integer quantization (for
**	instance floating point codecs), in which case dither is never applied.
*/

static int
codec_target_bits (int codec)
{
	switch (codec)
	{	case SF_FORMAT_PCM_S8 :
		case SF_FORMAT_PCM_U8 :
		case SF_FORMAT_DPCM_8 :
				return 8 ;

		case SF_FORMAT_PCM_16 :
		case SF_FORMAT_DPCM_16 :
				return 16 ;

		case SF_FORMAT_PCM_24 :
				return 24 ;

		case SF_FORMAT_PCM_32 :
				return 32 ;

		default :
				return 0 ;
		} ;
} /* codec_target_bits */

/*------------------------------------------------------------------------------
**	Per-channel pseudo random number generator. The state is kept per
**	channel so that interleaved channels are driven by independent
**	sequences. A simple 31 bit linear congruential generator is adequate
**	for dither noise.
*/

static uint32_t
dither_rand (uint32_t *state)
{
	*state = (*state * 1103515245u + 12345u) & 0x7FFFFFFFu ;

	if (*state == 0)
		*state = 1 ;

	return *state ;
} /* dither_rand */

/*
**	Return one uniformly distributed random value in the range [-1.0, 1.0).
*/

static double
dither_rand_uniform (uint32_t *state)
{
	return (2.0 * ((double) dither_rand (state) / 2147483648.0)) - 1.0 ;
} /* dither_rand_uniform */

/*
**	Generate one dither sample scaled so that a level of 1.0 corresponds
**	to one target least significant bit.
**
**	White noise is uniformly distributed over [-0.5, 0.5) LSB.
**	TPDF noise is the sum of two such independent uniforms and is
**	therefore distributed over [-1.0, 1.0) LSB with a triangular
**	probability density function.
*/

static double
dither_sample (DITHER_DATA *pdither, int channel)
{
	double	value ;
	uint32_t *state = &pdither->rand_state [channel] ;

	switch (pdither->dither_type)
	{	case SFD_WHITE :
				value = 0.5 * dither_rand_uniform (state) ;
				break ;

		case SFD_TRIANGULAR_PDF :
				value = 0.5 * (dither_rand_uniform (state) + dither_rand_uniform (state)) ;
				break ;

		default :
				value = 0.0 ;
				break ;
		} ;

	return value * pdither->dither_level ;
} /* dither_sample */

static void
dither_seed (DITHER_DATA *pdither, int channels)
{	int	ch ;

	for (ch = 0 ; ch < channels ; ch++)
	{	/* Seed from the library wide generator so that different
		** SNDFILE handles get different sequences, while keeping each
		** channel independent. Make sure the state is never zero. */
		pdither->rand_state [ch] = (uint32_t) psf_rand_int32 () ;
		if (pdither->rand_state [ch] == 0)
			pdither->rand_state [ch] = (uint32_t) (ch + 1) ;
		} ;

	return ;
} /* dither_seed */

/*==============================================================================
*/

int
dither_init (SF_PRIVATE *psf, int mode)
{	DITHER_DATA *pdither ;
	SF_DITHER_INFO *info ;
	int			target_bits, dither_type, do_dither ;

	/* Dither on read is not implemented; the write side function
	** pointers installed below never touch the read side, so this is a
	** no-op that leaves the stored read parameters untouched. */
	if (mode != SFM_WRITE)
		return 0 ;

	info = &psf->write_dither ;
	dither_type = info->type & SFD_TYPEMASK ;

	if (dither_type != SFD_WHITE && dither_type != SFD_TRIANGULAR_PDF)
	{	/* SFD_NO_DITHER (or anything unknown): restore the codec
		** functions so that writes are bit identical to the
		** never-dithered case. */
		if ((pdither = psf->dither) != NULL)
		{	if (pdither->write_short)
				psf->write_short = pdither->write_short ;
			if (pdither->write_int)
				psf->write_int = pdither->write_int ;
			if (pdither->write_float)
				psf->write_float = pdither->write_float ;
			if (pdither->write_double)
				psf->write_double = pdither->write_double ;

			pdither->write_short = NULL ;
			pdither->write_int = NULL ;
			pdither->write_float = NULL ;
			pdither->write_double = NULL ;
			} ;

		info->type = SFD_NO_DITHER ;
		return 0 ;
		} ;

	target_bits = codec_target_bits (SF_CODEC (psf->sf.format)) ;

	/* Only integer PCM quantization benefits from dither; a target_bits
	** value of zero means the codec has no integer quantization stage. */
	do_dither = (target_bits > 0) ;

	if ((pdither = psf->dither) == NULL)
	{	pdither = calloc (1, sizeof (DITHER_DATA)) ;
		if (pdither == NULL)
			return SFE_MALLOC_FAILED ;
		psf->dither = pdither ;

		/* Seed once, at allocation time, so that switching the dither
		** type or level between writes does not restart the per-channel
		** random sequences. */
		dither_seed (pdither, psf->sf.channels) ;
		} ;

	/* Store the requested dither parameters. A non-positive custom level
	** disables the noise without disabling the dither machinery. */
	pdither->dither_type = dither_type ;
	pdither->dither_level = (info->type & SFD_CUSTOM_LEVEL) ? info->level : DITHER_DEFAULT_LEVEL ;
	if (pdither->dither_level < 0.0)
		pdither->dither_level = 0.0 ;

	pdither->target_bits = target_bits ;

	/* Effective source precision for each entry point. */
	pdither->short_bits = 16 ;
	pdither->int_bits = 32 ;
	pdither->float_bits = 24 ;	/* 24 bit mantissa. */
	pdither->double_bits = 53 ;	/* 53 bit mantissa. */

	if (! do_dither)
	{	/* Float/double containers or unknown codec: pass everything
		** straight through untouched. */
		if (pdither->write_short)
			psf->write_short = pdither->write_short ;
		if (pdither->write_int)
			psf->write_int = pdither->write_int ;
		if (pdither->write_float)
			psf->write_float = pdither->write_float ;
		if (pdither->write_double)
			psf->write_double = pdither->write_double ;

		pdither->write_short = NULL ;
		pdither->write_int = NULL ;
		pdither->write_float = NULL ;
		pdither->write_double = NULL ;
		return 0 ;
		} ;

	/* Install the wrappers, remembering the codec functions the first
	** time around. Re-issuing the command (switching type or level) must
	** not wrap the wrappers. */
	if (pdither->write_short == NULL)
		pdither->write_short = psf->write_short ;
	if (pdither->write_int == NULL)
		pdither->write_int = psf->write_int ;
	if (pdither->write_float == NULL)
		pdither->write_float = psf->write_float ;
	if (pdither->write_double == NULL)
		pdither->write_double = psf->write_double ;

	psf->write_short = dither_write_short ;
	psf->write_int = dither_write_int ;
	psf->write_float = dither_write_float ;
	psf->write_double = dither_write_double ;

	return 0 ;
} /* dither_init */

/*==============================================================================
**	Write entry points. Each one processes at most one internal buffer at a
**	time, dithers into pdither->buffer and then hands the result to the
**	saved codec write function.
*/

static sf_count_t
dither_write_short	(SF_PRIVATE *psf, const short *ptr, sf_count_t len)
{	DITHER_DATA *pdither ;
	int			bufferlen, writecount, thiswrite ;
	sf_count_t	total = 0 ;

	if ((pdither = psf->dither) == NULL)
	{	psf->error = SFE_DITHER_BAD_PTR ;
		return 0 ;
		} ;

	/* Source precision not greater than target: leave the data alone. */
	if (pdither->short_bits <= pdither->target_bits)
		return pdither->write_short (psf, ptr, len) ;

	bufferlen = (int) (sizeof (pdither->buffer) / sizeof (short)) ;

	while (len > 0)
	{	writecount = (len >= bufferlen) ? bufferlen : (int) len ;
		writecount /= psf->sf.channels ;
		writecount *= psf->sf.channels ;

		dither_short_array (pdither, ptr + total, (short*) pdither->buffer,
			writecount / psf->sf.channels, psf->sf.channels) ;

		thiswrite = (int) pdither->write_short (psf, (short*) pdither->buffer, writecount) ;
		total += thiswrite ;
		len -= thiswrite ;
		if (thiswrite < writecount)
			break ;
		} ;

	return total ;
} /* dither_write_short */

static sf_count_t
dither_write_int	(SF_PRIVATE *psf, const int *ptr, sf_count_t len)
{	DITHER_DATA *pdither ;
	int			bufferlen, writecount, thiswrite ;
	sf_count_t	total = 0 ;

	if ((pdither = psf->dither) == NULL)
	{	psf->error = SFE_DITHER_BAD_PTR ;
		return 0 ;
		} ;

	if (pdither->int_bits <= pdither->target_bits)
		return pdither->write_int (psf, ptr, len) ;

	bufferlen = (int) (sizeof (pdither->buffer) / sizeof (int)) ;

	while (len > 0)
	{	writecount = (len >= bufferlen) ? bufferlen : (int) len ;
		writecount /= psf->sf.channels ;
		writecount *= psf->sf.channels ;

		dither_int_array (pdither, ptr + total, (int*) pdither->buffer,
			writecount / psf->sf.channels, psf->sf.channels) ;

		thiswrite = (int) pdither->write_int (psf, (int*) pdither->buffer, writecount) ;
		total += thiswrite ;
		len -= thiswrite ;
		if (thiswrite < writecount)
			break ;
		} ;

	return total ;
} /* dither_write_int */

static sf_count_t
dither_write_float	(SF_PRIVATE *psf, const float *ptr, sf_count_t len)
{	DITHER_DATA *pdither ;
	int			bufferlen, writecount, thiswrite ;
	sf_count_t	total = 0 ;

	if ((pdither = psf->dither) == NULL)
	{	psf->error = SFE_DITHER_BAD_PTR ;
		return 0 ;
		} ;

	if (pdither->float_bits <= pdither->target_bits)
		return pdither->write_float (psf, ptr, len) ;

	bufferlen = (int) (sizeof (pdither->buffer) / sizeof (float)) ;

	while (len > 0)
	{	writecount = (len >= bufferlen) ? bufferlen : (int) len ;
		writecount /= psf->sf.channels ;
		writecount *= psf->sf.channels ;

		dither_float_array (pdither, ptr + total, (float*) pdither->buffer,
			writecount / psf->sf.channels, psf->sf.channels) ;

		thiswrite = (int) pdither->write_float (psf, (float*) pdither->buffer, writecount) ;
		total += thiswrite ;
		len -= thiswrite ;
		if (thiswrite < writecount)
			break ;
		} ;

	return total ;
} /* dither_write_float */

static sf_count_t
dither_write_double	(SF_PRIVATE *psf, const double *ptr, sf_count_t len)
{	DITHER_DATA *pdither ;
	int			bufferlen, writecount, thiswrite ;
	sf_count_t	total = 0 ;

	if ((pdither = psf->dither) == NULL)
	{	psf->error = SFE_DITHER_BAD_PTR ;
		return 0 ;
		} ;

	if (pdither->double_bits <= pdither->target_bits)
		return pdither->write_double (psf, ptr, len) ;

	bufferlen = (int) (sizeof (pdither->buffer) / sizeof (double)) ;

	while (len > 0)
	{	writecount = (len >= bufferlen) ? bufferlen : (int) len ;
		writecount /= psf->sf.channels ;
		writecount *= psf->sf.channels ;

		dither_double_array (pdither, ptr + total, pdither->buffer,
			writecount / psf->sf.channels, psf->sf.channels) ;

		thiswrite = (int) pdither->write_double (psf, pdither->buffer, writecount) ;
		total += thiswrite ;
		len -= thiswrite ;
		if (thiswrite < writecount)
			break ;
		} ;

	return total ;
} /* dither_write_double */

/*==============================================================================
**	Dither array processing.
**
**	For integer source data the dither amplitude is expressed in target
**	LSB units of the source integer container, i.e. the dither value is
**	shifted left by the difference in bit widths and added before the
**	codec's own truncating shift. The result is clamped to the source
**	container range so that the codec's existing clipping semantics are
**	all that govern the final value.
**
**	For floating point source data the dither is expressed directly in
**	target LSB units of the normalized floating point scale, matching the
**	quantization that the codec performs after multiplying by the
**	normalization factor.
*/

static void
dither_short_array (DITHER_DATA *pdither, const short *in, short *out, int frames, int channels)
{	int	ch, k, shift ;
	int64_t	value, dither ;

	shift = pdither->short_bits - pdither->target_bits ;

	for (ch = 0 ; ch < channels ; ch++)
		for (k = ch ; k < channels * frames ; k += channels)
		{	dither = (int64_t) (dither_sample (pdither, ch) * (double) (1 << shift)) ;
			value = ((int64_t) in [k]) + dither ;

			/* Clamp to the source container range; the codec applies
			** its own clipping semantics on top of this. */
			if (value > SHRT_MAX)
				value = SHRT_MAX ;
			else if (value < SHRT_MIN)
				value = SHRT_MIN ;

			out [k] = (short) value ;
			} ;

} /* dither_short_array */

static void
dither_int_array (DITHER_DATA *pdither, const int *in, int *out, int frames, int channels)
{	int		ch, k, shift ;
	double	scale ;
	int64_t	value ;

	shift = pdither->int_bits - pdither->target_bits ;
	scale = (double) (INT64_C (1) << shift) ;

	for (ch = 0 ; ch < channels ; ch++)
		for (k = ch ; k < channels * frames ; k += channels)
		{	value = (int64_t) in [k] + (int64_t) (dither_sample (pdither, ch) * scale) ;

			if (value > INT_MAX)
				value = INT_MAX ;
			else if (value < INT_MIN)
				value = INT_MIN ;

			out [k] = (int) value ;
			} ;

} /* dither_int_array */

static void
dither_float_array (DITHER_DATA *pdither, const float *in, float *out, int frames, int channels)
{	int		ch, k ;
	double	lsb, dither ;

	/* One target LSB in the normalized [-1.0, 1.0) float domain. */
	lsb = 1.0 / (double) (1 << (pdither->target_bits - 1)) ;

	for (ch = 0 ; ch < channels ; ch++)
		for (k = ch ; k < channels * frames ; k += channels)
		{	dither = dither_sample (pdither, ch) * lsb ;
			out [k] = (float) (((double) in [k]) + dither) ;
			} ;

} /* dither_float_array */

static void
dither_double_array (DITHER_DATA *pdither, const double *in, double *out, int frames, int channels)
{	int		ch, k ;
	double	lsb ;

	lsb = 1.0 / (double) (INT64_C (1) << (pdither->target_bits - 1)) ;

	for (ch = 0 ; ch < channels ; ch++)
		for (k = ch ; k < channels * frames ; k += channels)
			out [k] = in [k] + dither_sample (pdither, ch) * lsb ;

} /* dither_double_array */
