/*
 * Default (no-op) logging hooks for the standalone `storage` library.
 *
 * storage.h instruments itself through a family of L_* logging macros that are
 * no-ops by default, so the library builds with zero dependency on any logging
 * header. The engine's behavior does not depend on them: they only emit
 * diagnostics (IO errors, debug traces) a host may want to surface.
 *
 * The macros used by storage.h:
 *
 *   - L_CALL(...)         — entry trace for each public method.
 *   - L_DEBUG(...)        — low-severity diagnostic.
 *   - L_ERR(...)          — an IO error about to be thrown as a Storage*Error.
 *   - L_WARNING(...)      — a recoverable problem (e.g. cannot grow the file).
 *   - L_WARNING_ONCE(...) — like L_WARNING but a host may de-duplicate it.
 *   - L_EXC(...)          — an exception swallowed in the destructor.
 *   - L_NOTHING(...)      — explicit no-op (used as a default for L_CALL).
 *
 * To restore real logging (the way Xapiand uses it), provide your own versions.
 * Two ways:
 *
 *   1. Define STORAGE_TRACE_HEADER to the path of a header that defines them,
 *      e.g.
 *        c++ -DSTORAGE_TRACE_HEADER='"my_trace.h"' ...
 *      storage.h will include that instead of this file.
 *
 *   2. Define the macros directly before including storage.h.
 *
 * Each macro is `#ifndef`-guarded, so defining any subset is fine; the rest fall
 * back to the no-op defaults here. To recover Xapiand's behavior, point these at
 * Xapiand's log.h macros (L_CALL/L_ERR/L_DEBUG/... from logger.h).
 */

#pragma once

#ifndef L_NOTHING
#define L_NOTHING(...)
#endif

#ifndef L_CALL
#define L_CALL L_NOTHING
#endif

#ifndef L_DEBUG
#define L_DEBUG L_NOTHING
#endif

#ifndef L_ERR
#define L_ERR L_NOTHING
#endif

#ifndef L_WARNING
#define L_WARNING L_NOTHING
#endif

#ifndef L_WARNING_ONCE
#define L_WARNING_ONCE L_NOTHING
#endif

#ifndef L_EXC
#define L_EXC L_NOTHING
#endif
