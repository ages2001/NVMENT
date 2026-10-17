!ifdef MASTER_MAKE
BUILD_BITS=32
BUILD_TYPE=block
!INCLUDE $(DDKROOT)\master.mk
!endif

# Shared sources live one level up (src\); NVMENT_W9X selects the 9x target
INCLUDE =.;..;$(INCLUDE)

DEVICE          = nvment9x
OBJS            = nvmemain.obj nvmecmd.obj nvmesatl.obj nvmeutil.obj nvmecpl.obj

######### Definitions for the debug level #########

# DEBUG=1: compile in debug output
!ifdef DEBUG

DDEBUG =-DDEBLEVEL=1 -DDEBUG
CFLAGS =$(CFLAGS) -Oy1

!else

DDEBUG =-DDEBLEVEL=0

!endif

########## Definitions for C compiler #############

CL      = cl
CFLAGS  = -nologo -Zp4 -Gs -c -DIS_32 -D_X86_ -Zl -Gz -DDBG -DNVMENT_W9X -D_WIN32_WINNT=0x400 $(CFLAGS)
CFLAGS  = $(CFLAGS) $(DDEBUG)

########## Definitions for linker #############

LINK 	 = link

########## Base inference rules ###################

#       32 bit C compiler inference rules

.c.obj:
	set CL=$(CFLAGS)
	$(CL) -Fo$*.obj $<

############### VxD device section ####################

$(DEVICE).sym: $(DEVICE).map
	mapsym -s -o .\$(DEVICE).sym .\$(DEVICE).map > nul

$(DEVICE).map: $(DEVICE).mpd

$(DEVICE).mpd : $(OBJS) $(DEVICE).res $(LIBS) $(STUB)
	$(LINK) @$(DEVICE).lnk

# Shared sources (after the default target above)

nvmemain.obj: ..\nvmemain.c ..\nvment.h ..\nvme.h
	set CL=$(CFLAGS)
	$(CL) -Fonvmemain.obj ..\nvmemain.c

nvmecmd.obj: ..\nvmecmd.c ..\nvment.h ..\nvme.h
	set CL=$(CFLAGS)
	$(CL) -Fonvmecmd.obj ..\nvmecmd.c

nvmesatl.obj: ..\nvmesatl.c ..\nvment.h ..\nvme.h
	set CL=$(CFLAGS)
	$(CL) -Fonvmesatl.obj ..\nvmesatl.c

nvmeutil.obj: ..\nvmeutil.c ..\nvment.h ..\nvme.h
	set CL=$(CFLAGS)
	$(CL) -Fonvmeutil.obj ..\nvmeutil.c

nvmecpl.obj: ..\nvmecpl.c ..\nvment.h ..\nvme.h
	set CL=$(CFLAGS)
	$(CL) -Fonvmecpl.obj ..\nvmecpl.c

clean:
	-@del *.obj
	-@del *.mpd
	-@del *.map
	-@del *.pdb
	-@del *.sym

$(DEVICE).res: ..\nvment.rc
	rc -r -DNVMENT_W9X -D_WIN32_WINNT=0x400 -fo$(DEVICE).res ..\nvment.rc
