/* testmovie.c — testa write/read roundtrip do header .bsm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "movie.h"
#include "genesis.h"   /* for SYSTEM_GENESIS */
#include "vdp.h"       /* for BORDER_LEFT */
#include "serialize.h" /* for init_serialize, serialize_buffer */

/* Stubs for util.c dependencies */
int headless = 1;
void render_errorbox(char *msg) { (void)msg; }
void render_infobox(char *msg) { (void)msg; }

static uint8_t *fake_serialize(system_header *s, size_t *sz)
{
	(void)s;
	*sz = 16;
	uint8_t *ret = malloc(16);
	memset(ret, 0, 16);
	return ret;
}

static void fake_deserialize(system_header *s, uint8_t *buf, size_t sz)
{
	(void)s; (void)buf; (void)sz;
}

static system_header make_fake_system(void)
{
	static uint8_t fake_rom[1] = {0};
	system_header s;
	memset(&s, 0, sizeof(s));
	s.serialize   = fake_serialize;
	s.deserialize = fake_deserialize;
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

static void test_freeze_unfreeze_roundtrip(void)
{
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "testmovie_roundtrip.bsm");
	assert(r == 0);

	/* Freeze the initial state (0 frames) */
	serialize_buffer sbuf;
	init_serialize(&sbuf);
	movie_freeze(&sbuf);
	assert(sbuf.size > 0);

	/* Build a deserialize_buffer from the frozen data and unfreeze */
	deserialize_buffer dbuf;
	init_deserialize(&dbuf, sbuf.data, sbuf.size);
	/* register_section_handler doubles max_handler once per call (init: 8→16).
	 * SECTION_MOVIE=24 > 16 would be OOB. Pre-register at 16 to grow to 32 first. */
	register_section_handler(&dbuf, (section_handler){0}, 16);
	register_section_handler(&dbuf,
		(section_handler){.fun = movie_unfreeze, .data = NULL},
		SECTION_MOVIE);
	movie_prepare_for_load();
	load_section(&dbuf);
	movie_check_after_load();

	/* Should still be recording (SECTION_MOVIE was present) */
	assert(movie_get_state() == BSM_STATE_RECORD);

	movie_record_stop();
	free(sbuf.data);
	printf("test_freeze_unfreeze_roundtrip: PASSED\n");
}

static void test_io_port_set_pad_state(void)
{
	/* Build a minimal io_port with a 6-button gamepad */
	io_port port;
	memset(&port, 0, sizeof(port));
	port.device_type = IO_GAMEPAD6;

	/* All buttons released: read should return 0 */
	uint16_t state = io_read_pad_buttons(&port);
	assert(state == 0);

	/* Set A+B+START pressed */
	uint16_t want = (1u << (BUTTON_A     - DPAD_UP))
	              | (1u << (BUTTON_B     - DPAD_UP))
	              | (1u << (BUTTON_START - DPAD_UP));
	io_port_set_pad_state(&port, want);

	state = io_read_pad_buttons(&port);
	assert(state == want);

	/* Release all -- set 0 */
	io_port_set_pad_state(&port, 0);
	state = io_read_pad_buttons(&port);
	assert(state == 0);

	printf("test_io_port_set_pad_state: PASSED\n");
}

static void test_export_pipe(void)
{
	/* Check if ffmpeg is available */
	FILE *check = popen("ffmpeg -version 2>/dev/null", "r");
	if (!check) {
		printf("test_export_pipe: SKIP (ffmpeg not found)\n");
		return;
	}
	pclose(check);

	/* Create a synthetic .bsm with 5 frames */
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_export.bsm");
	assert(r == 0);

	genesis_context fake_gen;
	memset(&fake_gen, 0, sizeof(fake_gen));
	fake_gen.header.type = SYSTEM_GENESIS;
	fake_gen.io.ports[0].device_type = IO_GAMEPAD6;
	fake_gen.io.ports[0].device.pad.gamepad_num = 1;
	fake_gen.io.ports[1].device_type = IO_GAMEPAD6;
	fake_gen.io.ports[1].device.pad.gamepad_num = 2;

	uint16_t pad1_inputs[5] = {0x0001, 0x0003, 0x0007, 0x000F, 0x001F};
	uint16_t pad2_inputs[5] = {0x0010, 0x0030, 0x0070, 0x00F0, 0x01F0};

	for (int i = 0; i < 5; i++) {
		io_port_set_pad_state(&fake_gen.io.ports[0], pad1_inputs[i]);
		io_port_set_pad_state(&fake_gen.io.ports[1], pad2_inputs[i]);
		movie_update(&fake_gen.header);
	}
	movie_record_stop();

	/* Verify the .bsm file was created and has correct frame count */
	FILE *f = fopen("/tmp/testmovie_export.bsm", "rb");
	assert(f != NULL);
	bsm_header h;
	r = bsm_read_header(f, &h);
	assert(r == 0);
	assert(h.frame_count == 5);
	fclose(f);

	/* Cleanup */
	remove("/tmp/testmovie_export.bsm");
	printf("test_export_pipe: PASSED\n");
}

