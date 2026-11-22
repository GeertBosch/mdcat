#include "mdcat_state.h"

#include "mdcat_style.h"

namespace mdcat {

// Mutable theme colours (declared in mdcat_style.h); initTheme overrides them for a dark theme.
std::string kCodeOn =
    "\033[48;5;254;38;5;236m";  // light-gray bg, dark-gray fg (light-theme default)
std::string kLightGray = "\033[38;5;250m";

int gWidthOverride = 0;
bool gForceGraphics = false;
int gForcedBackend = -1;  // -1 = unset; else a GraphicsBackend value
MermaidMode gMermaidMode = MermaidMode::Auto;
std::string gFileDir;
std::string gSourceName;
size_t gBlockLine = 0;
std::string gOutputFile;
int gIndent = 0;
int gListDepth = 0;
int gListNesting = 0;
bool gTightItem = false;
int gRenderWidth = 0;
bool gEmitProvenance = false;

}  // namespace mdcat
