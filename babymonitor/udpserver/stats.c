#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#define BUFSIZE 700
int16_t val[BUFSIZE];

void do_stats(uint16_t val[BUFSIZE]) 
{
	int i;
	for (i = 1; i < BUFSIZE; i++) {
		printf("%d\t%d\n", val[i], val[i] - val[i-1]);
	}
}

int do_delta7(uint16_t val[BUFSIZE], uint8_t *ptr)
{
	// Implement delta 7 compression.
	// First bit = 0 <=> uncompressed 15 bits following 
	// First bit = 1 <=> 7 bits follow representing delta
	// must switch to big endian...
	uint16_t last = 0;
	uint8_t *start = ptr;
	for (int i = 0; i < BUFSIZE; i++) {
		const uint8_t lowbyte1 = val[i] & 0xFF;
		const uint8_t highbyte1 = (val[i] & 0x0F00) >> 8;

		const int16_t diff = val[i] - last;
		if (diff > -64 && diff < 64) {
			// 7bit delta possible
			// Encode the delta as "sign and magnitude" format. 
			// CSMMMMMM (compressed signed magnitude^6)
			int8_t out = 0x80 | ((diff < 0) ? 0x40 : 0x0) | abs(diff);
			*ptr++ = out;
		} else {
			// 7bit delta impossible, output as-is
			*ptr++ = highbyte1;
			*ptr++ = lowbyte1;
		}
		last = val[i];
	}
	return ptr - start;

}

int do_undelta7(const uint8_t *val, int sz, uint16_t out[BUFSIZE])
{
	// Implement delta 7 decompression.
	// First bit = 0 <=> uncompressed 15 bits following 
	// First bit = 1 <=> 7 bits follow representing delta
	// must switch to big endian...
	uint16_t last = 0;
	uint8_t *ptr = (uint8_t *)&out[0];
	const uint8_t *start = ptr;
	for (int i = 0; i < sz; i++) {
		uint16_t *ptr16 = (uint16_t *)ptr;
		const int8_t firstbyte = val[i];
		if (firstbyte & 0x80) {
			// Delta7 compressed
			// byte is CSMMMMMM
			int8_t delta = firstbyte & 0x3F;
			if (firstbyte & 0x40) {
				delta = -delta;
			}
			const uint16_t value = last + delta;
			*ptr16 = value;
			ptr += 2;

			last = value;
		} else {
			// uncompressed -- switch bytes back to LE
			*ptr++ = val[i+1];
			*ptr++ = val[i];
			last = val[i+1] | val[i] << 8;
			i++;
		}
	}

	return ptr - start;

}

int do_pack(const uint16_t val[BUFSIZE], uint8_t *ptr)
{
	// Implement 12bit packing
	// In is 12bit samples in 16bit per sample
	// Out is 12bit simples in 12bit per sample

	uint8_t *start = ptr;
	int i;
	for (i = 0; i < BUFSIZE-1; i+=2) {
		const uint8_t lowbyte1 = val[i] & 0xFF;
		const uint8_t highbyte1 = (val[i] & 0x0F00) >> 8;
		const uint8_t lowbyte2 = val[i+1] & 0xFF;
		const uint8_t highbyte2 = (val[i+1] & 0x0F00) >> 8;

		assert((val[i] & 0x0F00) == (val[i] & 0xFF00));
		assert((val[i+1] & 0x0F00) == (val[i+1] & 0xFF00));

		/* With the four L H l h bytes, go from:
		   0x0308 0x0108
		   LLLLLLLL HHHH0000 llllllll hhhh0000
		   to
		   0x03 0x
		   LLLLLLLL HHHHllll llllhhhh
		   */

		*ptr++ = lowbyte1;
		const uint8_t middle = (highbyte1) << 4 | (lowbyte2 >> 4) & 0xF;
		const uint8_t high = ((lowbyte2) & 0xF) << 4 | highbyte2;
		*ptr++ = middle;
		*ptr++ = high;
	}

	return ptr-start;
}

int do_unpack(const uint8_t *val, int sz, uint16_t out[BUFSIZE])
{
	// Implement 12bit unpacking

	int i;
	uint8_t *ptr = (uint8_t *)&out[0];
	const uint8_t *start = ptr;
	for (i = 0; i <= sz-3; i+=3) {
		const uint8_t low = val[i];
		const uint8_t middle = val[i + 1];
		const uint8_t high = val[i + 2];

		/* Go from:
		   LLLLLLLL HHHHllll llllhhhh
		   to
		   LLLLLLLL HHHH0000 llllllll hhhh0000
		   */

		*ptr++ = low;
		const uint8_t highbyte1 = (middle & 0xF0) >> 4;
		const uint8_t lowbyte2 = (middle & 0xF) << 4 | (high & 0xF0) >> 4;
		const uint8_t highbyte2 = high & 0xF;

		*ptr++ = highbyte1;
		*ptr++ = lowbyte2;
		*ptr++ = highbyte2;
	}

	return ptr-start;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "wrong cmdline\n");
		exit(1);
	}

	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror("opening file");
		exit(1);
	}

	FILE *out = fopen("/tmp/a.undelta7", "w");

	printf("Statistics\nValue\tDelta prev\n");
	while (!feof(f)) {
		fread(&val[0], 2, BUFSIZE, f);
#if 0
		do_stats(val);
//		do_delta7(val);
		uint8_t out_packed[BUFSIZE * 2];
		uint16_t out_depacked[BUFSIZE];
		uint8_t out_delta7[BUFSIZE * 2];
		uint16_t out_undelta7[BUFSIZE];
		int delta7_sz;
		printf("Packing... %d bytes\n", do_pack(val, out_packed));
		printf("Unpacking... %d bytes\n", do_unpack(&out_packed[0], ceil(BUFSIZE * 1.5), out_depacked));
		printf("Delta7... %d bytes\n", delta7_sz = do_delta7(val, out_delta7));
		printf("Undelta7... %d bytes\n", do_undelta7(&out_delta7[0], delta7_sz, out_undelta7));
		for (int i = 0; i < BUFSIZE; i++) {
			if (val[i] != out_depacked[i]) {
				fprintf(stderr, "Error depacking byte %d: %#x vs %#x\n", i, val[i], out_depacked[i]);
				exit(1);
			}
		}
		for (int i = 0; i < BUFSIZE; i++) {
			if (val[i] != out_undelta7[i]) {
				fprintf(stderr, "Error undelta7ing byte %d: %#x vs %#x\n", i, val[i], out_undelta7[i]);
			}
		}
#endif
		uint16_t out_undelta7[BUFSIZE*2];
		int sz = do_undelta7((uint8_t *)&val[0], BUFSIZE*2, out_undelta7);
		printf("Undelta7... %d bytes\n", sz);
		fwrite(&out_undelta7, 1, sz, out);

	}
	fclose(f);
	fclose(out);
}

