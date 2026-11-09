CXX ?= g++
JUCE := build/deps/JUCE/modules
CPPFLAGS := -I$(JUCE) $(shell pkg-config --cflags freetype2 fontconfig alsa libdrm) \
    -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG
CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP
CFLAGS := -O2 -MMD -MP
LDLIBS := $(shell pkg-config --libs freetype2 fontconfig alsa libdrm) -ldl -pthread -lrt
MODULE_CPP := $(wildcard $(JUCE)/juce_core/*.cpp $(JUCE)/juce_events/*.cpp $(JUCE)/juce_graphics/*.cpp)
MODULE_C := $(wildcard $(JUCE)/juce_core/*.c $(JUCE)/juce_graphics/*.c)
OBJECTS := $(addprefix build/obj/,$(notdir $(MODULE_CPP:.cpp=.o) $(MODULE_C:.c=.o))) \
    build/obj/main.o build/obj/display.o build/obj/screen.o

all: build/sample-base
build/sample-base: $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDLIBS) -o $@
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
-include $(OBJECTS:.o=.d)
