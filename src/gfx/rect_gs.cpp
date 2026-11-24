// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Rectangle lists. The console draws a rectangle from three vertices and the hardware supplies
// the fourth corner; PC Direct3D 12 has no such primitive. A pipeline of that kind is created
// with triangles and the geometry shader made here, which takes each triangle the vertex shader
// produced and emits the whole rectangle as a four-vertex strip.
//
// The fourth vertex: of the three given, the corner is the one that shares its x with one of the
// other two and its y with the other (in clip space after the divide by w). The missing vertex
// lies opposite it: a + b - corner for the other two, a and b. That holds for the position and
// for every interpolated (float) output; integer outputs are the same on all vertices and are
// taken from the first.
//
// The shader is written against the output struct of the translated vertex shader (the text
// "struct VSOut {...};" translate.cpp emits), so its input and output signatures match what the
// pixel shader expects. Results are kept by a hash of that text.
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.h"
#include "xsl/log.h"

namespace pc {
namespace {
constexpr const char* CH = "gfx.rectgs";

struct Field {
    std::string name;
    bool interpolated;   // a float value without `nointerpolation`
};

// The fields of "struct VSOut {\n  <modifiers> <type> <name> : <SEMANTIC>;\n ... };".
bool parse_fields(const std::string& text, std::vector<Field>& out) {
    size_t at = text.find('{');
    const size_t end = text.rfind('}');
    if (at == std::string::npos || end == std::string::npos || end < at) return false;
    at++;
    while (at < end) {
        size_t semi = text.find(';', at);
        if (semi == std::string::npos || semi > end) break;
        std::string line = text.substr(at, semi - at);
        at = semi + 1;
        const size_t colon = line.find(':');
        if (colon != std::string::npos) line.resize(colon);
        // Words before the colon: modifiers, then the type, then the name.
        std::vector<std::string> words;
        for (size_t i = 0; i < line.size();) {
            while (i < line.size() && isspace((unsigned char)line[i])) i++;
            size_t j = i;
            while (j < line.size() && !isspace((unsigned char)line[j])) j++;
            if (j > i) words.push_back(line.substr(i, j - i));
            i = j;
        }
        if (words.empty()) continue;
        if (words.size() < 2) return false;
        Field f;
        f.name = words.back();
        f.interpolated = words[words.size() - 2].compare(0, 5, "float") == 0;
        for (size_t i = 0; i + 2 < words.size(); i++)
            if (words[i] == "nointerpolation") f.interpolated = false;
        out.push_back(std::move(f));
    }
    return !out.empty();
}

std::string gs_hlsl(const std::string& vsOut, const std::vector<Field>& fields) {
    std::string h = vsOut;
    h += "[maxvertexcount(4)]\n"
         "void main(triangle VSOut v[3], inout TriangleStream<VSOut> strip) {\n"
         "  float2 p[3];\n"
         "  for (uint i = 0; i < 3; i++) p[i] = v[i].pos_.xy / (v[i].pos_.w != 0.0f ? v[i].pos_.w : 1.0f);\n"
         "  // The corner shares its x with one of the other two vertices and its y with the other;\n"
         "  // the vertex that comes closest to that is taken, so small errors do not matter.\n"
         "  uint c = 0;\n"
         "  float best = 3.0e38f;\n"
         "  for (uint k = 0; k < 3; k++) {\n"
         "    float2 da = abs(p[k] - p[(k + 1) % 3]), db = abs(p[k] - p[(k + 2) % 3]);\n"
         "    float d = min(da.x + db.y, da.y + db.x);\n"
         "    if (d < best) { best = d; c = k; }\n"
         "  }\n"
         "  VSOut K = v[c], A = v[(c + 1) % 3], B = v[(c + 2) % 3];\n"
         "  VSOut D = v[0];   // the fourth vertex, opposite the corner\n";
    for (auto& f : fields) {
        if (f.interpolated) h += "  D." + f.name + " = A." + f.name + " + B." + f.name + " - K." + f.name + ";\n";
        else h += "  K." + f.name + " = A." + f.name + " = B." + f.name + " = v[0]." + f.name + ";\n";
    }
    // K, A, B keeps the winding of the triangle as given; the strip's second triangle is A, B, D.
    h += "  strip.Append(K);\n  strip.Append(A);\n  strip.Append(B);\n  strip.Append(D);\n}\n";
    return h;
}

uint64_t hash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
    return h;
}

std::mutex g_lock;
// Hash of the struct text -> the compiled shader (empty: it failed, and is not tried again).
// Entries are never removed, so pointers to their bytes stay valid.
std::map<uint64_t, std::vector<uint8_t>> g_done;
}  // namespace

bool rect_list_gs_from_struct(const std::string& vsOutStruct, const void** gsDxil, size_t* gsSize) {
    const uint64_t key = hash(vsOutStruct);
    std::lock_guard<std::mutex> g(g_lock);   // one at a time: the same struct is not compiled twice
    auto f = g_done.find(key);
    if (f == g_done.end()) {
        std::vector<uint8_t> dxil;
        std::vector<Field> fields;
        bool hasPos = false;
        if (parse_fields(vsOutStruct, fields))
            for (auto& fl : fields) hasPos |= fl.name == "pos_";
        if (!hasPos) {
            XSL_WARN(CH, "rectangle list: vertex output struct not understood:\n%s", vsOutStruct.c_str());
        } else {
            std::string log;
            const std::string hlsl = gs_hlsl(vsOutStruct, fields);
            if (!compile_layer_shader_cached(hlsl, "gs_6_0", dxil, log)) {
                XSL_ERR(CH, "rectangle-list geometry shader does not compile:\n%s\n%s", log.c_str(), hlsl.c_str());
                dxil.clear();
            } else {
                XSL_INFO(CH, "rectangle-list geometry shader %016llx: %zu fields, %zu bytes", (unsigned long long)key, fields.size(), dxil.size());
            }
        }
        f = g_done.emplace(key, std::move(dxil)).first;
    }
    if (f->second.empty()) return false;
    *gsDxil = f->second.data();
    *gsSize = f->second.size();
    return true;
}

bool rect_list_gs(const void* vs, size_t vsSize, const void** gsDxil, size_t* gsSize) {
    std::string text;
    if (!vs || !vsSize || !vs_output_struct(vs, vsSize, &text)) {
        static volatile long shown = 0;
        if (++shown <= 5) XSL_WARN(CH, "rectangle list: no translated vertex shader to take the outputs from (half of each rectangle is missing)");
        return false;
    }
    return rect_list_gs_from_struct(text, gsDxil, gsSize);
}
}  // namespace pc
