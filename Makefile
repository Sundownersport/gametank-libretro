TARGET_NAME := gametank
CORE_DIR := .
STATIC_LINKING := 0

ifeq ($(platform),)
	platform = unix
	ifeq ($(shell uname -s),)
		platform = win
	else ifneq ($(findstring MINGW,$(shell uname -s)),)
		platform = win
	else ifneq ($(findstring Darwin,$(shell uname -s)),)
		platform = osx
	endif
endif

GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_VERSION),)
	CXXFLAGS += -DGIT_VERSION=\"\ $(GIT_VERSION)\"
endif

ifeq ($(platform), unix)
	TARGET := $(TARGET_NAME)_libretro.so
	fpic := -fPIC
	SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined

else ifeq ($(platform), osx)
	TARGET := $(TARGET_NAME)_libretro.dylib
	fpic := -fPIC
	SHARED := -dynamiclib
	ifeq ($(CROSS_COMPILE),1)
		TARGET_RULE := -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
		CXXFLAGS += $(TARGET_RULE)
		LDFLAGS += $(TARGET_RULE)
	endif
	CXXFLAGS += $(ARCHFLAGS)
	LDFLAGS += $(ARCHFLAGS)

else ifneq (,$(findstring ios,$(platform)))
	TARGET := $(TARGET_NAME)_libretro_ios.dylib
	fpic := -fPIC
	SHARED := -dynamiclib
	ifeq ($(IOSSDK),)
		IOSSDK := $(shell xcodebuild -version -sdk iphoneos Path)
	endif
	CXX = c++ -arch arm64 -isysroot $(IOSSDK) -miphoneos-version-min=11.0

else ifeq ($(platform), tvos-arm64)
	TARGET := $(TARGET_NAME)_libretro_tvos.dylib
	fpic := -fPIC
	SHARED := -dynamiclib
	ifeq ($(IOSSDK),)
		IOSSDK := $(shell xcodebuild -version -sdk appletvos Path)
	endif
	CXX = c++ -arch arm64 -isysroot $(IOSSDK) -mappletvos-version-min=11.0

else ifeq ($(platform), emscripten)
	TARGET := $(TARGET_NAME)_libretro_$(platform).bc
	STATIC_LINKING := 1

else ifeq ($(platform), libnx)
	include $(DEVKITPRO)/libnx/switch_rules
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	PLATFORM_DEFINES := -D__SWITCH__ -DHAVE_LIBNX -I$(LIBNX)/include -fPIE \
		-ffunction-sections -fdata-sections -ftls-model=local-exec \
		-march=armv8-a -mtune=cortex-a57 -mtp=soft
	STATIC_LINKING := 1

else ifeq ($(platform), vita)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CXX = arm-vita-eabi-g++
	AR = arm-vita-eabi-ar
	PLATFORM_DEFINES := -DVITA
	STATIC_LINKING := 1

else ifeq ($(platform), ctr)
	TARGET := $(TARGET_NAME)_libretro_$(platform).a
	CXX = $(DEVKITARM)/bin/arm-none-eabi-g++
	AR = $(DEVKITARM)/bin/arm-none-eabi-ar
	PLATFORM_DEFINES := -DARM11 -D_3DS -march=armv6k -mtune=mpcore -mfloat-abi=hard -mword-relocations
	STATIC_LINKING := 1

else
	TARGET := $(TARGET_NAME)_libretro.dll
	SHARED := -shared -static-libgcc -static-libstdc++ -Wl,--version-script=link.T -Wl,--no-undefined
endif

ifeq ($(DEBUG), 1)
	CXXFLAGS += -O0 -g
else
	CXXFLAGS += -O2
endif

include Makefile.common

CXXFLAGS += -std=c++17 -fno-exceptions -Wall $(fpic) $(PLATFORM_DEFINES) $(INCFLAGS)
OBJS := $(SOURCES_CXX:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJS)
else
	$(CXX) $(fpic) $(SHARED) -o $@ $(OBJS) $(LDFLAGS)
endif

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
