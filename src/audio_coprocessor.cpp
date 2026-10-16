#include "audio_coprocessor.h"

void AudioCoprocessor::ram_write(uint16_t address, uint8_t value) {
	state.ram[address & 0xFFF] = value;
}

uint8_t AudioCoprocessor::ram_read(uint16_t address) {
	return state.ram[address & 0xFFF];
}

void AudioCoprocessor::register_write(uint16_t address, uint8_t value) {
	switch(address & 7) {
		case ACP_RESET:
			state.resetting = true;
			state.irqCounter = 255;
			break;
		case ACP_NMI:
			cpu->NMI();
			cpu->Run(state.cycles_per_sample, state.cycle_counter);
			break;
		case ACP_RATE:
			state.irqRate = (((value << 1) & 0xFE) | (value & 1));
			state.running = (value & 0x80) != 0;
			state.cycles_per_sample = state.irqRate * state.clkMult;
			break;
		default:
			break;
	}
}

void AudioCoprocessor::fill_audio(int16_t* stream, size_t len) {
	for(size_t i = 0; i < len; i++) {
		stream[i] = (int16_t) ((state.dacReg - 128) * 256);
		state.irqCounter -= clksPerHostSample;
		if(state.irqCounter < 0) {
			if(state.resetting) {
				state.resetting = false;
				cpu->Reset();
			}
			state.irqCounter += state.irqRate;
			state.cycle_counter = 0;
			if(state.running) {
				cpu->IRQ();
				cpu->ClearIRQ();
				cpu->Run(state.cycles_per_sample, state.cycle_counter);
			}
		}
	}
}

ACPState* AudioCoprocessor::singleton_acp_state;

static uint8_t ACP_MemoryRead(uint16_t address) {
	return AudioCoprocessor::singleton_acp_state->ram[address & 0xFFF];
}

static void ACP_MemoryWrite(uint16_t address, uint8_t value) {
	AudioCoprocessor::singleton_acp_state->ram[address & 0xFFF] = value;
	if(address & 0x8000) {
		AudioCoprocessor::singleton_acp_state->dacReg = value;
	}
}

static void ACP_CPUStopped() {
}

AudioCoprocessor::AudioCoprocessor() {
	AudioCoprocessor::singleton_acp_state = &state;

	cpu = new mos6502(ACP_MemoryRead, ACP_MemoryWrite, ACP_CPUStopped);

	state.irqCounter = 0;
	state.irqRate = 0;
	state.resetting = false;
	state.running = false;
	state.dacReg = 128;
	state.cycles_per_sample = 1024;
	state.clkMult = 4;
	state.cycle_counter = 0;
}

AudioCoprocessor::~AudioCoprocessor() {
	delete cpu;
}

void AudioCoprocessor::GetState(ACPState& s) {
	cpu->GetState(state.cpu);
	s = state;
}

void AudioCoprocessor::SetState(const ACPState& s) {
	state = s;
	cpu->SetState(state.cpu, NULL);
}
