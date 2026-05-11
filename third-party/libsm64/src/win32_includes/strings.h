/* Windows shim for strings.h */
#ifndef _STRINGS_H_SHIM
#define _STRINGS_H_SHIM
#include <string.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#endif
