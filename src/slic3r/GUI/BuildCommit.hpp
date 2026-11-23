#pragma once

#include "libslic3r_version.h"

namespace Slic3r { namespace GUI {
// Orca 2.5.2 plugin system: the build label plugins can read (orca.host.app).
inline const char *const build_commit_label = SLIC3R_APP_NAME " " SLIC3R_VERSION;
}} // namespace Slic3r::GUI
