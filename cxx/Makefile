# The build of c++ocamlc, c++ocamlopt and their stage tools (GNU make).
#
#   make -C cxx            the release build, in cxx/build-release/
#   make -C cxx debug      the debug build, in cxx/build/
#   make -C cxx <tool>     one tool of the release build (c++ocamlc, ...);
#                          MODE=debug for the debug build's
#   make -C cxx clean      both builds removed
#
# Settings (on the command line, in the environment, or in cxx/Makefile.local,
# which is read first and not under version control):
#   CXX         the C++23 compiler (default g++)
#   OPTFLAGS    the optimisation flags (release -O3 -DNDEBUG, debug -g)
#   CXXFLAGS, CPPFLAGS, LDFLAGS   added to the compiler's / the linker's
#   MIMALLOC    the directories to find mimalloc's static allocator override
#               in (mimalloc.o, mimalloc.h: directly, or in lib/ and
#               include/); default: CMAKE_PREFIX_PATH's, /usr/local, /usr.
#               MIMALLOC=no builds without it (slower).
#   ZSTD_LIBS   libzstd's link flags; default configure's (Makefile.config):
#               compressed marshaled output is byte-identical only with the
#               libzstd the OCaml runtime itself uses.  Empty: without zstd
#               (c++ocamlc then serves only installations without it).
#   ZSTD_CFLAGS zstd.h's -I, when it is not on the default path

-include Makefile.local
# (configure's choice, as the runtime's)
ZSTD_LIBS ?= $(shell sed -n 's/^ZSTD_LIBS=//p' ../Makefile.config 2>/dev/null)

CXX ?= g++
AR ?= ar
MODE ?= release

ifeq "$(MODE)" "release"
O ?= build-release
OPTFLAGS ?= -O3 -DNDEBUG
else ifeq "$(MODE)" "debug"
O ?= build
OPTFLAGS ?= -g
else
$(error MODE is release or debug, not $(MODE))
endif

# V=1: the commands themselves (as OCaml's Makefile.common)
V ?= 0
ifeq "$(V)" "0"
V_CXX = @$(info $(SPACE)  CXX $@)
V_AR = @$(info $(SPACE)  AR $@)
V_LINK = @$(info $(SPACE)  LINK $@)
endif
SPACE :=

TOOLS := c++ocamlc c++ocamlopt c++lex c++parse c++cmxinfo c++typing-dump

.PHONY: all release debug clean tools $(TOOLS)
all: release
release:
	@$(MAKE) --no-print-directory MODE=release tools
debug:
	@$(MAKE) --no-print-directory MODE=debug tools
$(TOOLS):
	@$(MAKE) --no-print-directory TOOL=$@ one
.PHONY: one
one: $(O)/$(TOOL)
	@:
tools: $(addprefix $(O)/,$(TOOLS))
	@:
clean:
	rm -rf build build-release

# ---- mimalloc ---------------------------------------------------------------
# (statically linked: the compiler is allocation-bound, and the override
# object, rather than -lmimalloc, always interposes libc's malloc)
ifneq "$(MIMALLOC)" "no"
MIMALLOC_DIRS := $(or $(MIMALLOC),\
  $(subst :, ,$(CMAKE_PREFIX_PATH)) /usr/local /usr)
mimalloc_find = $(firstword $(wildcard \
  $(foreach d,$(MIMALLOC_DIRS),$(d)/$(1) $(d)/$(2)/$(1))))
MIMALLOC_O := $(call mimalloc_find,mimalloc.o,lib)
MIMALLOC_H := $(call mimalloc_find,mimalloc.h,include)
MIMALLOC_MISSING := $(if $(MIMALLOC_O),,mimalloc.o not found in \
  $(MIMALLOC_DIRS): set MIMALLOC to its directory, or MIMALLOC=no)
endif

# ---- flags ------------------------------------------------------------------
DEFINES := $(if $(ZSTD_LIBS),-DCPPCAML_HAVE_ZSTD=1) \
  $(if $(MIMALLOC_H),-DCPPCAML_HAVE_MIMALLOC=1 -I$(dir $(MIMALLOC_H)))
