# Warriors Orochi PC port: build shortcuts. Every step calls Python, so this
# works with GNU make from cmd, PowerShell or Git Bash on Windows.
# No make yet? winget install ezwinports.make (then open a new terminal).
#
# First time, with "Warriors Orochi (USA).iso" next to this folder:
#   make deps       install what is missing and download PS2Recomp and PCSX2
#   make check      check that everything is present
#   make generate   read the ISO and generate the game code (first time: long)
#   make install    compile and fill the "Warriors Orochi" player folder
#   make test       run the regression tests
# Day to day, after editing the code: "make reinstall" (or just "make").
# It compiles only what changed and puts the new programs in the game folder;
# the build is reused, never redone from scratch. Saves and settings stay.
# To send everything to someone: "make package".

PYTHON ?= python
HELPER := $(PYTHON) tools/make_helper.py

.PHONY: default all help deps check generate game tools analyze ghidra build install reinstall test package run

default: install

all: deps check generate install test

help:
	$(HELPER) help

# Python itself is needed for every other step: install it first if missing.
deps:
	$(PYTHON) --version || winget install --id Python.Python.3.12 -e --accept-source-agreements --accept-package-agreements
	$(HELPER) deps

check:
	$(HELPER) check

# "generate", one part at a time, in order.
game:
	$(HELPER) pathcheck
	$(PYTHON) tools/inspect_game.py
tools: game
	$(PYTHON) tools/workflow.py tools
analyze: tools
	$(PYTHON) tools/workflow.py analyze
generate: analyze
	$(PYTHON) tools/workflow.py generate

# Optional: rebuild the address map with Ghidra (config/orochi.ranges.csv is used otherwise).
ghidra:
	$(PYTHON) tools/download_analysis_tools.py
	$(PYTHON) tools/analyze_ghidra.py

build:
	$(HELPER) build

install:
	$(HELPER) install

# After editing the code: update patches and build configuration (new .cpp files too),
# compile only what changed, replace the programs in the game folder.
reinstall:
	$(HELPER) reinstall

test:
	$(PYTHON) tools/run_regressions.py

package:
	$(HELPER) package

run:
	$(HELPER) run
