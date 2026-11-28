ARDUINO_CLI := arduino-cli

# Board profile from sketch.yaml: classic or v3s
PROFILE	?= classic
ifeq ($(PROFILE),v3s)
FQBN	= esp32.esp32.esp32s3
PORT	?= /dev/ttyACM0
else
FQBN	= esp32.esp32.esp32
PORT	?= /dev/ttyUSB0
endif
INO	= ats-mini.ino
ELF	= ./build/$(FQBN)/$(INO).elf

DEBUG_LEVEL ?= 0

#
# HALF_STEP       : Enable encoder half-steps
#
DEFINES = -DDEBUG=$(DEBUG_LEVEL)

ifdef HALF_STEP
        DEFINES += -DHALF_STEP
endif

OPTIONS = \
	--build-property "compiler.cpp.extra_flags=$(DEFINES)" \
	--warnings all

HEADERS = $(wildcard *.h)
SRC = $(INO) $(wildcard *.cpp)

all: build

help:
	@echo 'Run this command to upload the firmware to your radio:'
	@echo
	@echo '  make upload PROFILE=classic PORT=/dev/ttyUSB0'
	@echo '  make upload PROFILE=v3s PORT=/dev/ttyACM0'
	@echo

build: $(ELF)

$(ELF): $(INO) $(SRC) $(HEADERS)
	$(ARDUINO_CLI) compile -e -p $(PROFILE) $(OPTIONS)

upload: build
	$(ARDUINO_CLI) upload -m $(PROFILE) -p $(PORT)

clean:
	$(ARDUINO_CLI) cache clean
	rm -Rf ./build/


.PHONY: all help build upload clean