ALL_CXXFLAGS := -std=c++23 $(OPTFLAGS) \
  -Wall -Wextra -Wpedantic -Wno-unused-parameter \
  -Iinclude $(DEFINES) $(ZSTD_CFLAGS) $(CPPFLAGS) $(CXXFLAGS)
# The typing/ port initializes aggregates partially on purpose (an OCaml
# constructor's unused fields stay defaulted).
PARTIAL_INIT := -Wno-missing-field-initializers
LIBS := $(MIMALLOC_O) $(ZSTD_LIBS)
# A compiler process is short-lived and started once per unit: linking the
# C++ runtime in saves the dynamic loader's symbol resolution and makes the
# binary self-contained; its GNU build-id keys the .cmi image cache
# (cmi_image.hpp).
COMPILER_LDFLAGS := -static-libstdc++ -static-libgcc -Wl,--build-id=sha1

# a changed flag rebuilds everything (the stamp is rewritten only then)
FLAGS_STAMP := $(O)/flags
$(shell mkdir -p $(O) && \
  echo '$(CXX) $(ALL_CXXFLAGS) | $(LDFLAGS) $(LIBS)' > $(O)/flags.new && \
  (cmp -s $(O)/flags.new $(FLAGS_STAMP) || mv $(O)/flags.new $(FLAGS_STAMP)); \
  rm -f $(O)/flags.new)

# ---- objects ----------------------------------------------------------------
CORE_SRCS := $(wildcard src/*.cpp src/typing/*.cpp)
CORE_OBJS := $(CORE_SRCS:%.cpp=$(O)/%.o)

# (before compiling anything)
.PHONY: check-mimalloc
check-mimalloc:
	$(if $(MIMALLOC_MISSING),$(error $(MIMALLOC_MISSING)))@:

$(O)/%.o: %.cpp $(FLAGS_STAMP) | check-mimalloc
	@mkdir -p $(@D)
	$(V_CXX)$(CXX) $(ALL_CXXFLAGS) $(EXTRA_CXXFLAGS) -MMD -MP -c $< -o $@

$(O)/src/typing/%.o $(O)/src/tools/typing_dump_main.o: \
  EXTRA_CXXFLAGS := $(PARTIAL_INIT)

# c++ocamlopt: c++ocamlc's driver as Optmaindriver / Optcompile
$(O)/src/tools/cppocamlopt_main.o: src/tools/cppocamlc_main.cpp $(FLAGS_STAMP) \
  | check-mimalloc
	@mkdir -p $(@D)
	$(V_CXX)$(CXX) $(ALL_CXXFLAGS) -DCPPCAML_OCAMLOPT=1 -MMD -MP -c $< -o $@

$(O)/libcppcaml_core.a: $(CORE_OBJS)
	$(V_AR)rm -f $@ && $(AR) rcs $@ $^

# ---- tools ------------------------------------------------------------------
LINK = $(V_LINK)$(CXX) \
  $(OPTFLAGS) $(LDFLAGS) -o $@ $< $(O)/libcppcaml_core.a $(LIBS)

$(O)/c++ocamlc: $(O)/src/tools/cppocamlc_main.o $(O)/libcppcaml_core.a
	$(LINK) $(COMPILER_LDFLAGS)
$(O)/c++ocamlopt: $(O)/src/tools/cppocamlopt_main.o $(O)/libcppcaml_core.a
	$(LINK) $(COMPILER_LDFLAGS)
$(O)/c++lex: $(O)/src/tools/cpplex_main.o $(O)/libcppcaml_core.a
	$(LINK)
$(O)/c++parse: $(O)/src/tools/cppparse_main.o $(O)/libcppcaml_core.a
	$(LINK)
$(O)/c++cmxinfo: $(O)/src/tools/cmxinfo_main.o $(O)/libcppcaml_core.a
	$(LINK)
$(O)/c++typing-dump: $(O)/src/tools/typing_dump_main.o $(O)/libcppcaml_core.a
	$(LINK)

-include $(wildcard $(O)/src/*.d $(O)/src/typing/*.d $(O)/src/tools/*.d)
