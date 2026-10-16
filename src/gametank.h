#pragma once
#include <cstddef>
#include <cstdint>

namespace GameTank {

namespace Buttons {
	enum Mask : uint16_t {
		UP    = 0b0000100000001000,
		DOWN  = 0b0000010000000100,
		LEFT  = 0b0000001000000000,
		RIGHT = 0b0000000100000000,
		A     = 0b0000000000010000,
		B     = 0b0001000000000000,
		C     = 0b0010000000000000,
		START = 0b0000000000100000,
	};
}

const int WIDTH = 128;
const int HEIGHT = 128;
const double SYSTEM_CLOCK = 315000000.0 / 88.0;

bool Init();
void Deinit();
bool LoadRom(const uint8_t* data, size_t size);
void Reset();

void SetButtons(uint16_t pad1, uint16_t pad2);
void SetInstantBlit(bool enabled);

// Runs one vsync worth of CPU time, appends mono samples to audio and returns their count.
size_t RunFrame(int16_t* audio, size_t max_samples);
uint32_t CyclesPerFrame();

const uint8_t* VisibleFramebuffer();

uint8_t* SystemRam();
size_t SystemRamSize();
uint8_t* SaveRam();
size_t SaveRamSize();

bool HasFlash();
uint8_t* FlashData();
size_t FlashSize();
const uint8_t* OriginalRom();
bool TakeFlashSaveRequest();

size_t StateSize();
bool SaveState(void* data, size_t size);
bool LoadState(const void* data, size_t size);

}
