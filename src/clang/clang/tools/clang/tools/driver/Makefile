##===- tools/driver/Makefile -------------------------------*- Makefile -*-===##
# 
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
# 
##===----------------------------------------------------------------------===##
LEVEL = ../../../..

TOOLNAME = clang
CPPFLAGS += -I$(PROJ_SRC_DIR)/../../include -I$(PROJ_OBJ_DIR)/../../include
CXXFLAGS = -fno-rtti

# This tool has no plugins, optimize startup time.
TOOL_NO_EXPORTS = 1

# FIXME: It is unfortunate we need to pull in the bitcode reader and
# writer just to get the serializer stuff used by clangBasic.
LINK_COMPONENTS := system support bitreader bitwriter
USEDLIBS = clangDriver.a clangBasic.a

include $(LEVEL)/Makefile.common
