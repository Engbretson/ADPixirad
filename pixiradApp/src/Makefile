TOP=../..
include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE

LIBRARY_IOC += pixirad

LIB_SRCS += pixirad.cpp
LIB_SRCS += PIXIEII_data_utilities.cpp

LIB_LIBS += MaddenPlugins

# Need to link DLL with WinSock library
LIB_SYS_LIBS_WIN32 += ws2_32

DBD += pixiradSupport.dbd

include $(ADCORE)/ADApp/commonLibraryMakefile

#=============================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