static void test_export_frame_write(void)
{
	const int w = 4, h = 4;
	/* pitch must accommodate BORDER_LEFT + w pixels per row */
	const int pitch = (BORDER_LEFT + w) * (int)sizeof(pixel_t);
	/* fb must be large enough for h rows at full pitch */
	pixel_t fb[(BORDER_LEFT + w) * h];

	/* Zero the whole buffer first (border area + visible area) */
	memset(fb, 0, sizeof(fb));

	/* Fill visible area only (skip BORDER_LEFT columns) */
	uint32_t colors[4] = {
		0xFFFF0000u, /* red */
		0xFF00FF00u, /* green */
		0xFF0000FFu, /* blue */
		0xFF000000u, /* black */
	};
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			fb[y * (BORDER_LEFT + w) + BORDER_LEFT + x] = colors[y];
		}
	}

	FILE *f = tmpfile();
	assert(f != NULL);

	int ret = movie_export_write_frame(fb, pitch, w, h, f);
	assert(ret == 0);

	rewind(f);
	uint8_t rgb[48];
	size_t nread = fread(rgb, 1, sizeof(rgb), f);
	assert(nread == 48);
	fclose(f);

	/* Row 0: red */
	for (int x = 0; x < w; x++) {
		assert(rgb[0 * 12 + x * 3 + 0] == 0xFF);
		assert(rgb[0 * 12 + x * 3 + 1] == 0x00);
		assert(rgb[0 * 12 + x * 3 + 2] == 0x00);
	}
	/* Row 1: green */
	for (int x = 0; x < w; x++) {
		assert(rgb[1 * 12 + x * 3 + 0] == 0x00);
		assert(rgb[1 * 12 + x * 3 + 1] == 0xFF);
		assert(rgb[1 * 12 + x * 3 + 2] == 0x00);
	}
	/* Row 2: blue */
	for (int x = 0; x < w; x++) {
		assert(rgb[2 * 12 + x * 3 + 0] == 0x00);
		assert(rgb[2 * 12 + x * 3 + 1] == 0x00);
		assert(rgb[2 * 12 + x * 3 + 2] == 0xFF);
	}
	/* Row 3: black */
	for (int x = 0; x < w; x++) {
		assert(rgb[3 * 12 + x * 3 + 0] == 0x00);
		assert(rgb[3 * 12 + x * 3 + 1] == 0x00);
		assert(rgb[3 * 12 + x * 3 + 2] == 0x00);
	}

	printf("test_export_frame_write: PASSED\n");
}

static void test_playback_input_buffer_roundtrip(void)
{
	/* Phase 1: record 5 frames with known input patterns to a temp file */
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_pb.bsm");
	assert(r == 0);

	genesis_context fake_gen;
	memset(&fake_gen, 0, sizeof(fake_gen));
	fake_gen.header.type = SYSTEM_GENESIS;

	fake_gen.io.ports[0].device_type = IO_GAMEPAD6;
	fake_gen.io.ports[0].device.pad.gamepad_num = 1;
	fake_gen.io.ports[1].device_type = IO_GAMEPAD6;
	fake_gen.io.ports[1].device.pad.gamepad_num = 2;

	uint16_t pad1_inputs[5] = {0x0001, 0x0003, 0x0007, 0x000F, 0x001F};
	uint16_t pad2_inputs[5] = {0x0010, 0x0030, 0x0070, 0x00F0, 0x01F0};

	for (int i = 0; i < 5; i++) {
		io_port_set_pad_state(&fake_gen.io.ports[0], pad1_inputs[i]);
		io_port_set_pad_state(&fake_gen.io.ports[1], pad2_inputs[i]);
		movie_update(&fake_gen.header);
	}

	movie_record_stop();
	assert(movie_get_state() == BSM_STATE_NONE);

	/* Phase 2: read the file back manually and verify inputs */
	FILE *f = fopen("/tmp/testmovie_pb.bsm", "rb");
	assert(f != NULL);

	bsm_header h;
	r = bsm_read_header(f, &h);
	assert(r == 0);
	assert(h.frame_count == 5);

	fseek(f, h.input_offset, SEEK_SET);
	bsm_frame_input frames[5];
	size_t read = fread(frames, sizeof(bsm_frame_input), 5, f);
	assert(read == 5);
	fclose(f);

	for (int i = 0; i < 5; i++) {
		assert(frames[i].pad1 == pad1_inputs[i]);
		assert(frames[i].pad2 == pad2_inputs[i]);
	}

	/* Phase 3: movie_play_start loads the file, reads header + inputs into RAM.
	 * movie_play_pre_inject injects frame 0 BEFORE the first io_run,
	 * fixing the 1-frame playback offset. */
	assert(movie_get_state() == BSM_STATE_NONE);
	r = movie_play_start(&sys, "/tmp/testmovie_pb.bsm");
	assert(r == 0);
	assert(movie_get_state() == BSM_STATE_PLAY);

	/* Pre-inject frame 0: must be called after movie_play_start */
	movie_play_pre_inject(&fake_gen.header);
	assert(movie_get_play_frame() == 1);

	/* Verify frame 0 is already in the pad ports */
	uint16_t port1_state = io_read_pad_buttons(&fake_gen.io.ports[0]);
	uint16_t port2_state = io_read_pad_buttons(&fake_gen.io.ports[1]);
	assert(port1_state == pad1_inputs[0]);
	assert(port2_state == pad2_inputs[0]);

	/* movie_update injects frames 1..4; on the 5th call
	 * play_frame reaches frame_count and state flips to NONE */
	for (int i = 0; i < 5; i++) {
		assert(movie_get_play_frame() == (uint32_t)(i + 1));
		movie_update(&fake_gen.header);
	}
	assert(movie_get_state() == BSM_STATE_NONE);

	printf("test_playback_input_buffer_roundtrip: PASSED\n");
}

int main(void)
{
	test_header_roundtrip();
	test_bad_magic();
	test_check_noop_when_not_recording();
	test_stop_on_missing_section();
	test_freeze_writes_section();
	test_freeze_unfreeze_roundtrip();
	test_io_port_set_pad_state();
	test_playback_input_buffer_roundtrip();
	test_export_frame_write();
	test_export_pipe();
	printf("All tests passed.\n");
	return 0;
}
