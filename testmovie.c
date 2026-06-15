/* testmovie.c — testa write/read roundtrip do header .bsm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "movie.h"

static void test_header_roundtrip(void)
{
	bsm_header expected = {
		.magic            = BSM_MAGIC,
		.version          = BSM_VERSION,
		.movie_id         = 0xDEADBEEFu,
		.rerecord_count   = 7,
		.frame_count      = 3600,
		.flags            = BSM_FLAG_PAL,
		.pad_mask         = 0x03,
		.reserved         = 0,
		.rom_crc32        = 0x12345678u,
		.savestate_offset = BSM_HEADER_SIZE,
		.input_offset     = BSM_HEADER_SIZE + 4 + 1024,
	};
	strncpy(expected.rom_name, "Sonic The Hedgehog", sizeof(expected.rom_name) - 1);
	memset(expected.reserved2, 0, sizeof(expected.reserved2));

	FILE *f = tmpfile();
	assert(f != NULL);

	bsm_write_header(f, &expected);

	rewind(f);

	bsm_header actual;
	int result = bsm_read_header(f, &actual);
	assert(result == 0);

	assert(actual.magic            == expected.magic);
	assert(actual.version          == expected.version);
	assert(actual.movie_id         == expected.movie_id);
	assert(actual.rerecord_count   == expected.rerecord_count);
	assert(actual.frame_count      == expected.frame_count);
	assert(actual.flags            == expected.flags);
	assert(actual.pad_mask         == expected.pad_mask);
	assert(actual.rom_crc32        == expected.rom_crc32);
	assert(memcmp(actual.rom_name, expected.rom_name, sizeof(expected.rom_name)) == 0);
	assert(actual.savestate_offset == expected.savestate_offset);
	assert(actual.input_offset     == expected.input_offset);

	fclose(f);
	printf("test_header_roundtrip: PASSED\n");
}

static void test_bad_magic(void)
{
	FILE *f = tmpfile();
	assert(f != NULL);
	uint32_t bad = 0xDEADBEEF;
	fwrite(&bad, 4, 1, f);
	rewind(f);
	bsm_header h;
	int result = bsm_read_header(f, &h);
	assert(result == -1);
	fclose(f);
	printf("test_bad_magic: PASSED\n");
}

int main(void)
{
	test_header_roundtrip();
	test_bad_magic();
	printf("All tests passed.\n");
	return 0;
}
