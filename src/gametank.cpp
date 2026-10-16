#include <cstring>
#include <vector>

#include "gametank.h"
#include "system_state.h"
#include "blitter.h"
#include "audio_coprocessor.h"
#include "mos6502/mos6502.h"

namespace GameTank {

const uint8_t VIA_ORA = 0x1;

const uint8_t VIA_SPI_BIT_CLK  = 0b00000001;
const uint8_t VIA_SPI_BIT_MOSI = 0b00000010;
const uint8_t VIA_SPI_BIT_CS   = 0b00000100;

const int FLASH_SIZE = 1 << 21;
const uint64_t CYCLES_PER_VSYNC = (315000000 / 88) / 60;

#define RAM_HIGHBITS_SHIFT 7
#define FULL_RAM_ADDRESS(x) (((system_state.banking & BANK_RAM_MASK) << RAM_HIGHBITS_SHIFT) | (x))

static RomType loadedRomType;
static SystemState system_state;
static CartridgeState cartridge_state;
static std::vector<uint8_t> original_rom;

static mos6502* cpu_core;
static Blitter* blitter;
static AudioCoprocessor* soundcard;

static uint64_t totalCyclesCount;
static uint64_t cycles_since_vsync;
static uint64_t audio_cycles;
static uint32_t rng_state;
static bool flash_save_requested;

static bool pad1State, pad2State;
static uint16_t pad1Mask, pad2Mask;

static uint8_t random_byte() {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (uint8_t) rng_state;
}

static uint8_t open_bus() {
	return random_byte();
}

static uint8_t JoystickRead(uint8_t portNum) {
	uint8_t outbyte;
	if(portNum % 2) {
		outbyte = pad2State ? (uint8_t) (pad2Mask >> 8) : (uint8_t) pad2Mask;
		pad1State = false;
		pad2State = !pad2State;
	} else {
		outbyte = pad1State ? (uint8_t) (pad1Mask >> 8) : (uint8_t) pad1Mask;
		pad2State = false;
		pad1State = !pad1State;
	}
	return ~outbyte;
}

static uint8_t VDMA_Read(uint16_t address) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		return open_bus();
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
			}
		} else {
			bufPtr = system_state.gram;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->state.gram_mid_bits)) << 14;
		}
		return bufPtr[(address & 0x3FFF) | offset];
	}
}

static void VDMA_Write(uint16_t address, uint8_t value) {
	blitter->CatchUp();
	if(system_state.dma_control & DMA_COPY_ENABLE_BIT) {
		blitter->SetParam(address, value);
	} else {
		uint8_t* bufPtr;
		uint32_t offset = 0;
		if(system_state.dma_control & DMA_CPU_TO_VRAM) {
			bufPtr = system_state.vram;
			if(system_state.banking & BANK_VRAM_MASK) {
				offset = 0x4000;
			}
		} else {
			bufPtr = system_state.gram;
			offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->state.gram_mid_bits)) << 14;
		}
		bufPtr[(address & 0x3FFF) | offset] = value;
	}
}

static void UpdateFlashShiftRegister(uint8_t nextVal) {
	uint8_t oldVal = system_state.VIA_regs[VIA_ORA];
	uint8_t risingBits = nextVal & ~oldVal;
	if(risingBits & VIA_SPI_BIT_CLK) {
		cartridge_state.bank_shifter = cartridge_state.bank_shifter << 1;
		cartridge_state.bank_shifter &= 0xFE;
		cartridge_state.bank_shifter |= !!(oldVal & VIA_SPI_BIT_MOSI);
	} else if(risingBits & VIA_SPI_BIT_CS) {
		cartridge_state.bank_mask = cartridge_state.bank_shifter;
		if(loadedRomType != RomType::FLASH2M_RAM32K) {
			cartridge_state.bank_mask |= 0x80;
		}
	}
}

static uint8_t MemoryRead_Flash2M(uint16_t address) {
	if(address & 0x4000) {
		return cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)];
	} else {
		if(!(cartridge_state.bank_mask & 0x80))
			return cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)];
		else return cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)];
	}
}

static uint8_t MemoryRead_Unknown(uint16_t address) {
	if(cartridge_state.size <= 32768) {
		return cartridge_state.rom[((address & 0x7FFF) + 32768 - cartridge_state.size) % cartridge_state.size];
	} else {
		return cartridge_state.rom[((address & 0x7FFF) + cartridge_state.size - 32768)];
	}
}

