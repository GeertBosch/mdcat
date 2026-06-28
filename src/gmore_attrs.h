#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "gmore_types.h"

namespace gmore {

extern std::vector<Attr> gAttrs;
extern std::unordered_map<Attr, uint16_t, AttrHash> gAttrMap;
extern std::vector<std::string> gUris;
extern std::unordered_map<std::string, uint32_t> gUriMap;

uint16_t internAttr(const Attr& a);
uint32_t internUri(const std::string& uri);
std::string sgrFor(uint16_t id);

}  // namespace gmore
