#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "libretro.h"
#include "gametank.h"
#include "gametank_palette.h"

#define PALETTE_SELECT_OLD 0
#define PALETTE_SELECT_CAPTURE 256
#define PALETTE_SELECT_SCALED 512
#define PALETTE_SELECT_HDMI 768

static const int BORDER_WIDTH = 21;
static const int MAX_WIDTH = GameTank::WIDTH + BORDER_WIDTH * 2;
static const size_t MAX_SAMPLES = 2048;

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static uint32_t framebuffer[MAX_WIDTH * GameTank::HEIGHT];
static uint32_t palette_lut[256];
static int16_t mono_samples[MAX_SAMPLES];
static int16_t stereo_samples[MAX_SAMPLES * 2];

static bool borders = true;
static bool libretro_supports_bitmasks;
static std::string flash_save_path;

static void fallback_log(enum retro_log_level level, const char* fmt, ...) {
	(void) level;
	va_list va;
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
}

static void build_palette(int palette_select) {
	const uint8_t* vals = gt_palette_vals + palette_select * 3;
	for(int i = 0; i < 256; i++) {
		palette_lut[i] = (vals[i * 3] << 16) | (vals[i * 3 + 1] << 8) | vals[i * 3 + 2];
	}
}

static unsigned frame_width() {
	return borders ? MAX_WIDTH : GameTank::WIDTH;
}

static void get_geometry(retro_game_geometry* geom) {
	geom->base_width = frame_width();
	geom->base_height = GameTank::HEIGHT;
	geom->max_width = MAX_WIDTH;
	geom->max_height = GameTank::HEIGHT;
	geom->aspect_ratio = borders ? 4.0f / 3.0f : 1.0f;
}

static void check_variables(bool startup) {
	retro_variable var = {};

	var.key = "gametank_palette";
	int palette_select = PALETTE_SELECT_CAPTURE;
	if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		if(!strcmp(var.value, "Scaled")) palette_select = PALETTE_SELECT_SCALED;
		else if(!strcmp(var.value, "HDMI")) palette_select = PALETTE_SELECT_HDMI;
		else if(!strcmp(var.value, "Legacy")) palette_select = PALETTE_SELECT_OLD;
	}
	build_palette(palette_select);

	var.key = "gametank_borders";
	bool new_borders = true;
	if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		new_borders = strcmp(var.value, "disabled") != 0;
	}
	if(new_borders != borders) {
		borders = new_borders;
		if(!startup) {
			retro_game_geometry geom;
			get_geometry(&geom);
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
		}
	}

	var.key = "gametank_instant_blit";
	bool instant = false;
	if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		instant = !strcmp(var.value, "enabled");
	}
	GameTank::SetInstantBlit(instant);
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
	FILE* f = fopen(path.c_str(), "rb");
	if(!f) return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	if(size < 0) {
		fclose(f);
		return false;
	}
	out.resize(size);
	bool ok = fread(out.data(), 1, size, f) == (size_t) size;
	fclose(f);
	return ok;
}

static void load_flash_save() {
	std::vector<uint8_t> xor_data;
	if(!read_file(flash_save_path, xor_data)) return;
	uint8_t* flash = GameTank::FlashData();
	const uint8_t* rom = GameTank::OriginalRom();
	size_t n = xor_data.size() < GameTank::FlashSize() ? xor_data.size() : GameTank::FlashSize();
	for(size_t i = 0; i < n; i++) {
		flash[i] = rom[i] ^ xor_data[i];
	}
	log_cb(RETRO_LOG_INFO, "[GameTank] Loaded flash save %s\n", flash_save_path.c_str());
}

static void write_flash_save() {
	if(flash_save_path.empty() || !GameTank::HasFlash()) return;
	const uint8_t* flash = GameTank::FlashData();
	const uint8_t* rom = GameTank::OriginalRom();
	size_t size = GameTank::FlashSize();
	if(!memcmp(flash, rom, size)) return;
	std::vector<uint8_t> xor_data(size);
	for(size_t i = 0; i < size; i++) {
		xor_data[i] = flash[i] ^ rom[i];
	}
	FILE* f = fopen(flash_save_path.c_str(), "wb");
	if(!f) {
		log_cb(RETRO_LOG_ERROR, "[GameTank] Could not write %s\n", flash_save_path.c_str());
		return;
	}
	fwrite(xor_data.data(), 1, size, f);
	fclose(f);
}

static std::string flash_save_path_for(const char* game_path) {
	const char* save_dir = NULL;
	if(!game_path || !environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir) {
		return "";
	}
	std::string name(game_path);
	size_t slash = name.find_last_of("/\\");
	if(slash != std::string::npos) name = name.substr(slash + 1);
	size_t dot = name.find_last_of('.');
	if(dot != std::string::npos) name = name.substr(0, dot);
	return std::string(save_dir) + "/" + name + ".xor";
}

static uint16_t read_pad(unsigned port) {
	int16_t bits = 0;
	if(libretro_supports_bitmasks) {
		bits = input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
	} else {
		for(unsigned i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++) {
			if(input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, i)) bits |= 1 << i;
		}
	}
	uint16_t mask = 0;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_UP))    mask |= GameTank::Buttons::UP;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN))  mask |= GameTank::Buttons::DOWN;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT))  mask |= GameTank::Buttons::LEFT;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) mask |= GameTank::Buttons::RIGHT;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_A))     mask |= GameTank::Buttons::A;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_B))     mask |= GameTank::Buttons::B;
	if(bits & ((1 << RETRO_DEVICE_ID_JOYPAD_Y) | (1 << RETRO_DEVICE_ID_JOYPAD_X))) mask |= GameTank::Buttons::C;
	if(bits & (1 << RETRO_DEVICE_ID_JOYPAD_START)) mask |= GameTank::Buttons::START;
	return mask;
}

