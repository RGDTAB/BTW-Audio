/*
 * BTW - Better than WAV. Lossless compresssion for audio.
 *
 * -- LICENSE: APACHE 2.0
 * Copyright 2022 Raymond DiDonato
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * -- ABOUT:
 * BTW can compress audio losslessly at about 75% efficiency. Both encoding
 * and decoding speeds are slower than FLAC, but I believe this is mainly
 * due to a lack of optimization for bit reading and writing.
 *
 * -- TLDR:
 * Define BTW_IMPLEMENATION, and define whether you want to store samples in
 * uint8_t, int16_t, or int32_t integers.
 * Samples must be alligned to their integer type to be encoded correctly.
 *
 * #define BTW_IMPLEMENTATION
 * //#define BTW_U8 when samples can fit in 8 bits or less
 * //#define BTW_S16 when samples can fit in 16 bits or less
 * //#define BTW_S32 when samples can fit in 32 bits or less
 * #include "btw.h"
 *
 * -- DOCUMENTATION:
 *
 */

/* TLDR: define BTW_IMPLEMENTATION and a BTW_BIT_DEPTH */

#ifdef BTW_IMPLEMENTATION

#include <stdint.h>
#if defined(BTW_U8)
typedef uint8_t btw_sample_fmt;
#elif defined(BTW_S16)
typedef int16_t btw_sample_fmt;
#elif defined BTW_S32
typedef int32_t btw_sample_fmt;
#else
typedef void btw_sample_fmt; /* Should cause compiler failure */
#endif

#else
typedef void btw_sample_fmt; /* Should cause compiler failure */
#endif /* BTW_IMPLEMENTATION */

#ifndef BTW_H
#define BTW_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned int channels;
	unsigned int bits_per_sample;
	unsigned long sample_rate;
	unsigned long long sample_count;
} btw_def;


unsigned char *btw_encode(btw_sample_fmt *samples, btw_def *def,
		unsigned long long *out_len);

void btw_read_metadata(const unsigned char *data, btw_def *def);

btw_sample_fmt *btw_decode(const unsigned char *data, btw_def *def,
		unsigned long long *out_len);

#ifdef __cplusplus
}
#endif

#endif /* BTW_H */

#ifdef BTW_IMPLEMENTATION
#include <stdlib.h>

#define BTW_BLOCK_SIZE 512
#define BTW_HEADER_SIZE 20

#define BTW_MAGIC \
	(((unsigned int)'b') | ((unsigned int)'t') << 8 | \
	 ((unsigned int)'w') <<	16 | ((unsigned int)'f' << 24))

static long long
BTW_abs(long long number)
{
	return number >= 0 ? number : number * -1;
}

static int
bits_required(long long number)
{
	uint8_t r = 0;
	while (number >>= 1)
		r++;

	return r;
}

static void
put_bit(unsigned char *output, unsigned long long *out_pos,
		int *bit_pos, unsigned char bit)
{
	output[*out_pos] |= (bit << *bit_pos) & 0xff;

	*bit_pos = (*bit_pos + 1) % 8;
	*out_pos += !(*bit_pos);
}

static void
put_number(unsigned char *output, unsigned long long *out_pos,
		int *bit_pos, unsigned char bits, int64_t number)
{
	unsigned char cast;

	if (bits > (8 - *bit_pos)) {

		cast = number & (0xff >> (*bit_pos));
		output[(*out_pos)++] |= (cast << *bit_pos) & 0xff;
		bits -= (8 - *bit_pos);
		number >>= (8 - *bit_pos);

		*bit_pos = 0;

		while (bits >= 8) {
			cast = number & 0xff;
			output[(*out_pos)++] = cast;
			bits -= 8;
			number >>= 8;
		}
	}

	if (bits) {
		cast = number & (0xff >> (8 - bits));
		output[*out_pos] |= cast << *bit_pos;

		*bit_pos = (*bit_pos + bits) % 8;
		*out_pos += (*bit_pos == 0);
	}
}

static void
put_ones(unsigned char *output, unsigned long long *out_pos,
		int *bit_pos, unsigned int ones)
{
	if (ones > (8 - *bit_pos)) {

		output[(*out_pos)++] |= (0xff << *bit_pos) & 0xff;

		ones -= (8 - *bit_pos);
		*bit_pos = 0;

		while (ones >= 8) {

			output[(*out_pos)++] = 0xff;

			ones -= 8;
		}
	}

	if (ones) {
		output[*out_pos] |= ((0xff >> (8 - ones)) << *bit_pos) & 0xff;

		*bit_pos = (*bit_pos + ones) % 8;
		*out_pos += (*bit_pos == 0);
	}

}

static unsigned char
grab_bit(const unsigned char *input, unsigned long long *in_pos,
		int *bit_pos)
{
	int r = (input[*in_pos] >> *bit_pos) & 1;
	*bit_pos = (*bit_pos + 1) % 8;
	*in_pos += !(*bit_pos);
	return r;
}

static long long
grab_number(const unsigned char *input, unsigned long long *in_pos,
		int *bit_pos, int bits)
{
	long long r = 0;
	int tally = 0;

	if (bits > (8 - *bit_pos)) {

		r = (input[(*in_pos)++] >> *bit_pos);

		bits -= (8 - *bit_pos);
		tally = (8 - *bit_pos);

		*bit_pos = 0;

		while (bits >= 8) {
			r |= input[(*in_pos)++] << tally;

			tally += 8;
			bits -= 8;
		}
	}

	if (bits) {
		r |= ((input[*in_pos] >> *bit_pos) & (0xff >> (8 - bits))) << tally;
		*bit_pos = (*bit_pos + bits) % 8;
		*in_pos += (*bit_pos == 0);
	}

	return r;
}

