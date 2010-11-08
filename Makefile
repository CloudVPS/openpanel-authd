# This file is part of OpenPanel - The Open Source Control Panel
# OpenPanel is free software: you can redistribute it and/or modify it 
# under the terms of the GNU General Public License as published by the Free 
# Software Foundation, using version 3 of the License.
#
# Please note that use of the OpenPanel trademark may be subject to additional 
# restrictions. For more information, please visit the Legal Information 
# section of the OpenPanel website on http://www.openpanel.com/

include makeinclude

OBJ	= main.o version.o

all: authd.exe
	grace mkapp authd

version.cpp:
	grace mkversion version.cpp

authd.exe: $(OBJ)
	$(LD) $(LDFLAGS) -o authd.exe $(OBJ) $(LIBS)

clean:
	rm -f *.o *.exe
	rm -rf authd.app
	rm -f authd

SUFFIXES: .cpp .o
.cpp.o:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $<
