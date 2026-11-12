# SPDX-License-Identifier: AGPL-3.0-only
# The sample-base build: JUCE, the base layers (surface/, dsp/, drivers/,
# runtime/) and the app sources the including Makefile lists. This
# repository's Makefile includes it, and so does an app kept in its own
# repository with sample-base as a submodule (docs/APPS.md, "An app in its
# own repository").
#
# Set before including:
#   BASE_SRC     the base's src/ folder, from the including Makefile (default: src)
#   APP_SOURCES  the app's .cpp files under its own src/, main.cpp among them
#   TARGET       the binary (default: build/sample-base)
# JUCE comes from build/deps/JUCE, which tools/dependencies.py fetches.
BASE_SRC ?= src
TARGET ?= build/sample-base
CXX ?= g++
JUCE := build/deps/JUCE/modules
BASE_LAYERS := surface dsp drivers runtime
CPPFLAGS := -Isrc -I$(BASE_SRC) -I$(JUCE) $(shell pkg-config --cflags freetype2 fontconfig alsa libdrm libsystemd) \
    -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG
CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP
CFLAGS := -O2 -MMD -MP
LDLIBS := $(shell pkg-config --libs freetype2 fontconfig alsa libdrm libsystemd) -ldl -pthread -lrt
MODULE_CPP := $(wildcard $(JUCE)/juce_core/*.cpp $(JUCE)/juce_events/*.cpp $(JUCE)/juce_graphics/*.cpp $(JUCE)/juce_audio_basics/*.cpp)
MODULE_C := $(wildcard $(JUCE)/juce_core/*.c $(JUCE)/juce_graphics/*.c)
BASE_SOURCES := $(foreach layer,$(BASE_LAYERS),$(wildcard $(BASE_SRC)/$(layer)/*.cpp $(BASE_SRC)/$(layer)/*/*.cpp))
# Objects mirror the tree: JUCE in build/obj/, the base in build/obj/base/,
# the app in build/obj/.
APP_ONLY := $(filter-out $(BASE_SOURCES),$(APP_SOURCES))
OBJECTS := $(addprefix build/obj/,$(notdir $(MODULE_CPP:.cpp=.o) $(MODULE_C:.c=.o))) \
    $(patsubst $(BASE_SRC)/%.cpp,build/obj/base/%.o,$(BASE_SOURCES)) \
    $(patsubst src/%.cpp,build/obj/%.o,$(APP_ONLY))

all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDLIBS) -o $@
build/obj/base/%.o: $(BASE_SRC)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -c $< -o $@
build/obj/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -c $< -o $@
define module_rule
build/obj/$(notdir $(1:.cpp=.o)): $(1)
	@mkdir -p $$(@D)
	$$(CXX) $$(CPPFLAGS) $$(CXXFLAGS) -c $$< -o $$@
endef
$(foreach m,$(MODULE_CPP),$(eval $(call module_rule,$(m))))
define c_module_rule
build/obj/$(notdir $(1:.c=.o)): $(1)
	@mkdir -p $$(@D)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) -c $$< -o $$@
endef
$(foreach m,$(MODULE_C),$(eval $(call c_module_rule,$(m))))
# The panel's own driver (kernel/panel, docs/HARDWARE.md, Display), embedded
# for src/drivers/panel_driver.cpp to load when tools/build_kernel_modules.sh
# has built it; without it the panel stays on the stock driver.
PANEL_MODULE ?= $(BASE_SRC)/../kernel/panel/sample_base_panel.ko
ifneq ($(wildcard $(PANEL_MODULE)),)
OBJECTS += build/obj/base/drivers/panel_module.o
$(TARGET): build/obj/base/drivers/panel_module.o
build/obj/base/drivers/panel_module.o: $(BASE_SRC)/drivers/panel_module.S $(PANEL_MODULE)
	@mkdir -p $(@D)
	$(CC) -c -DPANEL_MODULE='"$(PANEL_MODULE)"' $< -o $@
endif
-include $(OBJECTS:.o=.d)
