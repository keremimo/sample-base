# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
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
# for src/drivers/panel_driver.cpp to load. The module is kept beside its
# source (tools/build_kernel_modules.sh rebuilds it), one for firmware
# 1.3.0.12's kernel and one (-6.18) for 1.4's; a build without one for the
# running kernel keeps the panel on the stock driver.
PANEL_MODULE ?= $(BASE_SRC)/../kernel/panel/sample_base_panel.ko
PANEL_MODULE_6_18 ?= $(BASE_SRC)/../kernel/panel/sample_base_panel-6.18.ko
PANEL_FILES := $(wildcard $(PANEL_MODULE) $(PANEL_MODULE_6_18))
PANEL_DEFINES := $(if $(wildcard $(PANEL_MODULE)),-DPANEL_MODULE='"$(PANEL_MODULE)"') \
	$(if $(wildcard $(PANEL_MODULE_6_18)),-DPANEL_MODULE_6_18='"$(PANEL_MODULE_6_18)"')
ifneq ($(PANEL_FILES),)
OBJECTS += build/obj/base/drivers/panel_module.o
$(TARGET): build/obj/base/drivers/panel_module.o
build/obj/base/drivers/panel_module.o: $(BASE_SRC)/drivers/panel_module.S $(PANEL_FILES)
	@mkdir -p $(@D)
	$(CC) -c $(PANEL_DEFINES) $< -o $@
endif
# Screen Mirror's modules (kernel/uvc, kernel/vepu; src/drivers/screen_mirror.h),
# embedded the same way, a pair for each kernel (1.4's uvc from
# kernel/uvc-6.18); a build without a pair for the running kernel has no
# Screen Mirror.
UVC_MODULE ?= $(BASE_SRC)/../kernel/uvc/sample_base_uvc.ko
VEPU_MODULE ?= $(BASE_SRC)/../kernel/vepu/sample_base_vepu.ko
UVC_MODULE_6_18 ?= $(BASE_SRC)/../kernel/uvc-6.18/sample_base_uvc-6.18.ko
VEPU_MODULE_6_18 ?= $(BASE_SRC)/../kernel/vepu/sample_base_vepu-6.18.ko
MIRROR_6_12 := $(and $(wildcard $(UVC_MODULE)),$(wildcard $(VEPU_MODULE)))
MIRROR_6_18 := $(and $(wildcard $(UVC_MODULE_6_18)),$(wildcard $(VEPU_MODULE_6_18)))
MIRROR_FILES := $(if $(MIRROR_6_12),$(UVC_MODULE) $(VEPU_MODULE)) $(if $(MIRROR_6_18),$(UVC_MODULE_6_18) $(VEPU_MODULE_6_18))
MIRROR_DEFINES := $(if $(MIRROR_6_12),-DUVC_MODULE='"$(UVC_MODULE)"' -DVEPU_MODULE='"$(VEPU_MODULE)"') \
	$(if $(MIRROR_6_18),-DUVC_MODULE_6_18='"$(UVC_MODULE_6_18)"' -DVEPU_MODULE_6_18='"$(VEPU_MODULE_6_18)"')
ifneq ($(strip $(MIRROR_FILES)),)
OBJECTS += build/obj/base/drivers/kernel_modules.o
$(TARGET): build/obj/base/drivers/kernel_modules.o
build/obj/base/drivers/kernel_modules.o: $(BASE_SRC)/drivers/kernel_modules.S $(MIRROR_FILES)
	@mkdir -p $(@D)
	$(CC) -c $(MIRROR_DEFINES) $< -o $@
endif
# File Sharing's module (kernel/ksmbd, kernel/ksmbd-6.18; src/drivers/file_share.h),
# embedded the same way, one for each kernel; a build without one for the
# running kernel has no File Sharing.
SMB_MODULE ?= $(BASE_SRC)/../kernel/ksmbd/sample_base_smb.ko
SMB_MODULE_6_18 ?= $(BASE_SRC)/../kernel/ksmbd-6.18/sample_base_smb-6.18.ko
SMB_FILES := $(wildcard $(SMB_MODULE) $(SMB_MODULE_6_18))
SMB_DEFINES := $(if $(wildcard $(SMB_MODULE)),-DSMB_MODULE='"$(SMB_MODULE)"') \
	$(if $(wildcard $(SMB_MODULE_6_18)),-DSMB_MODULE_6_18='"$(SMB_MODULE_6_18)"')
ifneq ($(SMB_FILES),)
OBJECTS += build/obj/base/drivers/smb_module.o
$(TARGET): build/obj/base/drivers/smb_module.o
build/obj/base/drivers/smb_module.o: $(BASE_SRC)/drivers/smb_module.S $(SMB_FILES)
	@mkdir -p $(@D)
	$(CC) -c $(SMB_DEFINES) $< -o $@
endif
-include $(OBJECTS:.o=.d)
