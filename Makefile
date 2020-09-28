.DEFAULT_GOAL := all
.PHONY: clean

V = @echo $@;

ifneq (,$(shell type clang 2>/dev/null))
  $(info Using clang)
  CC := clang++
  LD := clang++
else
  CC := g++
  LD := g++
endif

CFLAGS  := -ggdb3 -Wall -Werror -std=c++17
LDFLAGS := -lncurses -pthread

BUILDDIR := build
OBJDIR := $(BUILDDIR)/obj
DEPDIR := $(BUILDDIR)/dep
BINDIR := .

define objfile
$(patsubst %.cpp,$(OBJDIR)/%.o,$1)
endef

define depfile
$(patsubst %.cpp,$(DEPDIR)/%.d,$1)
endef

DST := $(BINDIR)/capuchinos
SRC := \
	main.cpp \
	ncctx.cpp \
	nc_lyt.cpp

OBJ := $(call objfile,$(SRC))
DEP := $(call depfile,$(SRC))

-include $(DEP)

all: $(DST)

$(DST): $(OBJ)
	$(V) \
	mkdir -p `dirname "$@"` && \
	$(LD) $(LDFLAGS) $^ -o $@

$(OBJDIR)/%.o: %.cpp Makefile
	$(V) \
	mkdir -p `dirname "$@"` && \
	mkdir -p `dirname "$(call depfile,$<)"` && \
	$(CC) $(CFLAGS) -MMD -MP -MF "$(call depfile,$<)" -c $< -o $@

clean:
	$(V)rm -rf $(OBJ) $(DEP) $(DST) $(BUILDDIR)