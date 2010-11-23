// This file is part of OpenPanel - The Open Source Control Panel
// OpenPanel is free software: you can redistribute it and/or modify it 
// under the terms of the GNU General Public License as published by the Free 
// Software Foundation, using version 3 of the License.
//
// Please note that use of the OpenPanel trademark may be subject to additional 
// restrictions. For more information, please visit the Legal Information 
// section of the OpenPanel website on http://www.openpanel.com/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main (int argc, char *argv[])
{
	char cmdpath[1024];
	uid_t uid = 65534;
	gid_t gid = 65534;
	char **newargv = NULL;
	int newargc = 0;
	/* argv[1]: uid
	   argv[2]: gid
	   argv[3]: cmd */
	
	if (argc<4)
	{
		fprintf (stderr, "%% Not enough arguments\n");
		return 1;
	}
	
	newargv = argv+3;
	newargc = argc-3;
	
	uid = strtoul (argv[1],NULL,10);
	gid = strtoul (argv[2],NULL,10);
	
	if (strlen (argv[3]) > 256) return 1;
	if (argv[3][0] == '/')
	{
		strcpy (cmdpath, argv[3]);
	}
	else
	{
		sprintf (cmdpath, "/var/openpanel/tools/%s", argv[3]);
	}
	
	if (setregid (gid,gid)) return 1;
	if (setreuid (uid,uid)) return 1;
	
	return execv (cmdpath, newargv);
}