static void render_frame() {
	const uint8_t* vram = GameTank::VisibleFramebuffer();
	unsigned width = frame_width();
	int x0 = borders ? BORDER_WIDTH : 0;
	for(int y = 0; y < GameTank::HEIGHT; y++) {
		const uint8_t* src = vram + y * GameTank::WIDTH;
		uint32_t* dst = framebuffer + y * width;
		for(int x = 0; x < GameTank::WIDTH; x++) {
			dst[x0 + x] = palette_lut[src[x]];
		}
		if(borders) {
			uint32_t edge = palette_lut[src[GameTank::WIDTH - 1]];
			for(int x = 0; x < BORDER_WIDTH; x++) {
				dst[x] = edge;
				dst[BORDER_WIDTH + GameTank::WIDTH + x] = edge;
			}
		}
	}
	video_cb(framebuffer, width, GameTank::HEIGHT, width * sizeof(uint32_t));
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
	environ_cb = cb;

	static const retro_variable vars[] = {
		{ "gametank_palette", "Palette; Capture|Scaled|HDMI|Legacy" },
		{ "gametank_borders", "Side borders (4:3); enabled|disabled" },
		{ "gametank_instant_blit", "Instant blitter; disabled|enabled" },
		{ NULL, NULL },
	};
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*) vars);

	bool no_rom = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

	static const retro_controller_description pads[] = {
		{ "GameTank Controller", RETRO_DEVICE_JOYPAD },
	};
	static const retro_controller_info ports[] = {
		{ pads, 1 },
		{ pads, 1 },
		{ NULL, 0 },
	};
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*) ports);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void) cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void) {
	retro_log_callback logging;
	if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
		log_cb = logging.log;
	} else {
		log_cb = fallback_log;
	}
	libretro_supports_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
	GameTank::Init();
}

RETRO_API void retro_deinit(void) {
	GameTank::Deinit();
	libretro_supports_bitmasks = false;
}

RETRO_API unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "GameTank";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	info->library_version = "1.0" GIT_VERSION;
	info->valid_extensions = "gtr";
	info->need_fullpath = false;
	info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
	get_geometry(&info->geometry);
	info->timing.fps = GameTank::SYSTEM_CLOCK / GameTank::CyclesPerFrame();
	info->timing.sample_rate = GameTank::SYSTEM_CLOCK / 81.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
	(void) port;
	(void) device;
}

RETRO_API void retro_reset(void) {
	GameTank::Reset();
}

RETRO_API void retro_run(void) {
	bool updated = false;
	if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		check_variables(false);
	}

	input_poll_cb();
	GameTank::SetButtons(read_pad(0), read_pad(1));

	size_t samples = GameTank::RunFrame(mono_samples, MAX_SAMPLES);
	for(size_t i = 0; i < samples; i++) {
		stereo_samples[i * 2] = mono_samples[i];
		stereo_samples[i * 2 + 1] = mono_samples[i];
	}

	render_frame();
	audio_batch_cb(stereo_samples, samples);

	if(GameTank::TakeFlashSaveRequest()) {
		write_flash_save();
	}
}

RETRO_API size_t retro_serialize_size(void) {
	return GameTank::StateSize();
}

RETRO_API bool retro_serialize(void* data, size_t size) {
	return GameTank::SaveState(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
	return GameTank::LoadState(data, size);
}

RETRO_API void retro_cheat_reset(void) {}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	(void) enabled;
	(void) code;
}

RETRO_API bool retro_load_game(const retro_game_info* game) {
	if(!game || !game->data) return false;

	retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		log_cb(RETRO_LOG_ERROR, "[GameTank] XRGB8888 is not supported\n");
		return false;
	}

	static const retro_input_descriptor desc[] = {
#define PAD_DESC(port) \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "C" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "C" }, \
		{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		PAD_DESC(0)
		PAD_DESC(1)
#undef PAD_DESC
		{ 0, 0, 0, 0, NULL },
	};
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) desc);

	if(!GameTank::LoadRom((const uint8_t*) game->data, game->size)) {
		log_cb(RETRO_LOG_ERROR, "[GameTank] Invalid ROM size %u\n", (unsigned) game->size);
		return false;
	}

	flash_save_path.clear();
	if(GameTank::HasFlash()) {
		flash_save_path = flash_save_path_for(game->path);
		if(!flash_save_path.empty()) load_flash_save();
		GameTank::Reset();
	}

	check_variables(true);
	return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const retro_game_info* info, size_t num_info) {
	(void) game_type;
	(void) info;
	(void) num_info;
	return false;
}

RETRO_API void retro_unload_game(void) {
	write_flash_save();
	flash_save_path.clear();
}

RETRO_API unsigned retro_get_region(void) {
	return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id) {
	switch(id) {
		case RETRO_MEMORY_SAVE_RAM: return GameTank::SaveRam();
		case RETRO_MEMORY_SYSTEM_RAM: return GameTank::SystemRam();
		default: return NULL;
	}
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
	switch(id) {
		case RETRO_MEMORY_SAVE_RAM: return GameTank::SaveRamSize();
		case RETRO_MEMORY_SYSTEM_RAM: return GameTank::SystemRamSize();
		default: return 0;
	}
}
