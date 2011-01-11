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

all: openpanel-authd.exe runas_ fcat_
	grace mkapp openpanel-authd

runas_: 
	cd runas && $(MAKE) 
	
fcat_: 
	cd fcat && $(MAKE) 

version.cpp:
	grace mkversion version.cpp

openpanel-authd.exe: $(OBJ)
	$(LD) $(LDFLAGS) -o openpanel-authd.exe $(OBJ) $(LIBS)

install:
	mkdir -p ${DESTDIR}/etc/init.d
	mkdir -p ${DESTDIR}/var/openpanel/log
	mkdir -p ${DESTDIR}/var/openpanel/bin
	mkdir -p ${DESTDIR}/var/openpanel/tools
	mkdir -p ${DESTDIR}/var/openpanel/taskqueue
	mkdir -p ${DESTDIR}/var/openpanel/sockets/authd
	
	cp -rf openpanel-authd.app ${DESTDIR}/var/openpanel/bin/openpanel-authd.app
	ln -sf openpanel-authd.app/exec ${DESTDIR}/var/openpanel/bin/openpanel-authd
	cp -rf opencore-tools/* ${DESTDIR}/var/openpanel/tools/
	cp -f fcat/fcat ${DESTDIR}/var/openpanel/tools/
	cp -f runas/runas ${DESTDIR}/var/openpanel/tools/
	install -m 755 contrib/debian.init ${DESTDIR}/etc/init.d/openpanel-authd

clean:
	rm -f *.o *.exe
	rm -rf openpanel-authd.app
	rm -f openpanel-authd
	cd runas && $(MAKE) clean
	
SUFFIXES: .cpp .o
.cpp.o:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $<
