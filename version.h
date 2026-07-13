/*$
	version.h
	Definizione della versione per ExplorerBgToolRe(dux).
	Luca Piergentili, 18/06/2026
*/
#ifndef _VERSION_H
#define _VERSION_H 1

#include "macro.h"
#include "versioning.h"

/*
	versione: <n>.<n>.<n>: <cambio architetturale/disegno/aggiunta funzionalita'>.<ampliazioni/riduzioni,revisioni>.<bugs,patches,etc>:
*/
#define MAJOR_VERSION	1
#define MINOR_VERSION	3
#define PATCH_VERSION	1
#define RELEASE_VERSION	0
#define RELEASE_TYPE	"" /*"(beta)"*/

/* per il .rc */
#define VER_MAJOR             MAJOR_VERSION
#define VER_MINOR             MINOR_VERSION
#define VER_REVISION          PATCH_VERSION
#define VER_BUILD             RELEASE_TYPE

#define VER_STR_VERSION       STR(VER_MAJOR)\
                              "."\
                              STR(VER_MINOR)\
                              "."\
                              STR(VER_REVISION)\
                              "."\
                              RELEASE_TYPE

#define VER_STR_VERSIONINFO   MAJOR_VERSION,MINOR_VERSION,PATCH_VERSION,RELEASE_VERSION

#define VER_STR_PROGRAM_NAME  "ExplorerBgToolRe"
#define VER_STR_PRODUCT_NAME  VER_STR_PROGRAM_NAME\
                              "(ExplorerBgToolRe)\0"
#define VER_STR_COPYRIGHT     "Copyright Luca Piergentili (c) 2026\0"

#endif /* _VERSION_H */