static uint8_t* GetRAM(const uint16_t address) {
	return &(system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)]);
}

static uint8_t MemoryRead(uint16_t address) {
	if(address & 0x8000) {
		switch(loadedRomType) {
			case RomType::EEPROM8K:
			return cartridge_state.rom[address & 0x1FFF];
			case RomType::EEPROM32K:
			return cartridge_state.rom[address & 0x7FFF];
			case RomType::FLASH2M:
			case RomType::FLASH2M_RAM32K:
			return MemoryRead_Flash2M(address);
			case RomType::UNKNOWN:
			return MemoryRead_Unknown(address);
		}
	} else if(address & 0x4000) {
		return VDMA_Read(address);
	} else if((address >= 0x3000) && (address <= 0x3FFF)) {
		return soundcard->ram_read(address);
	} else if((address >= 0x2800) && (address <= 0x2FFF)) {
		return system_state.VIA_regs[address & 0xF];
	} else if(address < 0x2000) {
		return *GetRAM(address);
	} else if((address == 0x2008) || (address == 0x2009)) {
		return JoystickRead((uint8_t) address);
	}
	return open_bus();
}

static void FlashCommand(uint16_t address, uint8_t value) {
	if(cartridge_state.write_mode) {
		uint8_t* location;
		if(address & 0x4000) {
			location = &(cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)]);
		} else {
			location = &(cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)]);
		}
		*location &= value;
		cartridge_state.write_mode = false;
	} else if(value == 0x10) {
		memset(cartridge_state.rom, 0xFF, FLASH_SIZE);
	} else if(value == 0x30) {
		uint8_t sectorBits = ((address & (1 << 13)) >> 13) | ((cartridge_state.bank_mask & 0x7F) << 1);
		uint8_t sectorNum = sectorBits >> 3;
		if(sectorNum < 31) {
			memset(&cartridge_state.rom[sectorNum << 16], 0xFF, 1 << 16);
		} else if((sectorBits & 4) == 0) {
			memset(&cartridge_state.rom[0x1F0000], 0xFF, 1 << 15);
		} else if(sectorBits == 0b11111100) {
			memset(&cartridge_state.rom[0x1F8000], 0xFF, 1 << 13);
		} else if(sectorBits == 0b11111101) {
			memset(&cartridge_state.rom[0x1FA000], 0xFF, 1 << 13);
		} else if((sectorBits >> 1) == 0b1111111) {
			memset(&cartridge_state.rom[0x1FC000], 0xFF, 1 << 14);
		}
	} else if(value == 0xA0) {
		cartridge_state.write_mode = true;
	} else if(value == 0x90) {
		flash_save_requested = true;
	}
}

static void MemoryWrite(uint16_t address, uint8_t value) {
	if(address & 0x8000) {
		if(loadedRomType == RomType::FLASH2M_RAM32K) {
			if(!(address & 0x4000)) {
				if(!(cartridge_state.bank_mask & 0x80)) {
					cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)] = value;
				}
			}
		}
		if(loadedRomType == RomType::FLASH2M) {
			FlashCommand(address, value);
		}
	}
	else if(address & 0x4000) {
		VDMA_Write(address, value);
	} else if(address >= 0x3000 && address <= 0x3FFF) {
		soundcard->ram_write(address, value);
	} else if((address & 0x2000)) {
		if(address & 0x800) {
			if((loadedRomType == RomType::FLASH2M) || (loadedRomType == RomType::FLASH2M_RAM32K)) {
				if((address & 0xF) == VIA_ORA) {
					UpdateFlashShiftRegister(value);
				}
			}
			system_state.VIA_regs[address & 0xF] = value;
		} else {
			if((address & 0x000F) == 0x0007) {
				blitter->CatchUp();
				system_state.dma_control = value;
				system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
			} else if((address & 0x000F) == 0x0005) {
				blitter->CatchUp();
				system_state.banking = value;
			} else {
				soundcard->register_write(address, value);
			}
		}
	}
	else if(address < 0x2000) {
		system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)] = true;
		system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)] = value;
	}
}

static void CPUStopped() {
}

