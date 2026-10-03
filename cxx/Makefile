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
#   CC          the C compiler, for mimalloc (default cc)
#   OPTFLAGS    the optimisation flags (release -O3 -DNDEBUG, debug -g)
#   CXXFLAGS, CPPFLAGS, LDFLAGS   added to the compiler's / the linker's
#   MIMALLOC=no without the vendored mimalloc allocator (vendor/mimalloc;
#               slower)
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
V_CC = @$(info $(SPACE)  CC $@)
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
# Statically linked: the compiler is allocation-bound.  Its static override
# object, as mimalloc's own build makes it (one translation unit,
# src/static.c), rather than a library: the object always interposes libc's
# malloc.  Arenas are committed as they are used, not eagerly: an eager
# commit makes every compiler fault in and zero memory it never uses (a
# parallel build's system time, a third of a small compilation's memory).
ifneq "$(MIMALLOC)" "no"
MIMALLOC_O := $(O)/mimalloc.o
MIMALLOC_CFLAGS := -O3 -DNDEBUG -std=gnu11 -fPIC -fvisibility=hidden \
  -ftls-model=initial-exec -fno-builtin-malloc \
  -DMI_BUILD_RELEASE -DMI_MALLOC_OVERRIDE -DMI_DEFAULT_ARENA_EAGER_COMMIT=0 \
  -Ivendor/mimalloc/include
endif

# ---- flags ------------------------------------------------------------------
DEFINES := $(if $(ZSTD_LIBS),-DCPPCAML_HAVE_ZSTD=1) \
  $(if $(MIMALLOC_O),-DCPPCAML_HAVE_MIMALLOC=1 -Ivendor/mimalloc/include)
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
  echo '$(CXX) $(ALL_CXXFLAGS) | $(LDFLAGS) $(LIBS)' \
    '| $(CC) $(MIMALLOC_CFLAGS)' > $(O)/flags.new && \
  (cmp -s $(O)/flags.new $(FLAGS_STAMP) || mv $(O)/flags.new $(FLAGS_STAMP)); \
  rm -f $(O)/flags.new)

# ---- objects ----------------------------------------------------------------
CORE_SRCS := $(wildcard src/*.cpp src/typing/*.cpp)
CORE_OBJS := $(CORE_SRCS:%.cpp=$(O)/%.o)

$(O)/%.o: %.cpp $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(V_CXX)$(CXX) $(ALL_CXXFLAGS) $(EXTRA_CXXFLAGS) -MMD -MP -c $< -o $@

$(O)/src/typing/%.o $(O)/src/tools/typing_dump_main.o: \
  EXTRA_CXXFLAGS := $(PARTIAL_INIT)

# c++ocamlopt: c++ocamlc's driver as Optmaindriver / Optcompile
$(O)/src/tools/cppocamlopt_main.o: src/tools/cppocamlc_main.cpp $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(V_CXX)$(CXX) $(ALL_CXXFLAGS) -DCPPCAML_OCAMLOPT=1 -MMD -MP -c $< -o $@

$(O)/mimalloc.o: vendor/mimalloc/src/static.c $(FLAGS_STAMP)
	$(V_CC)$(CC) $(MIMALLOC_CFLAGS) -MMD -MP -c $< -o $@

$(O)/libcppcaml_core.a: $(CORE_OBJS)
	$(V_AR)rm -f $@ && $(AR) rcs $@ $^

# ---- tools ------------------------------------------------------------------
CORE := $(O)/libcppcaml_core.a $(MIMALLOC_O)
LINK = $(V_LINK)$(CXX) \
  $(OPTFLAGS) $(LDFLAGS) -o $@ $< $(O)/libcppcaml_core.a $(LIBS)

$(O)/c++ocamlc: $(O)/src/tools/cppocamlc_main.o $(CORE)
	$(LINK) $(COMPILER_LDFLAGS)
$(O)/c++ocamlopt: $(O)/src/tools/cppocamlopt_main.o $(CORE)
	$(LINK) $(COMPILER_LDFLAGS)
$(O)/c++lex: $(O)/src/tools/cpplex_main.o $(CORE)
	$(LINK)
$(O)/c++parse: $(O)/src/tools/cppparse_main.o $(CORE)
	$(LINK)
$(O)/c++cmxinfo: $(O)/src/tools/cmxinfo_main.o $(CORE)
	$(LINK)
$(O)/c++typing-dump: $(O)/src/tools/typing_dump_main.o $(CORE)
	$(LINK)

-include $(wildcard $(O)/*.d $(O)/src/*.d $(O)/src/typing/*.d \
  $(O)/src/tools/*.d)
