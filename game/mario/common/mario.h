extern "C" {
#include "libsm64.h"
// load_surfaces.h transitively pulls in ultratypes.h which typedefs s64/u64
// as 'signed long long int'/'unsigned long long int'.  On Linux, int64_t
// (used by common_types.h for s64) is 'long', making these distinct types
// and producing a hard "type alias redefinition" error on clang.
// Redirect the conflicting typedefs to private throwaway aliases for the
// duration of this include.  The function declarations in load_surfaces.h
// itself don't use s64/u64 in their signatures, so this is safe.
#define s64 s64__libsm64_private_
#define u64 u64__libsm64_private_
#include "load_surfaces.h"
#undef s64
#undef u64
#include "decomp/tools/libmio0.h"
}