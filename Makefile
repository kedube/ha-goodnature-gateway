# Convenience targets around `esphome`. Everything reads settings.yaml and
# secrets.yaml; override on the command line if needed:
#
#   make flash                          # USB flash using upload_port from settings.yaml
#   make flash UPLOAD_PORT=/dev/ttyACM0 # ...or a one-off port
#   make ota                            # over the air to ota_address
#   make logs                           # network logs (API)
#   make logs-serial                    # serial logs on upload_port
#   make states                         # every entity the gateway reports, via the API
#   make capture                        # Debug Mode on, stream raw BLE data for 60 s
#   make capture SECONDS=120 POLL=1     # ...longer, and poll every trap first
#
YAML     ?= goodnature-gateway.yaml
SETTINGS ?= settings.yaml
ESPHOME  ?= esphome
# tools/gateway_inspect.py needs aioesphomeapi, which ships inside ESPHome's
# own Python; borrow that interpreter unless PYTHON is set.
PYTHON   ?= $(shell head -1 "$$(command -v $(ESPHOME))" 2>/dev/null | sed -n 's/^\#!//p')
ifeq ($(PYTHON),)
PYTHON   := python3
endif
SECONDS  ?= 60
POLL     ?=

# Pull values out of settings.yaml without needing a YAML parser.
_setting = $(shell sed -n 's/^[[:space:]]*$(1):[[:space:]]*"\([^"]*\)".*/\1/p' $(SETTINGS) 2>/dev/null | head -1)
UPLOAD_PORT ?= $(call _setting,upload_port)
OTA_ADDRESS ?= $(call _setting,ota_address)
DEVICE_NAME ?= $(call _setting,device_name)
# ota_address may reference ${device_name}; resolve that one substitution here.
OTA_ADDRESS := $(subst $${device_name},$(DEVICE_NAME),$(OTA_ADDRESS))

PORT_FLAG = $(if $(UPLOAD_PORT),--device $(UPLOAD_PORT),)
OTA_FLAG  = $(if $(OTA_ADDRESS),--device $(OTA_ADDRESS),--device OTA)

.PHONY: help setup config compile flash ota upload logs logs-serial states capture test clean

help:
	@sed -n 's/^#   //p' Makefile

setup: ## create settings.yaml / secrets.yaml from the examples if missing
	@test -f settings.yaml || cp settings-example.yaml settings.yaml
	@test -f secrets.yaml || cp secrets-example.yaml secrets.yaml
	@echo "Edit settings.yaml and secrets.yaml, then: make flash"

config: ## validate and print the resolved configuration
	$(ESPHOME) config $(YAML)

compile: ## build the firmware
	$(ESPHOME) compile $(YAML)

flash: ## compile and flash over USB (upload_port from settings.yaml, or prompt)
	$(ESPHOME) run $(YAML) $(PORT_FLAG)

ota upload: ## compile and upload over the air to ota_address
	$(ESPHOME) run $(YAML) $(OTA_FLAG)

logs: ## follow logs over the network
	$(ESPHOME) logs $(YAML) $(OTA_FLAG)

logs-serial: ## follow logs over the serial port
	$(ESPHOME) logs $(YAML) $(PORT_FLAG)

states: ## print every entity state the gateway reports over the API
	$(PYTHON) tools/gateway_inspect.py states

capture: ## Debug Mode on, stream decoded advertisements, GATT frames and logs (SECONDS=, POLL=1)
	$(PYTHON) tools/gateway_inspect.py capture --seconds $(SECONDS) $(if $(POLL),--poll,)

test: ## run the host-side protocol tests
	./tests/run_tests.sh

clean: ## remove the ESPHome build directory
	$(ESPHOME) clean $(YAML)
	rm -rf .esphome