static void randomize_memory() {
	for(int i = 0; i < RAMSIZE; i++) {
		system_state.ram[i] = random_byte();
		system_state.ram_initialized[i] = false;
	}
	for(int i = 0; i < VRAM_BUFFER_SIZE; i++) {
		system_state.vram[i] = random_byte();
	}
	for(int i = 0; i < GRAM_BUFFER_SIZE; i++) {
		system_state.gram[i] = random_byte();
	}
	for(int i = 0; i < AUDIO_RAM_SIZE; i++) {
		soundcard->ram_write(i, random_byte());
	}
	system_state.dma_control = random_byte();
	system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
	system_state.banking = random_byte();
	blitter->state.gram_mid_bits = random_byte() % 4;
}

bool Init() {
	cartridge_state.rom = new uint8_t[FLASH_SIZE]();
	soundcard = new AudioCoprocessor();
	cpu_core = new mos6502(MemoryRead, MemoryWrite, CPUStopped);
	blitter = new Blitter(cpu_core, &totalCyclesCount, &system_state);
	return true;
}

void Deinit() {
	delete blitter;
	delete cpu_core;
	delete soundcard;
	delete[] cartridge_state.rom;
	blitter = NULL;
	cpu_core = NULL;
	soundcard = NULL;
	cartridge_state.rom = NULL;
	original_rom.clear();
}

bool LoadRom(const uint8_t* data, size_t size) {
	if(size == 0 || size > (size_t) FLASH_SIZE) {
		return false;
	}
	cartridge_state.size = size;
	switch(size) {
		case 8192:
		loadedRomType = RomType::EEPROM8K;
		break;
		case 32768:
		loadedRomType = RomType::EEPROM32K;
		break;
		case FLASH_SIZE:
		loadedRomType = RomType::FLASH2M;
		break;
		default:
		loadedRomType = RomType::UNKNOWN;
		break;
	}
	memset(cartridge_state.rom, 0, FLASH_SIZE);
	memcpy(cartridge_state.rom, data, size);
	original_rom.assign(data, data + size);

	if(loadedRomType == RomType::FLASH2M &&
		(cartridge_state.rom[0x1FFFF0] == 'S') &&
		(cartridge_state.rom[0x1FFFF1] == 'A') &&
		(cartridge_state.rom[0x1FFFF2] == 'V') &&
		(cartridge_state.rom[0x1FFFF3] == 'E')) {
		loadedRomType = RomType::FLASH2M_RAM32K;
	}

	memset(cartridge_state.save_ram, 0, CARTRAMSIZE);
	cartridge_state.bank_shifter = 0;
	cartridge_state.bank_mask = 0;
	totalCyclesCount = 0;
	cycles_since_vsync = 0;
	audio_cycles = 0;
	flash_save_requested = false;
	rng_state = 0x6A7E7A4B;
	blitter->state = {};
	randomize_memory();
	Reset();
	return true;
}

void Reset() {
	cpu_core->Reset();
	cartridge_state.write_mode = false;
	pad1State = false;
	pad2State = false;
}

void SetButtons(uint16_t pad1, uint16_t pad2) {
	pad1Mask = pad1;
	pad2Mask = pad2;
}

void SetInstantBlit(bool enabled) {
	blitter->instant_mode = enabled;
}

uint32_t CyclesPerFrame() {
	return CYCLES_PER_VSYNC;
}

size_t RunFrame(int16_t* audio, size_t max_samples) {
	uint64_t frame_start = totalCyclesCount;
	cpu_core->Run(CYCLES_PER_VSYNC, totalCyclesCount);
	totalCyclesCount = frame_start + CYCLES_PER_VSYNC;

	cycles_since_vsync += CYCLES_PER_VSYNC;
	if(cycles_since_vsync >= CYCLES_PER_VSYNC) {
		cycles_since_vsync -= CYCLES_PER_VSYNC;
		if(system_state.dma_control & DMA_VSYNC_NMI_BIT) {
			cpu_core->NMI();
		}
	}
	blitter->CatchUp();

	audio_cycles += CYCLES_PER_VSYNC;
	size_t samples = audio_cycles / AudioCoprocessor::clksPerHostSample;
	if(samples > max_samples) {
		samples = max_samples;
	}
	audio_cycles -= samples * AudioCoprocessor::clksPerHostSample;
	soundcard->fill_audio(audio, samples);
	return samples;
}

