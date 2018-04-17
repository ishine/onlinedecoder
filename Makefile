all:

KALDI_ROOT=../../

ifeq ("$(wildcard $(KALDI_ROOT)/src/kaldi.mk)","")
$(error Cannot find Kaldi's makefile $(KALDI_ROOT)/src/kaldi.mk. \
Specify Kaldi's root directory using KALDI_ROOT when issuing make, e.g.: `KALDI_ROOT=/home/tanel/tools/kaldi-trunk make` )
endif

include $(KALDI_ROOT)/src/kaldi.mk
include $(KALDI_ROOT)/src/makefiles/default_rules.mk
ifneq ($(KALDI_FLAVOR), dynamic)
$(error Kaldi must compiled with dynamic libraries support. Run configure with --shared flag. )
endif

CXXFLAGS += -I$(KALDI_ROOT)/src

#EXTRA_CXXFLAGS += $(shell pkg-config --cflags gstreamer-1.0)
#EXTRA_CXXFLAGS += $(shell pkg-config --cflags glib-2.0)
EXTRA_CXXFLAGS += $(shell pkg-config --cflags jansson)

#EXTRA_LDLIBS += -lgstbase-1.0 -lgstcontroller-1.0 -lgobject-2.0 -lgmodule-2.0 -lgthread-2.0
#EXTRA_LDLIBS += $(shell pkg-config --libs gstreamer-1.0)
#EXTRA_LDLIBS += $(shell pkg-config --libs glib-2.0)
EXTRA_LDLIBS += $(shell pkg-config --libs jansson)

#Kaldi shared libraries required by the GStreamer plugin
EXTRA_LDLIBS += -lkaldi-online2 -lkaldi-lat -lkaldi-decoder -lkaldi-feat -lkaldi-transform \
 -lkaldi-gmm -lkaldi-hmm \
 -lkaldi-tree -lkaldi-matrix  -lkaldi-util -lkaldi-base -lkaldi-lm  \
 -lkaldi-nnet2 -lkaldi-nnet3 -lkaldi-cudamatrix -lkaldi-ivector -lkaldi-fstext -lkaldi-chain

OBJFILES = audio-buffer-source.o online-decoder.o 

LIBNAME=onlinedecoder

LIBFILE = lib$(LIBNAME).so
BINFILES= $(LIBFILE)

all: $(LIBFILE)

# MKL libs required when linked via shared library
ifdef MKLROOT
EXTRA_LDLIBS+=-lmkl_rt -lmkl_def
endif

# Library so name and rpath

CXX_VERSION=$(shell $(CXX) --version 2>/dev/null)
ifneq (,$(findstring clang, $(CXX_VERSION)))
    # clang++ linker
    EXTRA_LDLIBS +=  -Wl,-install_name,$(LIBFILE) -Wl,-rpath,$(KALDILIBDIR)
else
    # g++ linker
    EXTRA_LDLIBS +=  -Wl,-soname=$(LIBFILE) -Wl,--no-as-needed -Wl,-rpath=$(KALDILIBDIR) -lrt -pthread
endif


$(LIBFILE): $(OBJFILES)
	$(CXX) -shared -DPIC -o $(LIBFILE) -L$(KALDILIBDIR) $(EXTRA_LDLIBS) $(LDLIBS) $(LDFLAGS) \
	  $(OBJFILES)
 
clean: 
	-rm -f *.o *.a $(TESTFILES) $(BINFILES) 
 

-include .depend.mk

