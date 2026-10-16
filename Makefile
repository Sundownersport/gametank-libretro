TARGET_NAME := gametank
platform ?= unix

CXX ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -fPIC -fno-exceptions -Wall -Isrc -Ilibretro-common/include
LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined

GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_VERSION),)
	CXXFLAGS += -DGIT_VERSION=\"\ $(GIT_VERSION)\"
endif

ifeq ($(platform), win)
	TARGET := $(TARGET_NAME)_libretro.dll
	LDFLAGS := -shared -static-libgcc -static-libstdc++ -Wl,--version-script=link.T
else ifeq ($(platform), osx)
	TARGET := $(TARGET_NAME)_libretro.dylib
	LDFLAGS := -dynamiclib
else
	TARGET := $(TARGET_NAME)_libretro.so
endif

SRCS := src/libretro.cpp \
	src/gametank.cpp \
	src/blitter.cpp \
	src/audio_coprocessor.cpp \
	src/mos6502/mos6502.cpp
OBJS := $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