const uint8_t* VisibleFramebuffer() {
	return &system_state.vram[(system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? 0x4000 : 0];
}

uint8_t* SystemRam() {
	return system_state.ram;
}

size_t SystemRamSize() {
	return RAMSIZE;
}

uint8_t* SaveRam() {
	return loadedRomType == RomType::FLASH2M_RAM32K ? cartridge_state.save_ram : NULL;
}

size_t SaveRamSize() {
	return loadedRomType == RomType::FLASH2M_RAM32K ? CARTRAMSIZE : 0;
}

bool HasFlash() {
	return loadedRomType == RomType::FLASH2M;
}

uint8_t* FlashData() {
	return cartridge_state.rom;
}

size_t FlashSize() {
	return FLASH_SIZE;
}

const uint8_t* OriginalRom() {
	return original_rom.data();
}

bool TakeFlashSaveRequest() {
	bool requested = flash_save_requested;
	flash_save_requested = false;
	return requested;
}

static const uint32_t STATE_MAGIC = 0x47545331;

struct MachineState {
	uint32_t magic;
	uint32_t rom_type;
	uint64_t totalCyclesCount;
	uint64_t cycles_since_vsync;
	uint64_t audio_cycles;
	uint32_t rng_state;
	uint8_t bank_shifter;
	uint32_t bank_mask;
	bool write_mode;
	bool pad1State, pad2State;
	uint16_t pad1Mask, pad2Mask;
	mos6502::State cpu;
	BlitterState blitter;
};

size_t StateSize() {
	return sizeof(MachineState) + sizeof(SystemState) + CARTRAMSIZE + sizeof(ACPState);
}

bool SaveState(void* data, size_t size) {
	if(size < StateSize()) {
		return false;
	}
	MachineState m = {};
	m.magic = STATE_MAGIC;
	m.rom_type = loadedRomType;
	m.totalCyclesCount = totalCyclesCount;
	m.cycles_since_vsync = cycles_since_vsync;
	m.audio_cycles = audio_cycles;
	m.rng_state = rng_state;
	m.bank_shifter = cartridge_state.bank_shifter;
	m.bank_mask = cartridge_state.bank_mask;
	m.write_mode = cartridge_state.write_mode;
	m.pad1State = pad1State;
	m.pad2State = pad2State;
	m.pad1Mask = pad1Mask;
	m.pad2Mask = pad2Mask;
	cpu_core->GetState(m.cpu);
	m.blitter = blitter->state;

	ACPState acp;
	soundcard->GetState(acp);

	uint8_t* out = (uint8_t*) data;
	memcpy(out, &m, sizeof(m));
	out += sizeof(m);
	memcpy(out, &system_state, sizeof(system_state));
	out += sizeof(system_state);
	memcpy(out, cartridge_state.save_ram, CARTRAMSIZE);
	out += CARTRAMSIZE;
	memcpy(out, &acp, sizeof(acp));
	return true;
}

bool LoadState(const void* data, size_t size) {
	if(size < StateSize()) {
		return false;
	}
	const uint8_t* in = (const uint8_t*) data;
	MachineState m;
	memcpy(&m, in, sizeof(m));
	if(m.magic != STATE_MAGIC || m.rom_type != (uint32_t) loadedRomType) {
		return false;
	}
	in += sizeof(m);
	memcpy(&system_state, in, sizeof(system_state));
	in += sizeof(system_state);
	memcpy(cartridge_state.save_ram, in, CARTRAMSIZE);
	in += CARTRAMSIZE;
	ACPState acp;
	memcpy(&acp, in, sizeof(acp));
	soundcard->SetState(acp);

	totalCyclesCount = m.totalCyclesCount;
	cycles_since_vsync = m.cycles_since_vsync;
	audio_cycles = m.audio_cycles;
	rng_state = m.rng_state;
	cartridge_state.bank_shifter = m.bank_shifter;
	cartridge_state.bank_mask = m.bank_mask;
	cartridge_state.write_mode = m.write_mode;
	pad1State = m.pad1State;
	pad2State = m.pad2State;
	pad1Mask = m.pad1Mask;
	pad2Mask = m.pad2Mask;
	cpu_core->SetState(m.cpu, &system_state.dma_control_irq);
	blitter->state = m.blitter;
	return true;
}

}
