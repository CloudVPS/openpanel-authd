
# This file is part of OpenPanel - The Open Source Control Panel
# OpenPanel is free software: you can redistribute it and/or modify it 
# under the terms of the GNU General Public License as published by the Free 
# Software Foundation, using version 3 of the License.
#
# Please note that use of the OpenPanel trademark may be subject to additional 
# restrictions. For more information, please visit the Legal Information 
# section of the OpenPanel website on http://www.openpanel.com/

all: fcat

clean:
	rm -f fcat fcat.o

fcat: fcat.o
	$(CC) $(LDFLAGS) -o fcat fcat.o

fcat.o: fcat.c
	$(CC) $(CFLAGS) -c fcat.c
