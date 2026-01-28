#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, const char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	DG_Init();

	D_DoomMain ();
}

