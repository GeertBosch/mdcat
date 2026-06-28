#include "gmore_attrs.h"

namespace gmore {

std::vector<Attr> gAttrs;
std::unordered_map<Attr, uint16_t, AttrHash> gAttrMap;
std::vector<std::string> gUris;
std::unordered_map<std::string, uint32_t> gUriMap;

uint16_t internAttr(const Attr& a) {
    auto it = gAttrMap.find(a);
    if (it != gAttrMap.end()) return it->second;
    uint16_t id = static_cast<uint16_t>(gAttrs.size());
    gAttrs.push_back(a);
    gAttrMap.emplace(a, id);
    return id;
}

uint32_t internUri(const std::string& uri) {
    if (uri.empty()) return 0;
    auto it = gUriMap.find(uri);
    if (it != gUriMap.end()) return it->second;
    if (gUris.empty()) gUris.emplace_back();  // reserve slot 0
    uint32_t id = static_cast<uint32_t>(gUris.size());
    gUris.push_back(uri);
    gUriMap.emplace(uri, id);
    return id;
}

std::string sgrFor(uint16_t id) {
    const Attr& a = gAttrs[id];
    std::string s = "\033[0";
    if (a.flags & A_BOLD) s += ";1";
    if (a.flags & A_DIM) s += ";2";
    if (a.flags & A_ITALIC) s += ";3";
    if (a.flags & A_UNDER) s += ";4";
    if (a.flags & A_INVERSE) s += ";7";
    auto color = [&](uint32_t c, const char* p38) {
        if (!c) return;
        if ((c & 0xFF000000u) == PAL) {
            s += ";";
            s += p38;
            s += ";5;";
            s += std::to_string(c & 0xFF);
        } else {
            int v = c & 0xFFFFFF;
            s += ";";
            s += p38;
            s += ";2;";
            s += std::to_string((v >> 16) & 0xFF);
            s += ";";
            s += std::to_string((v >> 8) & 0xFF);
            s += ";";
            s += std::to_string(v & 0xFF);
        }
    };
    color(a.fg, "38");
    color(a.bg, "48");
    s += "m";
    return s;
}

}  // namespace gmore
