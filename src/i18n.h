#pragma once

// RFC 0003: gettext binding for the GTK UI chrome. In NLS-enabled builds the
// _() macro resolves through gettext; in plain builds (msgfmt missing) it is
// the identity, so the sources compile either way and English is always the
// fallback. The domain is bound once in main.cpp.

#ifdef ENABLE_NLS
#include <libintl.h>
#define _(String) gettext(String)
#else
#define _(String) (String)
#endif
