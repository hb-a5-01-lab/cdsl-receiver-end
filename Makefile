CONFIG := $(HOME)/.config/cdsl/config.xml
GENERATED := component_config.hpp
GENERATOR := generate_config.py

FQBN ?= arduino:avr:uno
PORT := $(shell python3 -c 'import xml.etree.ElementTree as ET; print(ET.parse("$(CONFIG)").getroot().find("arduino/port").text)')

.PHONY: all generate compile flash clean

all: generate compile flash

generate:
	python3 $(GENERATOR) $(CONFIG) $(GENERATED)
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(GENERATED); \
	else \
		echo "clang-format not found; skipping formatting"; \
	fi

compile:
	arduino-cli compile \
		--fqbn $(FQBN) \
		.

flash:
	arduino-cli upload \
		-p $(PORT) \
		--fqbn $(FQBN) \
		.

clean:
	rm -f $(GENERATED)
