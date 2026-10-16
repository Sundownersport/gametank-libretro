#pragma once
#include <cstddef>
#include <cstdint>
#include "mos6502/mos6502.h"

#define ACP_RESET 0
#define ACP_NMI 1
#define ACP_RATE 6

#define AUDIO_RAM_SIZE 4096

struct ACPState {
	uint8_t ram[AUDIO_RAM_SIZE];
	int16_t irqCounter;
	uint8_t irqRate;
	bool running;
	bool resetting;
	uint8_t dacReg;
	uint64_t cycles_per_sample;
	uint8_t clkMult;
	uint64_t cycle_counter;
	mos6502::State cpu;
};

class AudioCoprocessor {
private:
	ACPState state;
	mos6502* cpu;
public:
	static const uint16_t clksPerHostSample = 81;
	static ACPState* singleton_acp_state;
	AudioCoprocessor();
	~AudioCoprocessor();
	void ram_write(uint16_t address, uint8_t value);
	uint8_t ram_read(uint16_t address);
	void register_write(uint16_t address, uint8_t value);
	void fill_audio(int16_t* stream, size_t len);
	void GetState(ACPState& s);
	void SetState(const ACPState& s);
};
