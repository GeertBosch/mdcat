#include "mdcat_state.h"

namespace mdcat {

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
