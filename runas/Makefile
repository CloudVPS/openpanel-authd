# This file is part of OpenPanel - The Open Source Control Panel
# OpenPanel is free software: you can redistribute it and/or modify it 
# under the terms of the GNU General Public License as published by the Free 
# Software Foundation, using version 3 of the License.
#
# Please note that use of the OpenPanel trademark may be subject to additional 
# restrictions. For more information, please visit the Legal Information 
# section of the OpenPanel website on http://www.openpanel.com/

all: runas

clean:
	rm -f runas runas.o

runas: runas.o
	$(CC) $(LDFLAGS) -o runas runas.o

runas.o: runas.c
	$(CC) $(CFLAGS) -c runas.c
