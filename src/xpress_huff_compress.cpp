// ms-compress: implements Microsoft compression algorithms
// Copyright (C) 2012  Jeffrey Bush  jeff@coderforlife.com
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


#include "../include/mscomp/internal.h"

#ifdef MSCOMP_WITH_XPRESS_HUFF

#include "../include/xpress_huff.h"
#include "../include/mscomp/XpressDictionary.h"
#include "../include/mscomp/Bitstream.h"
#include "../include/mscomp/HuffmanEncoder.h"

#define PRINT_ERROR(...) // TODO: remove

////////////////////////////// General Definitions and Functions ///////////////////////////////////
#define MAX_OFFSET		0xFFFF
#define CHUNK_SIZE		0x10000

#define STREAM_END		0x100
#define STREAM_END_LEN_1	1
//#define STREAM_END_LEN_1	1<<4 // if STREAM_END&1

#define SYMBOLS			0x200
#define HALF_SYMBOLS	0x100

#define MIN_DATA		HALF_SYMBOLS + 4 // the 512 Huffman lens + 2 uint16s for minimal bitstream

typedef XpressDictionary<MAX_OFFSET, CHUNK_SIZE> Dictionary;
typedef HuffmanEncoder<15, SYMBOLS> Encoder;


////////////////////////////// Compression Functions ///////////////////////////////////////////////
WARNINGS_PUSH()
WARNINGS_IGNORE_POTENTIAL_UNINIT_VALRIABLE_USED()
static size_t xh_compress_lz77(const_bytes in, int32_t /* * */ in_len, const_bytes in_end, bytes out, uint32_t symbol_counts[], Dictionary* d)
{
	int32_t rem = /* * */ in_len;
	uint32_t mask;
	const const_bytes in_orig = in, out_orig = out;
	uint32_t* mask_out;
	byte i;
	
	d->Fill(in);
	memset(symbol_counts, 0, SYMBOLS*sizeof(uint32_t));

	////////// Count the symbols and write the initial LZ77 compressed data //////////
	// A uint32 mask holds the status of each subsequent byte (0 for literal, 1 for offset / length)
	// Literals are stored using a single byte for their value
	// Offset / length pairs are stored in the following manner:
	//   Offset: a uint16
	//   Length: for length-3:
	//     0x0000 <= length <  0x000000FF  length-3 as byte
	//     0x00FF <= length <= 0x0000FFFF  0xFF + length-3 as uint16
	//     0xFFFF <  length <= 0xFFFFFFFF  0xFF + 0x0000 + length-3 as uint32
	// The number of bytes between uint32 masks is >=32 and <=160 (5*32)
	//   with the exception that the a length > 0x10002 could be found, but this is longer than a chunk and would immediately end the chunk
	//   if it is the last one, then we need 4 additional bytes, but we don't have to take it into account in any other way
	while (rem > 0)
	{
		mask = 0;
		mask_out = (uint32_t*)out;
		out += sizeof(uint32_t);

		// Go through each bit
		for (i = 0; i < 32 && rem > 0; ++i)
		{
			uint32_t len, off;
			mask >>= 1;
			//d->Add(in);
			if (rem >= 3 && (len = d->Find(in, &off)) >= 3)
			{
				// TODO: allow len > rem (chunk-spanning matches)
				if (len > rem) { len = rem; }
				
				//d->Add(in + 1, len - 1);

				// Write offset / length
				*(uint16_t*)out = (uint16_t)off;
				out += 2;
				in += len;
				rem -= len;
				len -= 3;
				if (len > 0xFFFF) { *out = 0xFF; *(uint16_t*)(out+1) = 0; *(uint32_t*)(out+3) = len; out += 7; }
				if (len >= 0xFF)  { *out = 0xFF; *(uint16_t*)(out+1) = (uint16_t)len; out += 3; }
				else              { *out = (byte)len; ++out; }
				mask |= 0x80000000; // set the highest bit

				// Create a symbol from the offset and length
				++symbol_counts[(log2(off) << 4) | MIN(0xF, len) | 0x100];
			}
			else
			{
				// Write the literal value (which is the symbol)
				++symbol_counts[*out++ = *in++];
				--rem;
			}
		}

		// Save mask
		*mask_out = mask;
	}
	
	// Set the total number of bytes read from in
	/* *in_len -= rem; */
	mask >>= (32-i); // finish moving the value over
	if (in_orig+ /* * */ in_len == in_end)
	{
		// Add the end of stream symbol
		if (i == 32)
		{
			// Need to add a new mask since the old one is full with just one bit set
			*(uint32_t*)out = 1;
			out += 4;
		}
		else
		{
			// Add to the old mask
			mask |= 1 << i; // set the highest bit
		}
		memset(out, 0, 3);
		out += 3;
		++symbol_counts[STREAM_END];
	}
	*mask_out = mask;

	// Return the number of bytes in the output
	return out - out_orig;
}
WARNINGS_POP()
static const uint16_t OffsetMasks[16] = // (1 << O) - 1
{
	0x0000, 0x0001, 0x0003, 0x0007,
	0x000F, 0x001F, 0x003F, 0x007F,
	0x00FF, 0x01FF, 0x03FF, 0x07FF,
	0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF,
};
static size_t xh_compress_encode(const_bytes in, size_t in_len, bytes out, size_t out_len, Encoder *encoder)
{
	uint_fast16_t i;
	ptrdiff_t rem = (ptrdiff_t)in_len;
	uint32_t mask;
	const_bytes end;
	OutputBitstream bstr(out, out_len);

	// Write the encoded compressed data
	// This involves parsing the LZ77 compressed data and re-writing it with the Huffman code
	while (rem > 0)
	{
		// Handle a fragment
		// Bit mask tells us how to handle the next 32 symbols, go through each bit
		for (i = 32, mask = *(uint32_t*)in, in += 4, rem -= 4; mask && rem > 0; --i, mask >>= 1)
		{
			if (mask & 1) // offset / length symbol
			{
				uint_fast16_t off, sym;
				uint32_t len;
				byte O;

				// Get the LZ77 offset and length
				off = *(uint16_t*)in;
				len = in[2];
				in += 3; rem -= 3;
				if (len == 0xFF)
				{
					len = *(uint16_t*)in;
					in += 2; rem -= 2;
					if (len == 0x0000)
					{
						len = *(uint32_t*)in;
						in += 4; rem -= 4;
					}
				}

				// Write the Huffman code then extra offset bits and length bytes
				O = (byte)log2(off);
				// len is already -= 3
				off &= OffsetMasks[O]; // (1 << O) - 1)
				sym = (uint_fast16_t)((O << 4) | MIN(0xF, len) | 0x100);
				if (!encoder->EncodeSymbol(sym, &bstr))						{ break; }
				if (len >= 0xF)
				{
					if (len >= 0xFF + 0xF)
					{
						if (!bstr.WriteRawByte(0xFF))						{ break; }
						if (len > 0xFFFF)
						{
							if (!bstr.WriteRawUInt16(0x0000) || !bstr.WriteRawUInt32(len))	{ break; }
						}
						else if (!bstr.WriteRawUInt16((uint16_t)len))		{ break; }
					}
					else if (!bstr.WriteRawByte((byte)(len - 0xF)))			{ break; }
				}
				if (!bstr.WriteBits(off, O))								{ break; }
			}
			else
			{
				// Write the literal symbol
				if (!encoder->EncodeSymbol(*in, &bstr))						{ break; }
				++in; --rem;
			}
		}
		if (rem < 0) { break; }
		if (rem < i) { i = (byte)rem; }

		// Write the remaining literal symbols
		for (end = in+i; in != end && encoder->EncodeSymbol(*in, &bstr); ++in);
		if (in != end)														{ break; }
		rem -= i;
	}

	// Write end of stream symbol and return insufficient buffer or the compressed size
	return rem > 0 ? 0 : bstr.Finish(); // make sure that the write stream is finished writing
}
PREVENT_LOOP_VECTORIZATION
// TODO: GCC with -ftree-vectorize (default added with -O3) causes an access violation below
MSCompStatus xpress_huff_compress(const_bytes in, size_t in_len, bytes out, size_t* _out_len)
{
	if (in_len == 0) { *_out_len = 0; return MSCOMP_OK; }

	bytes buf = (bytes)malloc((in_len >= CHUNK_SIZE) ? 0x1200B : ((in_len + 31) / 32 * 36 + 4 + 7)); // for every 32 bytes in "in" we need up to 36 bytes in the temp buffer + maybe an extra uint32 length symbol + up to 7 for the EOS
	if (buf == NULL) { return MSCOMP_MEM_ERROR; }

	const bytes out_orig = out;
	const const_bytes in_end = in+in_len;
	size_t out_len = *_out_len;
	Dictionary d(in, in_end);
	Encoder encoder;

	// Go through each chunk except the last
	while (in_len > CHUNK_SIZE)
	{
		if (out_len < MIN_DATA) { PRINT_ERROR("Xpress Huffman Compression Error: Insufficient buffer\n"); free(buf); return MSCOMP_BUF_ERROR; }

		////////// Perform the initial LZ77 compression //////////
		uint32_t symbol_counts[SYMBOLS]; // 4*512 = 2 kb
		size_t buf_len = xh_compress_lz77(in, CHUNK_SIZE, in_end, buf, symbol_counts, &d);
	
		////////// Create the Huffman codes/lens and write the Huffman prefix codes as lengths //////////
		const_bytes lens = encoder.CreateCodes(symbol_counts);
		if (lens == NULL) { PRINT_ERROR("Xpress Huffman Compression Error: Unable to allocate buffer memory\n"); free(buf); return MSCOMP_MEM_ERROR; }
		for (uint_fast16_t i = 0, i2 = 0; i < HALF_SYMBOLS; ++i, i2+=2) { out[i] = (lens[i2+1] << 4) | lens[i2]; }

		////////// Encode compressed data //////////
		size_t done = xh_compress_encode(buf, buf_len, out+=HALF_SYMBOLS, out_len-=HALF_SYMBOLS, &encoder);
		if (done == 0) { PRINT_ERROR("Xpress Huffman Compression Error: Insufficient buffer\n"); free(buf); return MSCOMP_BUF_ERROR; }

		// Update all the positions and lengths
		in     += CHUNK_SIZE; out     += done;
		in_len -= CHUNK_SIZE; out_len -= done;
	}

	// Do the last chunk
	if (out_len < MIN_DATA) { PRINT_ERROR("Xpress Huffman Compression Error: Insufficient buffer\n"); free(buf); return MSCOMP_BUF_ERROR; }
	else if (in_len == 0) // implies end_of_stream
	{
		memset(out, 0, MIN_DATA);
		out[STREAM_END>>1] = STREAM_END_LEN_1;
		out += MIN_DATA;
	}
	else
	{
		////////// Perform the initial LZ77 compression //////////
		uint32_t symbol_counts[SYMBOLS]; // 4*512 = 2 kb
		size_t buf_len = xh_compress_lz77(in, (int32_t)in_len, in_end, buf, symbol_counts, &d);

		////////// Create the Huffman codes/lens and write the Huffman prefix codes as lengths //////////
		const_bytes lens = encoder.CreateCodes(symbol_counts);
		if (lens == NULL) { PRINT_ERROR("Xpress Huffman Compression Error: Unable to allocate buffer memory\n"); free(buf); return MSCOMP_MEM_ERROR; }
		for (uint_fast16_t i = 0, i2 = 0; i < HALF_SYMBOLS; ++i, i2+=2) { out[i] = (lens[i2+1] << 4) | lens[i2]; }

		////////// Encode compressed data //////////
		size_t done = xh_compress_encode(buf, buf_len, out+=HALF_SYMBOLS, out_len-=HALF_SYMBOLS, &encoder);
		if (done == 0) { PRINT_ERROR("Xpress Huffman Compression Error: Insufficient buffer\n"); free(buf); return MSCOMP_BUF_ERROR; }
		out += done;
	}

	// Cleanup
	free(buf);

	// Return the total number of compressed bytes
	*_out_len = out - out_orig;
	return MSCOMP_OK;
}

#endif
