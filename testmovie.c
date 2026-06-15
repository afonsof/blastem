/* testmovie.c — testa write/read roundtrip do header .bsm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "movie.h"
#include "genesis.h"   /* for SYSTEM_GENESIS */
#include "serialize.h" /* for init_serialize, serialize_buffer */

static uint8_t *fake_serialize(system_header *s, size_t *sz)
{
	(void)s;
	*sz = 16;
	uint8_t *ret = malloc(16);
	memset(ret, 0, 16);
	return ret;
}

static system_header make_fake_system(void)
{
	static uint8_t fake_rom[1] = {0};
	system_header s;
	memset(&s, 0, sizeof(s));
	s.serialize     = fake_serialize;
	s.info.rom      = fake_rom;
	s.info.rom_size = 1;
	s.type          = SYSTEM_GENESIS;
	return s;
}

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
	/* Write a full 64-byte header but with a wrong magic value,
	 * so bsm_read_header actually reaches the magic-check branch. */
	bsm_header bad = {0};
	bad.magic   = 0xDEADBEEFu;
	bad.version = BSM_VERSION;

	FILE *f = tmpfile();
	assert(f != NULL);
	bsm_write_header(f, &bad);
	rewind(f);

	bsm_header out;
	int result = bsm_read_header(f, &out);
	assert(result == -1);
	fclose(f);
	printf("test_bad_magic: PASSED\n");
}

/* Testa que movie_check_after_load é no-op quando não está gravando */
static void test_check_noop_when_not_recording(void)
{
	assert(movie_get_state() == BSM_STATE_NONE);
	movie_prepare_for_load();
	movie_check_after_load(); /* não deve travar nem mudar estado */
	assert(movie_get_state() == BSM_STATE_NONE);
	printf("test_check_noop_when_not_recording: PASSED\n");
}

/* Testa que carregar state sem SECTION_MOVIE para a gravação */
static void test_stop_on_missing_section(void)
{
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_stop.bsm");
	assert(r == 0);
	assert(movie_get_state() == BSM_STATE_RECORD);

	movie_prepare_for_load();
	/* Não chamamos movie_unfreeze — simula save state sem SECTION_MOVIE */
	movie_check_after_load();

	assert(movie_get_state() == BSM_STATE_NONE);
	printf("test_stop_on_missing_section: PASSED\n");
}

/* Testa que movie_freeze escreve algo na serialize_buffer quando gravando */
static void test_freeze_writes_section(void)
{
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_freeze.bsm");
	assert(r == 0);

	serialize_buffer sbuf;
	init_serialize(&sbuf);
	size_t size_before = sbuf.size;

	movie_freeze(&sbuf);

	assert(sbuf.size > size_before); /* algo foi escrito */
	/* Os primeiros 2 bytes devem ser SECTION_MOVIE (=24) big-endian */
	uint16_t section_id = (uint16_t)((sbuf.data[0] << 8) | sbuf.data[1]);
	assert(section_id == SECTION_MOVIE);

	movie_record_stop();
	free(sbuf.data);
	printf("test_freeze_writes_section: PASSED\n");
}

int main(void)
{
	test_header_roundtrip();
	test_bad_magic();
	test_check_noop_when_not_recording();
	test_stop_on_missing_section();
	test_freeze_writes_section();
	printf("All tests passed.\n");
	return 0;
}
