// This file is part of OpenPanel - The Open Source Control Panel
// OpenPanel is free software: you can redistribute it and/or modify it 
// under the terms of the GNU General Public License as published by the Free 
// Software Foundation, using version 3 of the License.
//
// Please note that use of the OpenPanel trademark may be subject to additional 
// restrictions. For more information, please visit the Legal Information 
// section of the OpenPanel website on http://www.openpanel.com/

/* ======================================================================== *\
 | fcat: output a single file only if it is a verified regular file and     |
 |       not a softlink or other special file.                              |
\* ======================================================================== */

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

int main (int argc, char *argv[])
{
	const char *filename;
	int fd;
	char buffer[8192];
	size_t insz;
	size_t outsz;
	struct stat st;
	struct stat ost;
	
	/* Verify command line arguments */
	if (argc<2)
	{
		fprintf (stderr, "%% Usage: %s filename\n", argv[0]);
		return 1;
	}
	
	filename = argv[1];
	
	/* Get stat for the file (do not follow symlinks) */
	if (lstat (filename, &ost))
	{
		fprintf (stderr, "%% Could not stat file: %s\n", strerror (errno));
		return 1;
	}
	
	/* If this is not a regular file, we are not interested */
	if ((ost.st_mode & S_IFMT) != S_IFREG)
	{
		fprintf (stderr, "%% Aborting: not a regular file\n");
		return 1;
	}
	
	/* Try to open the file */
	fd = open (filename, O_RDONLY, 0);
	if (fd < 0)
	{
		fprintf (stderr, "%% Could not open file: %s\n", strerror (errno));
		return 1;
	}
	
	/* Get the stat info for the opened file */
	if (fstat (fd, &st) != 0)
	{
		fprintf (stderr, "%% Could not fstat: %s\n", strerror (errno));
		close (fd);
		return 1;
	}
	
	/* If the inode differs from the original lstat() inode, someone changed
	   the file on us, bail out */
	if (st.st_ino != ost.st_ino)
	{
		fprintf (stderr, "%% Aborting: sneaky switcharoo\n");
		close (fd);
		return 1;
	}
	
	/* Let's make sure we're not following a hardlink */
	if (st.st_nlink > 1)
	{
		fprintf (stderr, "%% Aborting: cowardly hardlink\n");
		close (fd);
		return 1;
	}
	
	/* To err on the safe side, re-check that we're a regular file */
	if ((st.st_mode & S_IFMT) != S_IFREG)
	{
		fprintf (stderr, "%% Aborting: not a regular file\n");
		close (fd);
		return 1;
	}
	
	/* The actual read/write loop */
	while (1)
	{
		insz = read (fd, buffer, 8192);
		if (insz<0)
		{
			fprintf (stderr, "%% Read error: %s\n", strerror (errno));
			close (fd);
			return 1;
		}
		
		if (insz == 0) break;
		outsz = write (1, buffer, insz);
		
		if (outsz < insz)
		{
			fprintf (stderr, "%% Error writing: %s\n", strerror (errno));
			close (fd);
			return 1;
		}
	}
	close (fd);
	return 0;
}
