WORKDIR = `pwd`

CC = gcc
CXX = g++
AR = ar
LD = ld
WINDRES = 

INC = 
CFLAGS = -m32 -fPIC -D__LINUX__
RESINC = 
LIBDIR = 
LIB = 
LDFLAGS = -m32 -shared

INC_RELEASE = $(INC)
CFLAGS_RELEASE = $(CXX) $(CFLAGS) -O0 
RESINC_RELEASE = $(RESINC)
RCFLAGS_RELEASE = $(RCFLAGS)
LIBDIR_RELEASE = $(LIBDIR)
LIB_RELEASE = $(LIB) 
LDFLAGS_RELEASE = $(CXX) $(LDFLAGS) #-s
OBJDIR_RELEASE = obj/Release
DEP_RELEASE = 
OUT_RELEASE = Core.so

OBJ_RELEASE = $(OBJDIR_RELEASE)/UnObj.o $(OBJDIR_RELEASE)/UnCorSc.o

all: release

clean: clean_release

before_release: 
	test -d $(OBJDIR_RELEASE) || mkdir -p $(OBJDIR_RELEASE)

after_release: 

release: before_release out_release after_release

out_release: before_release $(OBJ_RELEASE) $(DEP_RELEASE)
	$(LDFLAGS_RELEASE) $(LIBDIR_RELEASE) $(OBJ_RELEASE) $(LIB_RELEASE) -o Core.so

$(OBJDIR_RELEASE)/UnObj.o: UnObj.cpp
	$(CFLAGS_RELEASE) $(INC_RELEASE) -c UnObj.cpp -o $(OBJDIR_RELEASE)/UnObj.o

$(OBJDIR_RELEASE)/UnCorSc.o: UnCorSc.cpp
	$(CFLAGS_RELEASE) $(INC_RELEASE) -c UnCorSc.cpp -o $(OBJDIR_RELEASE)/UnCorSc.o

clean_release: 
	rm -f $(OBJ_RELEASE) $(OUT_RELEASE)
	rm -rf $(OBJDIR_RELEASE)

.PHONY: before_release after_release clean_release