unsigned char *
btw_encode(btw_sample_fmt *samples, btw_def *def, unsigned long long *out_len)
{
	unsigned long long i = 0, rice_un;
	unsigned int cap, l, k, chan;
	int metadata_bit_pos = 0, rice_bit_pos = 0, rice_len;
	long long diff, av_diff;
	long long prev_sample, cur_sample;
	long long max_len, metadata_pos = 0;
	int bits_per_rice_len;
	unsigned char *output = NULL;
	*out_len = 0;

	if (!out_len || !def || !samples || !def->channels
			|| !def->sample_rate || !def->sample_count
			|| !def->bits_per_sample) {
		exit(EXIT_FAILURE);
		return NULL;
	}
	bits_per_rice_len = bits_required(def->bits_per_sample);

	max_len = ((def->channels * def->sample_count * def->bits_per_sample)
		/ 8) + 1024;
	output = calloc(max_len, sizeof(unsigned char));

	put_number(output, &metadata_pos, &metadata_bit_pos, 32, BTW_MAGIC);

	put_number(output, &metadata_pos, &metadata_bit_pos, 64, def->sample_count);
	put_number(output, &metadata_pos, &metadata_bit_pos, 16, def->channels);
	put_number(output, &metadata_pos, &metadata_bit_pos, 16,
			def->bits_per_sample);
	put_number(output, &metadata_pos, &metadata_bit_pos, 32, def->sample_rate);

	*out_len = metadata_pos;

	while (i < def->sample_count) {


		if (def->sample_count - i < BTW_BLOCK_SIZE) {
			cap = def->sample_count - i;
		} else {
			cap = BTW_BLOCK_SIZE;
		}

		for (chan = 0; chan < def->channels; chan++) {

			prev_sample = av_diff = 0;

			for (l = 0; l < cap; l++) {
				cur_sample = samples[(i + l) * def->channels
					+ chan];

				diff = cur_sample - prev_sample;
				av_diff += BTW_abs(diff);

				prev_sample = cur_sample;
			}

			rice_len = bits_required(av_diff / BTW_BLOCK_SIZE);

			put_number(output, out_len, &rice_bit_pos,
				bits_per_rice_len, rice_len);

			prev_sample = 0;
			for (l = 0; l < cap; l++) {
				cur_sample = samples[(i + l) * def->channels
					+ chan];
				diff = cur_sample - prev_sample;
				put_bit(output, out_len, &rice_bit_pos, diff
					< 0);

				diff = BTW_abs(diff);

				rice_un = diff >> rice_len;
				put_ones(output, out_len, &rice_bit_pos,
					rice_un);
				put_bit(output, out_len, &rice_bit_pos, 0);

				put_number(output, out_len, &rice_bit_pos,
					rice_len, diff);

				prev_sample = cur_sample;
			}
		}
		i += cap;
	}

	if (rice_bit_pos) {
		(*out_len)++;
	}
	return output;
}

void
btw_read_metadata(const unsigned char *data, btw_def *def)
{

	if (!def || !data) {
		return;
	}

	if (data[0] != 'b' || data[1] != 't' || data[2] != 'w' || data[3] != 'f') {
		return;
	}
	unsigned long long metadata_pos = 4;
	int metadata_bit_pos = 0;


	def->sample_count
		= grab_number(data, &metadata_pos, &metadata_bit_pos, 64);
	def->channels
		= grab_number(data, &metadata_pos, &metadata_bit_pos, 16);
	def->bits_per_sample
		= grab_number(data, &metadata_pos, &metadata_bit_pos, 16);
	def->sample_rate
		= grab_number(data, &metadata_pos, &metadata_bit_pos, 32);
}

btw_sample_fmt *
btw_decode(const unsigned char *data, btw_def *def, unsigned long long *out_len)
{
	unsigned long long i = 0, in_pos = 20, cap;
	unsigned int j, chan;
	int bit_pos = 0, rice_len, sign, bits_per_rice_len;
	long long diff, prev_sample;
	btw_sample_fmt *output = NULL;
	if (!def || !out_len || !data) {
		return NULL;
	}

	btw_read_metadata(data, def);

	if (!def->channels || !def->sample_rate || !def->sample_count
		|| !def->bits_per_sample) {
		return NULL;
	}
	output = (btw_sample_fmt *)malloc(def->sample_count * def->channels * sizeof(btw_sample_fmt));
	bits_per_rice_len = bits_required(def->bits_per_sample);

	*out_len = 0;

	while (i < def->sample_count) {
		if (def->sample_count - i < BTW_BLOCK_SIZE) {
			cap = (def->sample_count - i);
		} else {
			cap = BTW_BLOCK_SIZE;
		}
		for (chan = 0; chan < def->channels; chan++) {
			rice_len = grab_number(data, &in_pos, &bit_pos, bits_per_rice_len);

			prev_sample = 0;
			for (j = 0; j < cap; j++) {
				diff = 0;
				/* Is the diff negative? */
				sign = grab_bit(data, &in_pos, &bit_pos);

				/* Read unary portion of rice */
				while (grab_bit(data, &in_pos, &bit_pos)) {
					diff += 1 << rice_len;
				}

				/* Grab the remaining bits */
				diff |= grab_number(data, &in_pos, &bit_pos, rice_len);

				if (sign) {
					diff *= -1;
				}

				prev_sample = output[(i + j) * def->channels + chan] = prev_sample + diff;
				(*out_len)++;
			}
		}
		i += cap;
		in_pos += (bit_pos != 0);
		bit_pos = 0;
	}
	return output;
}
#endif
