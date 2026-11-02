// LitePainter - Tahap 6: bake multi-map, layer mask, smart material, drag & drop layer
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

// ================================================================ shader
static const char* kMeshVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
out vec3 vP; out vec3 vN; out vec2 vUV;
void main(){ vP = aPos; vN = aNrm; vUV = aUV; gl_Position = uMVP * vec4(aPos,1.0); }
)";

// uView: 0 Base Color, 1 PBR, 2 Roughness, 3 Metallic, 4 Bake map
static const char* kMeshFS = R"(#version 330 core
in vec3 vP; in vec3 vN; in vec2 vUV;
uniform vec3 uEye; uniform vec3 uLight;
uniform sampler2D uTex; uniform sampler2D uTexP;
uniform sampler2D uTexAO; uniform sampler2D uTexBake;
uniform int uView; uniform int uUseAO;
out vec4 frag;
const float PI = 3.14159265;

vec3 envMap(vec3 d, float r){
  float y = clamp(d.y, -1.0, 1.0);
  vec3 hor = vec3(0.80, 0.82, 0.86);
  vec3 top = vec3(0.35, 0.55, 0.95);
  vec3 bot = vec3(0.10, 0.09, 0.08);
  vec3 c = (y > 0.0) ? mix(hor, top, sqrt(y)) : mix(hor, bot, sqrt(-y));
  float s = pow(max(dot(d, normalize(vec3(0.4, 0.8, 0.3))), 0.0), mix(400.0, 6.0, r));
  c += vec3(1.0, 0.93, 0.8) * s * mix(4.0, 0.4, r);
  return mix(c, vec3(0.42, 0.45, 0.5), r * r);
}

void main(){
  if (uView == 4) { frag = vec4(texture(uTexBake, vUV).rgb, 1.0); return; }

  vec3 n = normalize(vN);
  vec3 v = normalize(uEye - vP);
  if (dot(n, v) < 0.0) n = -n;
  vec3 base = texture(uTex, vUV).rgb;
  vec2 rm = texture(uTexP, vUV).rg;
  float ao = (uUseAO == 1) ? texture(uTexAO, vUV).r : 1.0;

  if (uView == 0) {
    float d = max(dot(n, v), 0.0);
    float sky = 0.5 + 0.5 * n.y;
    frag = vec4(base * (0.3 + 0.5 * d + 0.2 * sky) * ao, 1.0);
    return;
  }
  if (uView == 2 || uView == 3) {
    float val = (uView == 2) ? rm.x : rm.y;
    float d = max(dot(n, v), 0.0);
    frag = vec4(vec3(val) * (0.35 + 0.65 * d), 1.0);
    return;
  }

  vec3 albedo = pow(base, vec3(2.2));
  float rough = clamp(rm.x, 0.05, 1.0);
  float metal = clamp(rm.y, 0.0, 1.0);
  vec3 F0 = mix(vec3(0.04), albedo, metal);
  float NdV = max(dot(n, v), 1e-3);
  vec3 l = normalize(uLight);
  vec3 hv = normalize(l + v);
  float NdL = max(dot(n, l), 0.0);
  float NdH = max(dot(n, hv), 0.0);
  float VdH = max(dot(v, hv), 0.0);
  float a = rough * rough; float a2 = a * a;
  float dd = NdH * NdH * (a2 - 1.0) + 1.0;
  float D = a2 / (PI * dd * dd);
  float k = (rough + 1.0) * (rough + 1.0) / 8.0;
  float G = (NdL / (NdL * (1.0 - k) + k)) * (NdV / (NdV * (1.0 - k) + k));
  vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0);
  vec3 spec = D * G * F / (4.0 * NdL * NdV + 1e-4);
  vec3 kd = (1.0 - F) * (1.0 - metal);
  vec3 direct = (kd * albedo / PI + spec) * NdL * 3.0;

  vec3 rv = reflect(-v, n);
  vec3 Fa = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdV, 5.0);
  vec3 amb = (1.0 - Fa) * (1.0 - metal) * albedo * envMap(n, 1.0) * 0.8 + envMap(rv, rough) * Fa;

  vec3 col = direct * mix(1.0, ao, 0.5) + amb * ao;
  col = col / (1.0 + col);
  frag = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}
)";

static const char* kLineVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP; uniform vec3 uOrigin; uniform float uScale;
void main(){ gl_Position = uMVP * vec4(uOrigin + vec3(aPos.x, 0.0, aPos.z) * uScale, 1.0); }
)";
static const char* kLineFS = R"(#version 330 core
uniform vec3 uColor; out vec4 frag;
void main(){ frag = vec4(uColor,1.0); }
)";

static const char* kPaintVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
out vec3 vP; out vec3 vN; out vec2 vUV;
void main(){ vP = aPos; vN = aNrm; vUV = aUV; gl_Position = vec4(aUV * 2.0 - 1.0, 0.0, 1.0); }
)";
static const char* kPaintFS = R"(#version 330 core
in vec3 vP; in vec3 vN; in vec2 vUV;
uniform vec3 uHit; uniform vec3 uHitN; uniform vec3 uColor;
uniform float uRadius; uniform float uHardness; uniform float uOpacity;
uniform float uRough; uniform float uMetal;
uniform int uFill;                       // 1 = isi penuh area (polygon fill), 0 = brush bulat
uniform sampler2D uMatC; uniform sampler2D uMatR; uniform sampler2D uMatM;
uniform int uHasC; uniform int uHasR; uniform int uHasM; uniform float uTile;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outPbr;
void main(){
  float a = uOpacity;
  if (uFill == 0) {
    float d = distance(vP, uHit) / uRadius;
    if (d >= 1.0) discard;
    if (dot(normalize(vN), uHitN) < 0.1) discard;
    a = (1.0 - smoothstep(uHardness, 1.0, d)) * uOpacity;
  }
  vec2 tuv = vUV * uTile;
  vec3 col = uColor; float rough = uRough; float metal = uMetal;
  if (uHasC == 1) col = texture(uMatC, tuv).rgb;
  if (uHasR == 1) rough = texture(uMatR, tuv).r;
  if (uHasM == 1) metal = texture(uMatM, tuv).r;
  outColor = vec4(col * a, a);
  outPbr = vec4(rough * a, a, metal * a, a);
}
)";
// Cat ke mask (1 kanal): putih = tampil, hitam = sembunyi
static const char* kPaintMaskFS = R"(#version 330 core
in vec3 vP; in vec3 vN;
uniform vec3 uHit; uniform vec3 uHitN;
uniform float uRadius; uniform float uHardness; uniform float uOpacity; uniform float uValue;
uniform int uFill;
out vec4 frag;
void main(){
  float a = uOpacity;
  if (uFill == 0) {
    float d = distance(vP, uHit) / uRadius;
    if (d >= 1.0) discard;
    if (dot(normalize(vN), uHitN) < 0.1) discard;
    a = (1.0 - smoothstep(uHardness, 1.0, d)) * uOpacity;
  }
  frag = vec4(vec3(uValue), a);
}
)";
static const char* kMaskFS = R"(#version 330 core
out vec4 frag;
void main(){ frag = vec4(1.0); }
)";
static const char* kGBufFS = R"(#version 330 core
in vec3 vP; in vec3 vN;
layout(location=0) out vec4 outPos;
layout(location=1) out vec4 outNrm;
void main(){ outPos = vec4(vP, 1.0); outNrm = vec4(normalize(vN), 0.0); }
)";
static const char* kFullVS = R"(#version 330 core
void main(){
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
static const char* kCompFS = R"(#version 330 core
uniform sampler2D uDst; uniform sampler2D uLayer;
uniform sampler2D uDstP; uniform sampler2D uLayerP;
uniform sampler2D uMask; uniform int uHasMask;
uniform float uOpacity; uniform int uMode;
layout(location=0) out vec4 outC;
layout(location=1) out vec4 outP;
vec3 blendFn(vec3 b, vec3 s){
  if (uMode == 1) return b * s;
  if (uMode == 2) return 1.0 - (1.0 - b) * (1.0 - s);
  if (uMode == 3) return mix(2.0 * b * s, 1.0 - 2.0 * (1.0 - b) * (1.0 - s), step(0.5, b));
  if (uMode == 4) return min(b + s, vec3(1.0));
  return s;
}
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  float m = (uHasMask == 1) ? texelFetch(uMask, c, 0).r : 1.0;
  vec4 dst = texelFetch(uDst, c, 0);
  vec4 L = texelFetch(uLayer, c, 0);
  float a = L.a * uOpacity * m;
  vec3 s = (L.a > 0.0001) ? (L.rgb / L.a) : vec3(0.0);
  outC = vec4(mix(dst.rgb, blendFn(dst.rgb, s), a), 1.0);

  vec4 dp = texelFetch(uDstP, c, 0);
  vec4 P = texelFetch(uLayerP, c, 0);
  float ra = P.g * uOpacity * m;
  float rv = (P.g > 0.0001) ? (P.r / P.g) : 0.0;
  float ma = P.a * uOpacity * m;
  float mv = (P.a > 0.0001) ? (P.b / P.a) : 0.0;
  outP = vec4(mix(dp.r, rv, ra), mix(dp.g, mv, ma), 0.0, 1.0);
}
)";
static const char* kDilFS = R"(#version 330 core
uniform sampler2D uSrc; uniform sampler2D uMask; uniform sampler2D uSrcP; uniform int uStage;
layout(location=0) out vec4 outC;
layout(location=1) out vec4 outP;
void main(){
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec4 s = texelFetch(uSrc, c, 0);
  vec4 sp = texelFetch(uSrcP, c, 0);
  if (uStage == 0) {
    float m = texelFetch(uMask, c, 0).r > 0.5 ? 1.0 : 0.0;
    outC = vec4(s.rgb, m);
    outP = vec4(sp.rg, 0.0, m);
    return;
  }
  if (s.a > 0.5) { outC = s; outP = sp; return; }
  ivec2 sz = textureSize(uSrc, 0);
  vec3 acc = vec3(0.0); vec2 accp = vec2(0.0); float n = 0.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      ivec2 p = clamp(c + ivec2(x, y), ivec2(0), sz - ivec2(1));
      vec4 t = texelFetch(uSrc, p, 0);
      if (t.a > 0.5) { acc += t.rgb; accp += texelFetch(uSrcP, p, 0).rg; n += 1.0; }
    }
  }
  if (n > 0.0) { outC = vec4(acc / n, 1.0); outP = vec4(accp / n, 0.0, 1.0); }
  else { outC = s; outP = sp; }
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log); std::fprintf(stderr, "Shader error: %s\n", log); }
    return s;
}
static GLuint makeProgram(const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log); std::fprintf(stderr, "Link error: %s\n", log); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// ================================================================ mesh
struct Vertex { glm::vec3 p; glm::vec3 n; glm::vec2 uv; };

struct Mesh {
    GLuint vao = 0, vbo = 0;
    int count = 0, triangles = 0;
    bool hasUV = false;
    glm::vec3 bmin{0}, bmax{0};
    std::vector<Vertex> cpu;
    std::vector<int> poly;   // id polygon asli (sebelum triangulasi) per segitiga

    void destroy() {
        if (vbo) glDeleteBuffers(1, &vbo);
        if (vao) glDeleteVertexArrays(1, &vao);
        vao = vbo = 0; count = 0;
    }
    void upload(const std::vector<Vertex>& v) {
        destroy();
        cpu = v;
        count = (int)v.size();
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Vertex), v.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p));
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n));
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
        glBindVertexArray(0);
    }
    void draw() const { if (!vao) return; glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES, 0, count); glBindVertexArray(0); }
};

// Baca OBJ -> daftar segitiga (non-indexed). polyOut (opsional) = id polygon asli tiap segitiga
static bool parseOBJ(const std::string& path, std::vector<Vertex>& verts, glm::vec3& bmin, glm::vec3& bmax,
                     bool& hasUV, std::string& err, std::vector<int>* polyOut = nullptr) {
    tinyobj::ObjReaderConfig cfg;
    cfg.triangulate = false;      // triangulasi manual (fan) supaya id polygon asli tetap diketahui
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg)) { err = "Gagal load: " + reader.Error(); return false; }

    const auto& a = reader.GetAttrib();
    verts.clear();
    if (polyOut) polyOut->clear();
    bmin = glm::vec3(1e30f); bmax = glm::vec3(-1e30f);

    auto makeVertex = [&](const tinyobj::index_t& id) {
        Vertex v;
        v.p = { a.vertices[3 * id.vertex_index], a.vertices[3 * id.vertex_index + 1], a.vertices[3 * id.vertex_index + 2] };
        v.n = glm::vec3(0);
        if (id.normal_index >= 0)
            v.n = { a.normals[3 * id.normal_index], a.normals[3 * id.normal_index + 1], a.normals[3 * id.normal_index + 2] };
        v.uv = glm::vec2(0);
        if (id.texcoord_index >= 0)
            v.uv = { a.texcoords[2 * id.texcoord_index], a.texcoords[2 * id.texcoord_index + 1] };
        bmin = glm::min(bmin, v.p); bmax = glm::max(bmax, v.p);
        return v;
    };

    int faceIdx = 0;
    for (const auto& shape : reader.GetShapes()) {
        const auto& idx = shape.mesh.indices;
        const auto& nfv = shape.mesh.num_face_vertices;
        size_t off = 0;
        for (size_t f = 0; f < nfv.size(); f++) {
            int nv = (int)nfv[f];
            if (nv >= 3) {
                for (int k = 1; k + 1 < nv; k++) {
                    Vertex tri[3] = { makeVertex(idx[off]), makeVertex(idx[off + k]), makeVertex(idx[off + k + 1]) };
                    glm::vec3 fn = glm::cross(tri[1].p - tri[0].p, tri[2].p - tri[0].p);
                    if (glm::length(fn) > 0) fn = glm::normalize(fn);
                    for (int j = 0; j < 3; j++) {
                        if (glm::length(tri[j].n) < 1e-6f) tri[j].n = fn;
                        verts.push_back(tri[j]);
                    }
                    if (polyOut) polyOut->push_back(faceIdx);
                }
            }
            off += (size_t)nv;
            faceIdx++;
        }
    }
    if (verts.empty()) { err = "File OBJ kosong / tidak ada segitiga."; return false; }
    hasUV = !a.texcoords.empty();
    return true;
}

static bool loadOBJ(const std::string& path, Mesh& mesh, std::string& msg) {
    std::vector<Vertex> verts; glm::vec3 bmin, bmax; bool hasUV = false;
    if (!parseOBJ(path, verts, bmin, bmax, hasUV, msg, &mesh.poly)) return false;
    mesh.upload(verts);
    mesh.bmin = bmin; mesh.bmax = bmax;
    mesh.triangles = (int)verts.size() / 3;
    mesh.hasUV = hasUV;
    msg = "Loaded: " + path;
    if (!mesh.hasUV) msg += " | PERINGATAN: tidak ada UV, painting nonaktif.";
    return true;
}

// ================================================================ hasil pick (klik di model)
struct Hit { bool ok = false; float t = 1e30f; int tri = -1; glm::vec3 p{0}, n{0, 1, 0}; };

// ================================================================ BVH (untuk baking)
struct BVH {
    struct Node { glm::vec3 bmin; int left; glm::vec3 bmax; int count; };   // count>0 = daun (left = indeks pertama)
    std::vector<Node> nodes;
    std::vector<glm::vec3> v0, e1, e2;
    std::vector<int> order;       // urutan BVH -> indeks segitiga asli
    std::vector<glm::vec3> n0, n1, n2;   // normal vertex (opsional, untuk high-poly)

    void build(const std::vector<Vertex>& verts, bool keepNormals = false) {
        int nt = (int)(verts.size() / 3);
        std::vector<int> idx(nt);
        std::vector<glm::vec3> cen(nt), tmin(nt), tmax(nt);
        for (int i = 0; i < nt; i++) {
            idx[i] = i;
            const glm::vec3& a = verts[3 * i].p; const glm::vec3& b = verts[3 * i + 1].p; const glm::vec3& c = verts[3 * i + 2].p;
            cen[i] = (a + b + c) / 3.0f;
            tmin[i] = glm::min(a, glm::min(b, c));
            tmax[i] = glm::max(a, glm::max(b, c));
        }
        nodes.clear(); nodes.reserve((size_t)nt * 2 + 1);
        nodes.push_back(Node());
        buildRec(0, 0, nt, idx, cen, tmin, tmax);
        order = idx;
        v0.resize(nt); e1.resize(nt); e2.resize(nt);
        if (keepNormals) { n0.resize(nt); n1.resize(nt); n2.resize(nt); }
        for (int i = 0; i < nt; i++) {
            int t = idx[i];
            v0[i] = verts[3 * t].p;
            e1[i] = verts[3 * t + 1].p - v0[i];
            e2[i] = verts[3 * t + 2].p - v0[i];
            if (keepNormals) { n0[i] = verts[3 * t].n; n1[i] = verts[3 * t + 1].n; n2[i] = verts[3 * t + 2].n; }
        }
    }
    void buildRec(int ni, int b, int e, std::vector<int>& idx, const std::vector<glm::vec3>& cen,
                  const std::vector<glm::vec3>& tmin, const std::vector<glm::vec3>& tmax) {
        glm::vec3 bmin(1e30f), bmax(-1e30f), cmin(1e30f), cmax(-1e30f);
        for (int i = b; i < e; i++) {
            int t = idx[i];
            bmin = glm::min(bmin, tmin[t]); bmax = glm::max(bmax, tmax[t]);
            cmin = glm::min(cmin, cen[t]); cmax = glm::max(cmax, cen[t]);
        }
        nodes[ni].bmin = bmin; nodes[ni].bmax = bmax;
        int cnt = e - b;
        if (cnt <= 4) { nodes[ni].left = b; nodes[ni].count = cnt; return; }
        glm::vec3 ext = cmax - cmin;
        int axis = (ext.x > ext.y) ? ((ext.x > ext.z) ? 0 : 2) : ((ext.y > ext.z) ? 1 : 2);
        int mid = (b + e) / 2;
        std::nth_element(idx.begin() + b, idx.begin() + mid, idx.begin() + e,
                         [&](int x, int y) { return cen[x][axis] < cen[y][axis]; });
        int l = (int)nodes.size();
        nodes.push_back(Node()); nodes.push_back(Node());
        nodes[ni].left = l; nodes[ni].count = 0;
        buildRec(l, b, mid, idx, cen, tmin, tmax);
        buildRec(l + 1, mid, e, idx, cen, tmin, tmax);
    }
    static glm::vec3 invDir(const glm::vec3& d) {
        return glm::vec3(1.0f / (std::fabs(d.x) < 1e-9f ? 1e-9f : d.x),
                         1.0f / (std::fabs(d.y) < 1e-9f ? 1e-9f : d.y),
                         1.0f / (std::fabs(d.z) < 1e-9f ? 1e-9f : d.z));
    }
    // true jika ada perpotongan apa pun (untuk AO)
    bool anyHit(const glm::vec3& o, const glm::vec3& d, float tmax) const {
        glm::vec3 inv = invDir(d);
        int stack[64]; int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& n = nodes[stack[--sp]];
            glm::vec3 t0 = (n.bmin - o) * inv, t1 = (n.bmax - o) * inv;
            glm::vec3 tn = glm::min(t0, t1), tf = glm::max(t0, t1);
            float tnear = std::max(std::max(tn.x, tn.y), std::max(tn.z, 0.0f));
            float tfar = std::min(std::min(tf.x, tf.y), std::min(tf.z, tmax));
            if (tnear > tfar) continue;
            if (n.count > 0) {
                for (int i = n.left; i < n.left + n.count; i++) {
                    glm::vec3 pv = glm::cross(d, e2[i]);
                    float det = glm::dot(e1[i], pv);
                    if (std::fabs(det) < 1e-12f) continue;
                    float idet = 1.0f / det;
                    glm::vec3 tv = o - v0[i];
                    float u = glm::dot(tv, pv) * idet;
                    if (u < 0.0f || u > 1.0f) continue;
                    glm::vec3 qv = glm::cross(tv, e1[i]);
                    float w = glm::dot(d, qv) * idet;
                    if (w < 0.0f || u + w > 1.0f) continue;
                    float t = glm::dot(e2[i], qv) * idet;
                    if (t > 1e-6f && t < tmax) return true;
                }
            } else {
                stack[sp++] = n.left; stack[sp++] = n.left + 1;
            }
        }
        return false;
    }
    // perpotongan terdekat (untuk thickness, normal & height map)
    bool closestHit(const glm::vec3& o, const glm::vec3& d, float tmax, float& tHit, int& tri, float& bu, float& bw) const {
        glm::vec3 inv = invDir(d);
        bool found = false;
        float best = tmax;
        int stack[64]; int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& n = nodes[stack[--sp]];
            glm::vec3 t0 = (n.bmin - o) * inv, t1 = (n.bmax - o) * inv;
            glm::vec3 tn = glm::min(t0, t1), tf = glm::max(t0, t1);
            float tnear = std::max(std::max(tn.x, tn.y), std::max(tn.z, 0.0f));
            float tfar = std::min(std::min(tf.x, tf.y), std::min(tf.z, best));
            if (tnear > tfar) continue;
            if (n.count > 0) {
                for (int i = n.left; i < n.left + n.count; i++) {
                    glm::vec3 pv = glm::cross(d, e2[i]);
                    float det = glm::dot(e1[i], pv);
                    if (std::fabs(det) < 1e-12f) continue;
                    float idet = 1.0f / det;
                    glm::vec3 tv = o - v0[i];
                    float u = glm::dot(tv, pv) * idet;
                    if (u < 0.0f || u > 1.0f) continue;
                    glm::vec3 qv = glm::cross(tv, e1[i]);
                    float w = glm::dot(d, qv) * idet;
                    if (w < 0.0f || u + w > 1.0f) continue;
                    float t = glm::dot(e2[i], qv) * idet;
                    if (t > 1e-6f && t < best) { best = t; tri = i; bu = u; bw = w; found = true; }
                }
            } else {
                stack[sp++] = n.left; stack[sp++] = n.left + 1;
            }
        }
        tHit = best;
        return found;
    }
};

// pick cepat memakai BVH (butuh build(..., true))
static Hit pickBVH(const BVH& b, const glm::vec3& o, const glm::vec3& d) {
    Hit h;
    if (b.nodes.empty() || b.n0.empty()) return h;
    float t = 0.0f, u = 0.0f, w = 0.0f; int tri = 0;
    if (b.closestHit(o, d, 1e30f, t, tri, u, w)) {
        h.ok = true; h.t = t; h.p = o + d * t;
        glm::vec3 n = (1.0f - u - w) * b.n0[tri] + u * b.n1[tri] + w * b.n2[tri];
        h.n = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 1, 0);
        h.tri = (tri >= 0 && tri < (int)b.order.size()) ? b.order[tri] : -1;
    }
    return h;
}

// ================================================================ topologi mesh (untuk Polygon Fill)
struct Topo {
    bool ready = false;
    std::vector<int> island, shell;          // id UV island, id mesh shell per segitiga
    std::vector<glm::vec3> fn;               // normal bidang per segitiga
    std::vector<std::vector<int>> adj;       // tetangga (berbagi sisi) per segitiga
};
struct IKey3Hash {
    size_t operator()(const std::array<int, 3>& a) const {
        return ((size_t)(unsigned)a[0] * 73856093u) ^ ((size_t)(unsigned)a[1] * 19349663u) ^ ((size_t)(unsigned)a[2] * 83492791u);
    }
};
static int dsuFind(std::vector<int>& p, int x) {
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}
static void buildTopo(const Mesh& m, Topo& T) {
    T = Topo();
    const auto& v = m.cpu;
    int nt = (int)(v.size() / 3);
    if (nt == 0) return;
    float diag = glm::length(m.bmax - m.bmin);
    float cell = std::max(diag * 1e-5f, 1e-9f);

    // weld posisi
    std::unordered_map<std::array<int, 3>, int, IKey3Hash> posMap;
    posMap.reserve((size_t)nt * 2);
    std::vector<int> pid(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        std::array<int, 3> k = {(int)std::floor(v[i].p.x / cell), (int)std::floor(v[i].p.y / cell), (int)std::floor(v[i].p.z / cell)};
        auto it = posMap.find(k);
        if (it == posMap.end()) { int id = (int)posMap.size(); posMap.emplace(k, id); pid[i] = id; }
        else pid[i] = it->second;
    }

    // normal bidang + daftar sisi
    T.fn.resize(nt);
    T.adj.assign(nt, std::vector<int>());
    std::vector<std::pair<unsigned long long, int>> ed;
    ed.reserve((size_t)nt * 3);
    for (int t = 0; t < nt; t++) {
        glm::vec3 f = glm::cross(v[3 * t + 1].p - v[3 * t].p, v[3 * t + 2].p - v[3 * t].p);
        T.fn[t] = glm::length(f) > 0.0f ? glm::normalize(f) : glm::vec3(0, 1, 0);
        int id[3] = {pid[3 * t], pid[3 * t + 1], pid[3 * t + 2]};
        for (int k = 0; k < 3; k++) {
            int x = id[k], y = id[(k + 1) % 3];
            if (x == y) continue;
            unsigned long long lo = (unsigned long long)std::min(x, y), hi = (unsigned long long)std::max(x, y);
            ed.emplace_back((lo << 32) | hi, t);
        }
    }
    std::sort(ed.begin(), ed.end());
    for (size_t i = 0; i < ed.size();) {
        size_t j = i;
        while (j < ed.size() && ed[j].first == ed[i].first) j++;
        if (j - i >= 2 && j - i <= 8)
            for (size_t a = i; a < j; a++)
                for (size_t b = i; b < j; b++)
                    if (a != b && ed[a].second != ed[b].second) T.adj[ed[a].second].push_back(ed[b].second);
        i = j;
    }

    // mesh shell (terhubung lewat sisi)
    T.shell.assign(nt, -1);
    std::vector<int> q;
    int sid = 0;
    for (int t = 0; t < nt; t++) {
        if (T.shell[t] >= 0) continue;
        q.clear(); q.push_back(t); T.shell[t] = sid;
        for (size_t h = 0; h < q.size(); h++) {
            int c = q[h];
            for (int nb : T.adj[c]) if (T.shell[nb] < 0) { T.shell[nb] = sid; q.push_back(nb); }
        }
        sid++;
    }

    // UV island (union-find lewat titik UV yang sama)
    std::vector<int> par(nt);
    std::iota(par.begin(), par.end(), 0);
    std::unordered_map<std::array<int, 3>, int, IKey3Hash> uvMap;
    uvMap.reserve((size_t)nt * 2);
    for (int t = 0; t < nt; t++) {
        for (int k = 0; k < 3; k++) {
            const glm::vec2& uv = v[3 * t + k].uv;
            std::array<int, 3> key = {(int)std::lround(uv.x * 20000.0f), (int)std::lround(uv.y * 20000.0f), 0};
            auto it = uvMap.find(key);
            if (it == uvMap.end()) uvMap.emplace(key, t);
            else {
                int a = dsuFind(par, t), b = dsuFind(par, it->second);
                if (a != b) par[a] = b;
            }
        }
    }
    T.island.resize(nt);
    for (int t = 0; t < nt; t++) T.island[t] = dsuFind(par, t);
    T.ready = true;
}

// mode: 0 segitiga, 1 polygon, 2 UV island, 3 mesh shell, 4 area sudut
static void selectTris(const Mesh& m, const Topo& T, int mode, int tri, float angleDeg, std::vector<int>& out) {
    out.clear();
    int nt = (int)(m.cpu.size() / 3);
    if (tri < 0 || tri >= nt) return;
    if (mode == 0 || !T.ready) { out.push_back(tri); return; }
    if (mode == 1) {
        if (m.poly.size() != (size_t)nt) { out.push_back(tri); return; }
        int id = m.poly[tri];
        for (int t = 0; t < nt; t++) if (m.poly[t] == id) out.push_back(t);
    } else if (mode == 2) {
        int id = T.island[tri];
        for (int t = 0; t < nt; t++) if (T.island[t] == id) out.push_back(t);
    } else if (mode == 3) {
        int id = T.shell[tri];
        for (int t = 0; t < nt; t++) if (T.shell[t] == id) out.push_back(t);
    } else {
        float cosT = std::cos(glm::radians(angleDeg));
        std::vector<char> seen((size_t)nt, 0);
        out.push_back(tri); seen[tri] = 1;
        for (size_t h = 0; h < out.size(); h++) {
            int c = out[h];
            for (int nb : T.adj[c]) {
                if (seen[nb]) continue;
                if (glm::dot(T.fn[c], T.fn[nb]) >= cosT) { seen[nb] = 1; out.push_back(nb); }
            }
        }
    }
}

// ================================================================ GL helpers
static GLuint makeTex(int sz, GLint internalFmt, GLenum fmt, bool mips) {
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, sz, sz, 0, fmt, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mips ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mips ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (!mips) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}
static GLuint makeFbo(GLuint tex) {
    GLuint f; glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return f;
}
static GLuint makeFbo2(GLuint t0, GLuint t1) {
    GLuint f; glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t1, 0);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return f;
}

// ================================================================ layers + canvas
struct Layer {
    int id = 0;
    std::string name;
    GLuint tex = 0, texP = 0, fbo = 0;       // color (premultiplied) + (rough, metal) + MRT fbo
    GLuint maskTex = 0, maskFbo = 0;         // mask R8 (putih = tampil)
    bool hasMask = false, maskOn = true;
    float opacity = 1.0f;
    int blend = 0;
    bool visible = true;
};
struct UndoEntry { int layerId = 0; bool mask = false; std::vector<unsigned char> px, pxP, pxM; };

struct Canvas {
    static constexpr size_t kMaxLayers = 8;
    int size = 0, active = 0, nextId = 1, cur = 0;
    bool dirty = false, maskValid = false;
    std::vector<Layer> layers;
    GLuint compC[2] = {0, 0}, compP[2] = {0, 0}, compFbo[2] = {0, 0};
    GLuint dispC = 0, dispP = 0, dispCFbo = 0, dispPFbo = 0;
    GLuint mask = 0, maskFbo = 0;            // mask area UV
    GLuint dummyMask = 0;                    // 1x1 putih
    GLuint compProg = 0, dilProg = 0, maskProg = 0, emptyVao = 0;
    std::vector<UndoEntry> undoS, redoS;

    size_t maxStates() const { return size >= 2048 ? 8 : (size >= 1024 ? 20 : 40); }

    void freeLayer(Layer& L) {
        if (L.fbo) glDeleteFramebuffers(1, &L.fbo);
        if (L.tex) glDeleteTextures(1, &L.tex);
        if (L.texP) glDeleteTextures(1, &L.texP);
        if (L.maskFbo) glDeleteFramebuffers(1, &L.maskFbo);
        if (L.maskTex) glDeleteTextures(1, &L.maskTex);
        L.fbo = L.tex = L.texP = L.maskFbo = L.maskTex = 0;
        L.hasMask = false;
    }
    void destroy() {
        for (auto& L : layers) freeLayer(L);
        layers.clear();
        for (int i = 0; i < 2; i++) {
            if (compFbo[i]) glDeleteFramebuffers(1, &compFbo[i]);
            if (compC[i]) glDeleteTextures(1, &compC[i]);
            if (compP[i]) glDeleteTextures(1, &compP[i]);
            compFbo[i] = compC[i] = compP[i] = 0;
        }
        if (dispCFbo) glDeleteFramebuffers(1, &dispCFbo);
        if (dispPFbo) glDeleteFramebuffers(1, &dispPFbo);
        if (dispC) glDeleteTextures(1, &dispC);
        if (dispP) glDeleteTextures(1, &dispP);
        if (maskFbo) glDeleteFramebuffers(1, &maskFbo);
        if (mask) glDeleteTextures(1, &mask);
        if (dummyMask) glDeleteTextures(1, &dummyMask);
        dispCFbo = dispPFbo = dispC = dispP = maskFbo = mask = dummyMask = 0;
        undoS.clear(); redoS.clear();
        maskValid = false;
    }
    void create(int sz) {
        destroy();
        size = sz; active = 0;
        for (int i = 0; i < 2; i++) {
            compC[i] = makeTex(sz, GL_RGBA8, GL_RGBA, false);
            compP[i] = makeTex(sz, GL_RGBA8, GL_RGBA, false);
            compFbo[i] = makeFbo2(compC[i], compP[i]);
        }
        dispC = makeTex(sz, GL_RGBA8, GL_RGBA, true); dispCFbo = makeFbo(dispC);
        dispP = makeTex(sz, GL_RGBA8, GL_RGBA, true); dispPFbo = makeFbo(dispP);
        mask = makeTex(sz, GL_R8, GL_RED, false); maskFbo = makeFbo(mask);
        dummyMask = makeTex(1, GL_R8, GL_RED, false);
        { unsigned char w = 255; glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &w); }
        addLayer();
        dirty = true;
    }
    void clearChannels(Layer& L, bool c, bool r, bool m, const float col[4], const float pbr[4]) {
        glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
        glViewport(0, 0, size, size);
        glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (c) glClearBufferfv(GL_COLOR, 0, col);
        if (r || m) {
            glColorMaski(1, (GLboolean)r, (GLboolean)r, (GLboolean)m, (GLboolean)m);
            glClearBufferfv(GL_COLOR, 1, pbr);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        dirty = true;
    }
    void addLayer() {
        if (layers.size() >= kMaxLayers) return;
        Layer L;
        L.id = nextId++;
        L.name = "Layer " + std::to_string(L.id);
        L.tex = makeTex(size, GL_RGBA8, GL_RGBA, false);
        L.texP = makeTex(size, GL_RGBA8, GL_RGBA, false);
        L.fbo = makeFbo2(L.tex, L.texP);
        const float z[4] = {0, 0, 0, 0};
        clearChannels(L, true, true, true, z, z);
        int pos = layers.empty() ? 0 : active + 1;
        layers.insert(layers.begin() + pos, L);
        active = pos;
        dirty = true;
    }
    void deleteActive() {
        if (layers.size() <= 1) return;
        freeLayer(layers[active]);
        layers.erase(layers.begin() + active);
        active = std::min(active, (int)layers.size() - 1);
        dirty = true;
    }
    void moveActive(int dir) {
        int j = active + dir;
        if (j < 0 || j >= (int)layers.size()) return;
        std::swap(layers[active], layers[j]);
        active = j; dirty = true;
    }
    Layer* findLayer(int id) {
        for (auto& L : layers) if (L.id == id) return &L;
        return nullptr;
    }
    // ---------- mask layer ----------
    void fillMask(Layer& L, float v) {
        if (!L.hasMask) return;
        glBindFramebuffer(GL_FRAMEBUFFER, L.maskFbo);
        glViewport(0, 0, size, size);
        glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const float c[4] = {v, v, v, v};
        glClearBufferfv(GL_COLOR, 0, c);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        dirty = true;
    }
    void ensureMask(Layer& L, float initial) {
        if (L.hasMask) return;
        L.maskTex = makeTex(size, GL_R8, GL_RED, false);
        { GLint sw[4] = {GL_RED, GL_RED, GL_RED, GL_ONE}; glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw); }
        L.maskFbo = makeFbo(L.maskTex);
        L.hasMask = true; L.maskOn = true;
        fillMask(L, initial);
    }
    void removeMask(Layer& L) {
        if (!L.hasMask) return;
        if (L.maskFbo) glDeleteFramebuffers(1, &L.maskFbo);
        if (L.maskTex) glDeleteTextures(1, &L.maskTex);
        L.maskFbo = L.maskTex = 0; L.hasMask = false;
        dirty = true;
    }
    void readMask(const Layer& L, std::vector<unsigned char>& out) {
        out.resize((size_t)size * size);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glBindFramebuffer(GL_FRAMEBUFFER, L.maskFbo);
        glReadPixels(0, 0, size, size, GL_RED, GL_UNSIGNED_BYTE, out.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    void writeMask(const Layer& L, const std::vector<unsigned char>& in) {
        if (!L.hasMask) return;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, L.maskTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_UNSIGNED_BYTE, in.data());
        dirty = true;
    }
    void invertMask(Layer& L) {
        if (!L.hasMask) return;
        std::vector<unsigned char> m; readMask(L, m);
        for (auto& b : m) b = (unsigned char)(255 - b);
        writeMask(L, m);
    }
    // ---------- undo ----------
    void readAtt(const Layer& L, int att, std::vector<unsigned char>& out) {
        out.resize((size_t)size * size * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
        glReadBuffer((GLenum)(GL_COLOR_ATTACHMENT0 + att));
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    void writeTex(GLuint t, const std::vector<unsigned char>& in) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, in.data());
        dirty = true;
    }
    void pushUndo(bool maskTarget) {
        Layer& L = layers[active];
        UndoEntry e; e.layerId = L.id; e.mask = maskTarget && L.hasMask;
        if (e.mask) readMask(L, e.pxM);
        else { readAtt(L, 0, e.px); readAtt(L, 1, e.pxP); }
        undoS.push_back(std::move(e));
        redoS.clear();
        if (undoS.size() > maxStates()) undoS.erase(undoS.begin());
    }
    void applyEntry(Layer& L, const UndoEntry& e, UndoEntry& inverse) {
        inverse.layerId = e.layerId; inverse.mask = e.mask;
        if (e.mask) { readMask(L, inverse.pxM); writeMask(L, e.pxM); }
        else { readAtt(L, 0, inverse.px); readAtt(L, 1, inverse.pxP); writeTex(L.tex, e.px); writeTex(L.texP, e.pxP); }
    }
    void undo() {
        while (!undoS.empty()) {
            UndoEntry e = std::move(undoS.back()); undoS.pop_back();
            Layer* L = findLayer(e.layerId);
            if (!L || (e.mask && !L->hasMask)) continue;
            UndoEntry r; applyEntry(*L, e, r);
            redoS.push_back(std::move(r));
            return;
        }
    }
    void redo() {
        while (!redoS.empty()) {
            UndoEntry e = std::move(redoS.back()); redoS.pop_back();
            Layer* L = findLayer(e.layerId);
            if (!L || (e.mask && !L->hasMask)) continue;
            UndoEntry r; applyEntry(*L, e, r);
            undoS.push_back(std::move(r));
            return;
        }
    }
    // ---------- komposit ----------
    void buildMask(const Mesh& m) {
        if (!m.vao || !m.hasUV) { maskValid = false; return; }
        glBindFramebuffer(GL_FRAMEBUFFER, maskFbo);
        glViewport(0, 0, size, size);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(maskProg);
        m.draw();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        maskValid = true; dirty = true;
    }
    void compose(const float base[3], float baseRough, float baseMetal, bool dilate) {
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, size, size);
        glBindVertexArray(emptyVao);
        cur = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, compFbo[cur]);
        const float c0[4] = {base[0], base[1], base[2], 1.0f};
        const float c1[4] = {baseRough, baseMetal, 0.0f, 1.0f};
        glClearBufferfv(GL_COLOR, 0, c0);
        glClearBufferfv(GL_COLOR, 1, c1);

        glUseProgram(compProg);
        glUniform1i(glGetUniformLocation(compProg, "uDst"), 0);
        glUniform1i(glGetUniformLocation(compProg, "uLayer"), 1);
        glUniform1i(glGetUniformLocation(compProg, "uDstP"), 2);
        glUniform1i(glGetUniformLocation(compProg, "uLayerP"), 3);
        glUniform1i(glGetUniformLocation(compProg, "uMask"), 4);
        GLint locOp = glGetUniformLocation(compProg, "uOpacity");
        GLint locMode = glGetUniformLocation(compProg, "uMode");
        GLint locHasMask = glGetUniformLocation(compProg, "uHasMask");
        for (auto& L : layers) {
            if (!L.visible || L.opacity <= 0.0f) continue;
            bool useMask = L.hasMask && L.maskOn;
            glBindFramebuffer(GL_FRAMEBUFFER, compFbo[1 - cur]);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, compC[cur]);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, L.tex);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, compP[cur]);
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, L.texP);
            glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, useMask ? L.maskTex : dummyMask);
            glUniform1f(locOp, L.opacity);
            glUniform1i(locMode, L.blend);
            glUniform1i(locHasMask, useMask ? 1 : 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            cur = 1 - cur;
        }

        if (dilate && maskValid) {
            glUseProgram(dilProg);
            glUniform1i(glGetUniformLocation(dilProg, "uSrc"), 0);
            glUniform1i(glGetUniformLocation(dilProg, "uMask"), 1);
            glUniform1i(glGetUniformLocation(dilProg, "uSrcP"), 2);
            GLint locStage = glGetUniformLocation(dilProg, "uStage");
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, mask);
            int passes = std::max(2, size / 256);
            for (int i = -1; i < passes; i++) {
                glUniform1i(locStage, i < 0 ? 0 : 1);
                glBindFramebuffer(GL_FRAMEBUFFER, compFbo[1 - cur]);
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, compC[cur]);
                glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, compP[cur]);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                cur = 1 - cur;
            }
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, compFbo[cur]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dispCFbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dispPFbo);
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, dispC); glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, dispP); glGenerateMipmap(GL_TEXTURE_2D);
        glBindVertexArray(0);
        dirty = false;
    }
    void readFinal(std::vector<unsigned char>& rgb, std::vector<unsigned char>& rough, std::vector<unsigned char>& metal) {
        size_t n = (size_t)size * size;
        std::vector<unsigned char> a(n * 4), b(n * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, compFbo[cur]);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, a.data());
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        rgb.resize(n * 3); rough.resize(n); metal.resize(n);
        for (size_t i = 0; i < n; i++) {
            rgb[3 * i] = a[4 * i]; rgb[3 * i + 1] = a[4 * i + 1]; rgb[3 * i + 2] = a[4 * i + 2];
            rough[i] = b[4 * i]; metal[i] = b[4 * i + 1];
        }
    }
};

// ================================================================ baking
enum BakeType { BK_AO = 0, BK_CURV, BK_THICK, BK_POS, BK_NRMOBJ, BK_NRMTAN, BK_HEIGHT, BK_COUNT };
static const char* kBakeShort[BK_COUNT] = {"AO", "Curvature", "Thickness", "Position", "Normal (Object)", "Normal (Tangent)", "Height"};
static const char* kBakeFile[BK_COUNT] = {"AO", "Curvature", "Thickness", "Position", "NormalObject", "NormalTangent", "Height"};
static const int kBakeCh[BK_COUNT] = {1, 1, 1, 3, 3, 3, 1};

struct BakeParams {
    bool on[BK_COUNT] = {true, true, false, false, false, false, false};
    int samples = 32;
    float aoDistPct = 30.0f, thickDistPct = 40.0f, power = 1.0f;
    bool flip = false;
    int curvRadius = 2;
    float curvStrength = 2.0f;
    float cagePct = 5.0f;
};

struct BakeJob {
    std::atomic<bool> running{false}, done{false}, cancel{false};
    std::atomic<int> rowsDone{0}, stage{-1};   // stage: >=0 jenis map, -1 BVH low, -2 BVH high, -3 padding
    std::thread master;
    int size = 0;
    BakeParams P;
    float modelRadius = 1.0f;
    glm::vec3 bmin{0}, bmax{1};
    std::vector<float> pos, nrm;      // RGBA32F per texel (G-buffer)
    std::vector<Vertex> tris, high;
    std::vector<unsigned char> out[BK_COUNT];
    bool made[BK_COUNT] = {};
};

struct BakeMaps {
    int size = 0;
    bool has[BK_COUNT] = {};
    GLuint tex[BK_COUNT] = {};
    std::vector<unsigned char> px[BK_COUNT];

    static GLuint upload(int sz, int ch, const std::vector<unsigned char>& d) {
        std::vector<unsigned char> rgb;
        const unsigned char* src = d.data();
        if (ch == 1) {
            rgb.resize((size_t)sz * sz * 3);
            for (size_t i = 0; i < (size_t)sz * sz; i++) rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = d[i];
            src = rgb.data();
        }
        GLuint t = makeTex(sz, GL_RGB8, GL_RGB, true);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sz, sz, GL_RGB, GL_UNSIGNED_BYTE, src);
        glGenerateMipmap(GL_TEXTURE_2D);
        return t;
    }
    void clearOne(int i) {
        if (tex[i]) glDeleteTextures(1, &tex[i]);
        tex[i] = 0; has[i] = false; px[i].clear();
    }
    void clear() { for (int i = 0; i < BK_COUNT; i++) clearOne(i); size = 0; }
    bool any() const { for (int i = 0; i < BK_COUNT; i++) if (has[i]) return true; return false; }
    int firstHas() const { for (int i = 0; i < BK_COUNT; i++) if (has[i]) return i; return -1; }
    void set(int sz, BakeJob& J) {
        if (size != sz) clear();
        size = sz;
        for (int i = 0; i < BK_COUNT; i++) {
            if (!J.made[i]) continue;
            clearOne(i);
            px[i] = J.out[i];
            tex[i] = upload(sz, kBakeCh[i], px[i]);
            has[i] = true;
        }
    }
};

// Render posisi + normal tiap texel (ruang UV) ke array float
static void renderGBuffer(const Mesh& m, GLuint prog, int sz, std::vector<float>& pos, std::vector<float>& nrm) {
    GLuint t[2];
    glGenTextures(2, t);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, t[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, sz, sz, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    GLuint f = makeFbo2(t[0], t[1]);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glViewport(0, 0, sz, sz);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float z[4] = {0, 0, 0, 0};
    glClearBufferfv(GL_COLOR, 0, z);
    glClearBufferfv(GL_COLOR, 1, z);
    glUseProgram(prog);
    m.draw();
    pos.resize((size_t)sz * sz * 4); nrm.resize((size_t)sz * sz * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, sz, sz, GL_RGBA, GL_FLOAT, pos.data());
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glReadPixels(0, 0, sz, sz, GL_RGBA, GL_FLOAT, nrm.data());
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &f);
    glDeleteTextures(2, t);
}

static unsigned hash2(unsigned x, unsigned y) {
    unsigned h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float radicalInverse2(unsigned bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}
// Lebarkan piksel valid keluar island UV (padding seam), ch = jumlah kanal (1 atau 3)
static void dilateN(std::vector<unsigned char>& img, const std::vector<unsigned char>& cover, int sz, int ch, int passes) {
    std::vector<unsigned char> valid = cover, nimg, nvalid;
    for (int p = 0; p < passes; p++) {
        nimg = img; nvalid = valid;
        bool any = false;
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (valid[i]) continue;
                int sum[3] = {0, 0, 0}, cnt = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= sz || yy >= sz) continue;
                        size_t j = (size_t)yy * sz + xx;
                        if (valid[j]) { for (int c = 0; c < ch; c++) sum[c] += img[j * ch + c]; cnt++; }
                    }
                }
                if (cnt > 0) {
                    for (int c = 0; c < ch; c++) nimg[i * ch + c] = (unsigned char)(sum[c] / cnt);
                    nvalid[i] = 1; any = true;
                }
            }
        }
        img.swap(nimg); valid.swap(nvalid);
        if (!any) break;
    }
}

template <class F>
static void parallelRows(BakeJob* J, int sz, F fn) {
    J->rowsDone = 0;
    std::atomic<int> next(0);
    auto worker = [&]() {
        for (;;) {
            if (J->cancel) return;
            int y = next.fetch_add(1);
            if (y >= sz) return;
            fn(y);
            J->rowsDone.fetch_add(1);
        }
    };
    unsigned hc = std::thread::hardware_concurrency();
    int nt = hc > 1 ? (int)hc - 1 : 1;
    std::vector<std::thread> th;
    for (int i = 0; i < nt; i++) th.emplace_back(worker);
    for (auto& t : th) t.join();
}

static unsigned char toByte(float v) { return (unsigned char)(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

static void bakeMain(BakeJob* J) {
    const int sz = J->size;
    const size_t n = (size_t)sz * sz;
    const BakeParams P = J->P;
    const float R = J->modelRadius;
    const int padPasses = std::max(4, sz / 128);

    std::vector<unsigned char> cover(n);
    for (size_t i = 0; i < n; i++) cover[i] = J->pos[4 * i + 3] > 0.5f ? 1 : 0;

    auto okc = [&](int x, int y) { return x >= 0 && y >= 0 && x < sz && y < sz && cover[(size_t)y * sz + x] != 0; };
    auto pos3 = [&](int x, int y) { size_t i = (size_t)y * sz + x; return glm::vec3(J->pos[4 * i], J->pos[4 * i + 1], J->pos[4 * i + 2]); };
    auto nrm3 = [&](int x, int y) { size_t i = (size_t)y * sz + x; return glm::vec3(J->nrm[4 * i], J->nrm[4 * i + 1], J->nrm[4 * i + 2]); };
    auto finish = [&](int type) {
        if (J->cancel) return;
        J->stage = -3;
        dilateN(J->out[type], cover, sz, kBakeCh[type], padPasses);
        J->made[type] = true;
    };
    // normal texel (sudah dinormalisasi, bisa dibalik)
    auto texelNormal = [&](int x, int y, glm::vec3& N) {
        N = nrm3(x, y);
        float ln = glm::length(N);
        if (ln < 1e-5f) return false;
        N /= ln;
        if (P.flip) N = -N;
        return true;
    };
    auto frameFromAxis = [](const glm::vec3& axis, glm::vec3& T, glm::vec3& B) {
        glm::vec3 up = std::fabs(axis.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        T = glm::normalize(glm::cross(up, axis));
        B = glm::cross(axis, T);
    };

    // ---------------- BVH low-poly (AO, Thickness) ----------------
    BVH low;
    if ((P.on[BK_AO] || P.on[BK_THICK]) && !J->cancel) { J->stage = -1; low.build(J->tris); }

    // ---------------- AO ----------------
    if (P.on[BK_AO] && !J->cancel) {
        J->stage = BK_AO;
        J->out[BK_AO].assign(n, 255);
        const float maxDist = std::max(P.aoDistPct / 100.0f * R, 1e-5f);
        const float eps = R * 0.001f;
        const int S = P.samples;
        parallelRows(J, sz, [&](int y) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (!cover[i]) continue;
                glm::vec3 N;
                if (!texelNormal(x, y, N)) continue;
                glm::vec3 org = pos3(x, y) + N * eps;
                glm::vec3 T, B; frameFromAxis(N, T, B);
                float rnd = (float)(hash2((unsigned)x, (unsigned)y) & 0xFFFFu) / 65536.0f;
                int hits = 0;
                for (int s = 0; s < S; s++) {
                    float u1 = ((float)s + 0.5f) / (float)S;
                    float u2 = radicalInverse2((unsigned)s) + rnd; u2 -= std::floor(u2);
                    float r = std::sqrt(u1), phi = 6.2831853f * u2;
                    glm::vec3 d = T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + N * std::sqrt(1.0f - u1);
                    if (low.anyHit(org, d, maxDist)) hits++;
                }
                float ao = 1.0f - (float)hits / (float)S;
                J->out[BK_AO][i] = toByte(std::pow(ao, P.power));
            }
        });
        finish(BK_AO);
    }

    // ---------------- Thickness ----------------
    if (P.on[BK_THICK] && !J->cancel) {
        J->stage = BK_THICK;
        J->out[BK_THICK].assign(n, 255);
        const float maxDist = std::max(P.thickDistPct / 100.0f * R, 1e-5f);
        const float eps = R * 0.001f;
        const int S = P.samples;
        parallelRows(J, sz, [&](int y) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (!cover[i]) continue;
                glm::vec3 N;
                if (!texelNormal(x, y, N)) continue;
                glm::vec3 axis = -N;                 // sinar ke arah dalam
                glm::vec3 org = pos3(x, y) + axis * eps;
                glm::vec3 T, B; frameFromAxis(axis, T, B);
                float rnd = (float)(hash2((unsigned)x, (unsigned)y) & 0xFFFFu) / 65536.0f;
                float sum = 0.0f;
                for (int s = 0; s < S; s++) {
                    float u1 = ((float)s + 0.5f) / (float)S;
                    float u2 = radicalInverse2((unsigned)s) + rnd; u2 -= std::floor(u2);
                    float r = std::sqrt(u1), phi = 6.2831853f * u2;
                    glm::vec3 d = T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + axis * std::sqrt(1.0f - u1);
                    float t = maxDist; int tri = 0; float bu = 0, bw = 0;
                    if (low.closestHit(org, d, maxDist, t, tri, bu, bw)) sum += t; else sum += maxDist;
                }
                J->out[BK_THICK][i] = toByte(sum / ((float)S * maxDist));
            }
        });
        finish(BK_THICK);
    }

    // ---------------- Curvature ----------------
    if (P.on[BK_CURV] && !J->cancel) {
        J->stage = BK_CURV;
        const int k = P.curvRadius;
        std::vector<float> cf(n, 0.5f);
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (!cover[i]) continue;
                float c = 0.0f; int cnt = 0;
                if (okc(x - k, y) && okc(x + k, y)) {
                    glm::vec3 dP = pos3(x + k, y) - pos3(x - k, y), dN = nrm3(x + k, y) - nrm3(x - k, y);
                    float d2 = glm::dot(dP, dP);
                    if (d2 > 1e-12f) { c += glm::dot(dN, dP) / d2; cnt++; }
                }
                if (okc(x, y - k) && okc(x, y + k)) {
                    glm::vec3 dP = pos3(x, y + k) - pos3(x, y - k), dN = nrm3(x, y + k) - nrm3(x, y - k);
                    float d2 = glm::dot(dP, dP);
                    if (d2 > 1e-12f) { c += glm::dot(dN, dP) / d2; cnt++; }
                }
                if (cnt > 0) c /= (float)cnt;
                cf[i] = 0.5f + 0.5f * std::tanh(c * R * P.curvStrength * 0.15f);
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            std::vector<float> tmp = cf;
            for (int y = 0; y < sz; y++) {
                for (int x = 0; x < sz; x++) {
                    size_t i = (size_t)y * sz + x;
                    if (!cover[i]) continue;
                    float sum = 0.0f; int cnt = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            if (okc(x + dx, y + dy)) { sum += cf[(size_t)(y + dy) * sz + (x + dx)]; cnt++; }
                    if (cnt > 0) tmp[i] = sum / (float)cnt;
                }
            }
            cf.swap(tmp);
        }
        J->out[BK_CURV].assign(n, 128);
        for (size_t i = 0; i < n; i++) J->out[BK_CURV][i] = toByte(cf[i]);
        finish(BK_CURV);
    }

    // ---------------- Position (object space, dinormalisasi ke bounding box) ----------------
    if (P.on[BK_POS] && !J->cancel) {
        J->stage = BK_POS;
        J->out[BK_POS].assign(n * 3, 0);
        glm::vec3 ext = glm::max(J->bmax - J->bmin, glm::vec3(1e-6f));
        for (size_t i = 0; i < n; i++) {
            if (!cover[i]) continue;
            glm::vec3 p(J->pos[4 * i], J->pos[4 * i + 1], J->pos[4 * i + 2]);
            glm::vec3 q = (p - J->bmin) / ext;
            for (int c = 0; c < 3; c++) J->out[BK_POS][3 * i + c] = toByte(q[c]);
        }
        finish(BK_POS);
    }

    // ---------------- Normal object space ----------------
    if (P.on[BK_NRMOBJ] && !J->cancel) {
        J->stage = BK_NRMOBJ;
        J->out[BK_NRMOBJ].assign(n * 3, 128);
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (!cover[i]) continue;
                glm::vec3 N;
                if (!texelNormal(x, y, N)) continue;
                for (int c = 0; c < 3; c++) J->out[BK_NRMOBJ][3 * i + c] = toByte(N[c] * 0.5f + 0.5f);
            }
        }
        finish(BK_NRMOBJ);
    }

    // ---------------- Normal tangent + Height (dari high-poly) ----------------
    if ((P.on[BK_NRMTAN] || P.on[BK_HEIGHT]) && !J->cancel && !J->high.empty()) {
        J->stage = -2;
        BVH hi; hi.build(J->high, true);
        if (P.on[BK_NRMTAN]) J->out[BK_NRMTAN].assign(n * 3, 0);
        if (P.on[BK_HEIGHT]) J->out[BK_HEIGHT].assign(n, 128);
        if (P.on[BK_NRMTAN]) for (size_t i = 0; i < n; i++) { J->out[BK_NRMTAN][3 * i] = 128; J->out[BK_NRMTAN][3 * i + 1] = 128; J->out[BK_NRMTAN][3 * i + 2] = 255; }
        J->stage = P.on[BK_NRMTAN] ? (int)BK_NRMTAN : (int)BK_HEIGHT;
        const float cage = std::max(P.cagePct / 100.0f * R, 1e-5f);

        auto tangentFrame = [&](int x, int y, const glm::vec3& N, glm::vec3& T, glm::vec3& B) {
            glm::vec3 dPdu(0.0f), dPdv(0.0f);
            if (okc(x + 1, y) && okc(x - 1, y)) dPdu = pos3(x + 1, y) - pos3(x - 1, y);
            else if (okc(x + 1, y)) dPdu = pos3(x + 1, y) - pos3(x, y);
            else if (okc(x - 1, y)) dPdu = pos3(x, y) - pos3(x - 1, y);
            if (okc(x, y + 1) && okc(x, y - 1)) dPdv = pos3(x, y + 1) - pos3(x, y - 1);
            else if (okc(x, y + 1)) dPdv = pos3(x, y + 1) - pos3(x, y);
            else if (okc(x, y - 1)) dPdv = pos3(x, y) - pos3(x, y - 1);
            T = dPdu - N * glm::dot(N, dPdu);
            if (glm::length(T) < 1e-12f) {
                glm::vec3 up = std::fabs(N.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                T = glm::cross(up, N);
            }
            T = glm::normalize(T);
            B = glm::cross(N, T);
            if (glm::dot(B, dPdv) < 0.0f) B = -B;
        };

        parallelRows(J, sz, [&](int y) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (!cover[i]) continue;
                glm::vec3 N;
                if (!texelNormal(x, y, N)) continue;
                glm::vec3 org = pos3(x, y) + N * cage;
                float t = 0.0f, bu = 0.0f, bw = 0.0f; int tri = 0;
                if (!hi.closestHit(org, -N, 2.0f * cage, t, tri, bu, bw)) continue;
                if (P.on[BK_NRMTAN]) {
                    glm::vec3 hn = (1.0f - bu - bw) * hi.n0[tri] + bu * hi.n1[tri] + bw * hi.n2[tri];
                    if (glm::length(hn) > 1e-6f) {
                        hn = glm::normalize(hn);
                        glm::vec3 T, B; tangentFrame(x, y, N, T, B);
                        glm::vec3 ts(glm::dot(hn, T), glm::dot(hn, B), glm::dot(hn, N));
                        if (glm::length(ts) > 1e-6f) ts = glm::normalize(ts);
                        for (int c = 0; c < 3; c++) J->out[BK_NRMTAN][3 * i + c] = toByte(ts[c] * 0.5f + 0.5f);
                    }
                }
                if (P.on[BK_HEIGHT]) {
                    float h = (cage - t) / cage;           // + = di atas permukaan low-poly
                    J->out[BK_HEIGHT][i] = toByte(0.5f + 0.5f * glm::clamp(h, -1.0f, 1.0f));
                }
            }
        });
        if (P.on[BK_NRMTAN]) finish(BK_NRMTAN);
        if (P.on[BK_HEIGHT]) finish(BK_HEIGHT);
    }
    J->done = true;
}

// ================================================================ material + mask generator
struct Material { const char* name; float c[3]; float rough, metal; };
enum { M_STEEL, M_ALU, M_GOLD, M_COPPER, M_RUST, M_PLASTIC, M_RUBBER, M_CONCRETE, M_WOOD, M_PAINT, M_LEATHER, M_DIRT, M_DUST, M_PATINA, M_MOSS, M_MUD, M_COUNT };
static const Material kMats[M_COUNT] = {
    {"Steel",          {0.56f, 0.57f, 0.58f}, 0.35f, 1.0f},
    {"Aluminium",      {0.91f, 0.92f, 0.92f}, 0.30f, 1.0f},
    {"Gold",           {1.00f, 0.77f, 0.34f}, 0.25f, 1.0f},
    {"Copper",         {0.95f, 0.64f, 0.54f}, 0.30f, 1.0f},
    {"Karat",          {0.45f, 0.20f, 0.10f}, 0.85f, 0.10f},
    {"Plastik Putih",  {0.90f, 0.90f, 0.90f}, 0.45f, 0.0f},
    {"Karet Hitam",    {0.05f, 0.05f, 0.05f}, 0.90f, 0.0f},
    {"Beton",          {0.50f, 0.50f, 0.50f}, 0.95f, 0.0f},
    {"Kayu",           {0.45f, 0.28f, 0.15f}, 0.65f, 0.0f},
    {"Cat Merah",      {0.70f, 0.08f, 0.06f}, 0.35f, 0.0f},
    {"Kulit",          {0.25f, 0.13f, 0.08f}, 0.60f, 0.0f},
    {"Kotoran",        {0.10f, 0.08f, 0.06f}, 0.95f, 0.0f},
    {"Debu",           {0.75f, 0.72f, 0.66f}, 1.00f, 0.0f},
    {"Patina",         {0.25f, 0.55f, 0.45f}, 0.70f, 0.0f},
    {"Lumut",          {0.18f, 0.30f, 0.10f}, 0.90f, 0.0f},
    {"Lumpur",         {0.20f, 0.14f, 0.08f}, 0.95f, 0.0f},
};

// ================================================================ MG Mask Editor (generator mask) + Smart Mask
enum GenType { GEN_EDGE = 0, GEN_CAVITY, GEN_DIRT, GEN_THICK, GEN_DUST, GEN_BOTTOM, GEN_HEIGHT, GEN_NOISE, GEN_TYPE_COUNT };
static const char* kGenNames[GEN_TYPE_COUNT] = {
    "Edge (Curvature cembung)", "Cavity (Curvature cekung)", "Dirt (AO)", "Tipis (Thickness)",
    "Debu (permukaan atas)", "Bawah (Position)", "Height (high-poly)", "Noise / Grunge / Goresan"};
static const char* kOpNames[6] = {"Multiply", "Add", "Max", "Min", "Subtract", "Screen"};

struct GenStage {
    int type = GEN_EDGE;
    bool enabled = true;
    float lo = 0.5f, hi = 0.8f;      // rentang (smoothstep) -> mengatur luas / ketebalan efek
    bool invert = false;
    float strength = 1.0f;           // bobot tahap ini
    int op = 0;                      // cara menggabung dengan tahap sebelumnya
    float jitter = 0.0f;             // variasi acak pada input (procedural noise)
    float scale = 24.0f;             // parameter noise
    float stretch = 1.0f;
    float angle = 0.0f;
    int octaves = 4;
    int seed = 1;
};
struct MaskGen {
    std::vector<GenStage> st;
    float blur = 0.0f;               // px
    float level = 0.5f;              // 0.5 netral, >0.5 lebih tebal / luas
    float contrast = 1.0f;
    bool invertAll = false;
};
struct SmartMaskDef { std::string name, desc; MaskGen gen; bool builtin = true; };

static float sstep(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    float t = glm::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static float vhash(int x, int y) { return (float)(hash2((unsigned)x, (unsigned)y) & 0xFFFFFFu) / 16777215.0f; }
static float vnoise(float x, float y) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - (float)xi, fy = y - (float)yi;
    float u = fx * fx * (3.0f - 2.0f * fx), v = fy * fy * (3.0f - 2.0f * fy);
    float a = vhash(xi, yi), b = vhash(xi + 1, yi), c = vhash(xi, yi + 1), d = vhash(xi + 1, yi + 1);
    return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v);
}
// noise fbm dengan skala, regangan (anisotropi), sudut, oktaf, seed
static float noiseSample(const GenStage& s, float u, float v) {
    float x = u * s.scale, y = v * s.scale;
    float ca = std::cos(glm::radians(s.angle)), sa = std::sin(glm::radians(s.angle));
    float a = (x * ca + y * sa) / std::max(s.stretch, 1.0f);
    float b = -x * sa + y * ca;
    float off = (float)s.seed * 37.0f;
    a += off; b += off * 0.7f;
    float amp = 0.5f, sum = 0.0f, norm = 0.0f;
    int oct = std::max(1, std::min(6, s.octaves));
    for (int i = 0; i < oct; i++) { sum += amp * vnoise(a, b); norm += amp; a *= 2.0f; b *= 2.0f; amp *= 0.5f; }
    return sum / norm;
}
static int genStageMap(int type) {   // map bake yang dibutuhkan, -1 = tidak ada
    switch (type) {
        case GEN_EDGE: case GEN_CAVITY: return BK_CURV;
        case GEN_DIRT: return BK_AO;
        case GEN_THICK: return BK_THICK;
        case GEN_DUST: return BK_NRMOBJ;
        case GEN_BOTTOM: return BK_POS;
        case GEN_HEIGHT: return BK_HEIGHT;
        default: return -1;
    }
}
static void maskGenNeeds(const MaskGen& g, std::vector<int>& need) {
    need.clear();
    for (const GenStage& s : g.st) {
        if (!s.enabled) continue;
        int m = genStageMap(s.type);
        if (m >= 0 && std::find(need.begin(), need.end(), m) == need.end()) need.push_back(m);
    }
}
template <class F>
static void parallelForRows(int sz, F fn) {
    std::atomic<int> next(0);
    auto worker = [&]() { for (;;) { int y = next.fetch_add(1); if (y >= sz) return; fn(y); } };
    unsigned hc = std::thread::hardware_concurrency();
    int nt = hc > 1 ? (int)hc : 1;
    std::vector<std::thread> th;
    for (int i = 0; i < nt; i++) th.emplace_back(worker);
    for (auto& t : th) t.join();
}
static void boxBlur(std::vector<float>& a, int sz, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(a.size()), pre((size_t)sz + 1);
    for (int y = 0; y < sz; y++) {
        pre[0] = 0.0f;
        for (int x = 0; x < sz; x++) pre[x + 1] = pre[x] + a[(size_t)y * sz + x];
        for (int x = 0; x < sz; x++) {
            int lo = std::max(0, x - r), hi = std::min(sz - 1, x + r);
            tmp[(size_t)y * sz + x] = (pre[hi + 1] - pre[lo]) / (float)(hi - lo + 1);
        }
    }
    for (int x = 0; x < sz; x++) {
        pre[0] = 0.0f;
        for (int y = 0; y < sz; y++) pre[y + 1] = pre[y] + tmp[(size_t)y * sz + x];
        for (int y = 0; y < sz; y++) {
            int lo = std::max(0, y - r), hi = std::min(sz - 1, y + r);
            a[(size_t)y * sz + x] = (pre[hi + 1] - pre[lo]) / (float)(hi - lo + 1);
        }
    }
}
// Jalankan tumpukan generator -> mask 8-bit. false jika map bake yang dibutuhkan belum ada.
static bool genMaskStack(const BakeMaps& bm, int sz, const MaskGen& g, std::vector<unsigned char>& out) {
    std::vector<int> need;
    maskGenNeeds(g, need);
    for (int t : need) if (!bm.has[t] || bm.size != sz) return false;
    size_t n = (size_t)sz * sz;
    std::vector<float> acc(n, 0.0f);
    bool first = true;
    for (const GenStage& s : g.st) {
        if (!s.enabled) continue;
        int mt = genStageMap(s.type);
        const std::vector<unsigned char>* map = (mt >= 0) ? &bm.px[mt] : nullptr;
        parallelForRows(sz, [&](int y) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                float u = ((float)x + 0.5f) / (float)sz, v = ((float)y + 0.5f) / (float)sz;
                float base = 0.0f;
                switch (s.type) {
                    case GEN_EDGE:   base = (*map)[i] / 255.0f; break;
                    case GEN_CAVITY: base = 1.0f - (*map)[i] / 255.0f; break;
                    case GEN_DIRT:   base = 1.0f - (*map)[i] / 255.0f; break;
                    case GEN_THICK:  base = 1.0f - (*map)[i] / 255.0f; break;
                    case GEN_DUST:   base = (*map)[3 * i + 1] / 255.0f * 2.0f - 1.0f; break;
                    case GEN_BOTTOM: base = (*map)[3 * i + 1] / 255.0f; break;
                    case GEN_HEIGHT: base = (*map)[i] / 255.0f; break;
                    default:         base = noiseSample(s, u, v); break;
                }
                if (s.jitter > 0.0f && s.type != GEN_NOISE) base += (noiseSample(s, u, v) - 0.5f) * s.jitter;
                float val = sstep(s.lo, s.hi, base);
                if (s.type == GEN_BOTTOM) val = 1.0f - val;
                if (s.invert) val = 1.0f - val;
                if (first) { acc[i] = val; continue; }
                float a = acc[i], r;
                switch (s.op) {
                    case 0: r = a * val; break;
                    case 1: r = std::min(a + val, 1.0f); break;
                    case 2: r = std::max(a, val); break;
                    case 3: r = std::min(a, val); break;
                    case 4: r = std::max(a - val, 0.0f); break;
                    default: r = 1.0f - (1.0f - a) * (1.0f - val); break;
                }
                acc[i] = a + (r - a) * s.strength;
            }
        });
        first = false;
    }
    int br = (int)std::lround(g.blur);
    if (br > 0) { boxBlur(acc, sz, br); boxBlur(acc, sz, br); }
    out.resize(n);
    float expo = std::pow(4.0f, (0.5f - g.level) * 2.0f);   // level 0.5 = netral
    for (size_t i = 0; i < n; i++) {
        float v = std::pow(glm::clamp(acc[i], 0.0f, 1.0f), expo);
        v = glm::clamp((v - 0.5f) * g.contrast + 0.5f, 0.0f, 1.0f);
        if (g.invertAll) v = 1.0f - v;
        out[i] = toByte(v);
    }
    return true;
}

static GenStage mkStage(int type, float lo, float hi, int op = 0, float strength = 1.0f, float jitter = 0.0f) {
    GenStage s; s.type = type; s.lo = lo; s.hi = hi; s.op = op; s.strength = strength; s.jitter = jitter;
    return s;
}
static GenStage mkNoise(float scale, int oct, float lo, float hi, int op = 0, float strength = 1.0f,
                        float stretch = 1.0f, float angle = 0.0f, int seed = 1) {
    GenStage s; s.type = GEN_NOISE; s.scale = scale; s.octaves = oct; s.lo = lo; s.hi = hi; s.op = op;
    s.strength = strength; s.stretch = stretch; s.angle = angle; s.seed = seed;
    return s;
}

enum { SM_EDGEWEAR = 0, SM_EDGEDAMAGE, SM_CAVITY, SM_DIRT, SM_DUST, SM_RUST, SM_GRIME, SM_STREAKS, SM_SCRATCHES, SM_MOSS, SM_GRUNGE, SM_BUILTIN_COUNT };

static std::vector<SmartMaskDef> builtinSmartMasks() {
    std::vector<SmartMaskDef> v;
    auto add = [&](const char* name, const char* desc, const MaskGen& g) {
        SmartMaskDef d; d.name = name; d.desc = desc; d.gen = g; v.push_back(d);
    };
    MaskGen g;
    g = MaskGen(); g.st.push_back(mkStage(GEN_EDGE, 0.55f, 0.75f, 0, 1.0f, 0.25f));
    add("Edge Wear", "Aus di sudut tajam / tepi. Butuh: Curvature", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_EDGE, 0.50f, 0.68f, 0, 1.0f, 0.15f));
    g.st.push_back(mkNoise(28.0f, 5, 0.42f, 0.58f, 0)); g.level = 0.55f;
    add("Edge Damage", "Tepi terkelupas tidak beraturan. Butuh: Curvature", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_CAVITY, 0.50f, 0.72f));
    g.st.push_back(mkStage(GEN_DIRT, 0.10f, 0.60f, 2, 0.7f));
    add("Cavity", "Celah dan lekukan terdalam. Butuh: Curvature, AO", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_DIRT, 0.10f, 0.55f, 0, 1.0f, 0.30f));
    g.st.push_back(mkNoise(16.0f, 5, 0.30f, 0.70f, 0, 0.6f));
    add("Dirt", "Kotoran di area tertutup (occlusion). Butuh: AO", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_DUST, 0.35f, 0.85f, 0, 1.0f, 0.30f));
    g.st.push_back(mkStage(GEN_DIRT, 0.10f, 0.50f, 2, 0.5f));
    g.st.push_back(mkNoise(20.0f, 5, 0.30f, 0.75f, 0, 0.7f));
    add("Dust", "Debu di permukaan atas dan celah. Butuh: Normal (Object), AO", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_DIRT, 0.05f, 0.45f));
    g.st.push_back(mkStage(GEN_EDGE, 0.55f, 0.70f, 2, 0.8f));
    g.st.push_back(mkNoise(12.0f, 5, 0.38f, 0.62f, 0)); g.level = 0.55f;
    add("Rust", "Karat di celah dan sudut dengan variasi acak. Butuh: AO, Curvature", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_BOTTOM, 0.0f, 0.45f, 0, 1.0f, 0.30f));
    g.st.push_back(mkNoise(14.0f, 5, 0.30f, 0.70f, 0, 0.7f));
    add("Bottom Grime", "Noda dan lumpur di bagian bawah. Butuh: Position", g);

    g = MaskGen(); g.st.push_back(mkNoise(22.0f, 4, 0.50f, 0.68f, 0, 1.0f, 14.0f, 90.0f));
    g.st.push_back(mkStage(GEN_DIRT, 0.05f, 0.50f, 0, 0.6f));
    add("Water Streaks", "Jejak air menetes ke bawah. Butuh: AO", g);

    g = MaskGen(); g.st.push_back(mkNoise(70.0f, 3, 0.60f, 0.66f, 0, 1.0f, 30.0f, 25.0f));
    g.st.push_back(mkNoise(55.0f, 3, 0.62f, 0.67f, 2, 1.0f, 25.0f, -40.0f, 7)); g.level = 0.55f;
    add("Scratches", "Goresan halus acak. Tidak butuh bake.", g);

    g = MaskGen(); g.st.push_back(mkStage(GEN_DUST, 0.30f, 0.80f));
    g.st.push_back(mkStage(GEN_CAVITY, 0.50f, 0.75f, 2, 0.6f));
    g.st.push_back(mkNoise(10.0f, 6, 0.40f, 0.60f, 0));
    add("Moss", "Lumut di atas dan celah lembap. Butuh: Normal (Object), Curvature", g);

    g = MaskGen(); g.st.push_back(mkNoise(8.0f, 6, 0.35f, 0.65f));
    add("Grunge", "Noise acak murni. Tidak butuh bake.", g);
    return v;
}

struct SmartLayer { int mat; int mask; bool inv; float opacity; int blend; const char* name; };
struct SmartMat { const char* name; const char* desc; int count; SmartLayer L[5]; };
static const SmartMat kSmart[] = {
    {"Machinery Rust", "Besi mesin berkarat, kotor, dan berdebu. Semua efek otomatis mengikuti bentuk model.", 4, {
        {M_STEEL, -1, false, 1.0f, 0, "Steel"},
        {M_RUST,  SM_RUST, false, 1.0f, 0, "Karat"},
        {M_DIRT,  SM_CAVITY, false, 0.7f, 0, "Kotoran celah"},
        {M_DUST,  SM_DUST, false, 0.6f, 0, "Debu"} }},
    {"Painted Steel", "Cat di atas besi: cat mengelupas di tepi dan memperlihatkan karat di bawahnya.", 4, {
        {M_STEEL, -1, false, 1.0f, 0, "Steel"},
        {M_RUST,  SM_EDGEDAMAGE, false, 1.0f, 0, "Karat bawah cat"},
        {M_PAINT, SM_EDGEDAMAGE, true,  1.0f, 0, "Cat"},
        {M_DIRT,  SM_DIRT, false, 0.6f, 0, "Kotoran"} }},
    {"Besi Cat Mengelupas", "Steel dengan cat merah mengelupas di tepi + kotoran.", 3, {
        {M_STEEL, -1, false, 1.0f, 0, "Steel"},
        {M_PAINT, SM_EDGEDAMAGE, true, 1.0f, 0, "Cat"},
        {M_DIRT,  SM_DIRT, false, 0.7f, 0, "Kotoran"} }},
    {"Besi Berkarat", "Steel dengan karat di celah dan di tepi.", 3, {
        {M_STEEL, -1, false, 1.0f, 0, "Steel"},
        {M_RUST,  SM_RUST, false, 1.0f, 0, "Karat"},
        {M_RUST,  SM_EDGEWEAR, false, 0.8f, 0, "Karat tepi"} }},
    {"Emas Aus", "Emas yang aus di tepi sampai terlihat logam di bawahnya.", 3, {
        {M_GOLD,  -1, false, 1.0f, 0, "Gold"},
        {M_STEEL, SM_EDGEWEAR, false, 1.0f, 0, "Aus"},
        {M_DIRT,  SM_DIRT, false, 0.5f, 0, "Kotoran"} }},
    {"Aluminium Tergores", "Aluminium dengan goresan halus dan kotoran di celah.", 3, {
        {M_ALU,  -1, false, 1.0f, 0, "Aluminium"},
        {M_DUST, SM_SCRATCHES, false, 0.6f, 0, "Goresan"},
        {M_DIRT, SM_CAVITY, false, 0.5f, 0, "Kotoran celah"} }},
    {"Karet Berdebu", "Karet hitam dengan debu di permukaan atas.", 2, {
        {M_RUBBER, -1, false, 1.0f, 0, "Karet"},
        {M_DUST,   SM_DUST, false, 0.85f, 0, "Debu"} }},
    {"Beton Kotor", "Beton dengan kotoran, noda di bawah, dan lumut.", 4, {
        {M_CONCRETE, -1, false, 1.0f, 0, "Beton"},
        {M_DIRT,     SM_DIRT, false, 1.0f, 0, "Kotoran"},
        {M_MUD,      SM_GRIME, false, 0.6f, 0, "Noda bawah"},
        {M_MOSS,     SM_MOSS, false, 0.7f, 0, "Lumut"} }},
    {"Kayu Lapuk", "Kayu dengan debu, lumut, dan kotoran di celah.", 4, {
        {M_WOOD, -1, false, 1.0f, 0, "Kayu"},
        {M_DUST, SM_DUST, false, 0.5f, 0, "Debu"},
        {M_MOSS, SM_MOSS, false, 0.8f, 0, "Lumut"},
        {M_DIRT, SM_CAVITY, false, 0.6f, 0, "Kotoran celah"} }},
    {"Tembaga Patina", "Tembaga dengan patina hijau di celah dan sudut.", 3, {
        {M_COPPER, -1, false, 1.0f, 0, "Tembaga"},
        {M_PATINA, SM_RUST, false, 0.9f, 0, "Patina"},
        {M_DIRT,   SM_DIRT, false, 0.4f, 0, "Kotoran"} }},
    {"Plastik Kusam", "Plastik putih dengan debu, kotoran, dan goresan.", 4, {
        {M_PLASTIC, -1, false, 1.0f, 0, "Plastik"},
        {M_DUST,    SM_DUST, false, 0.6f, 0, "Debu"},
        {M_DIRT,    SM_DIRT, false, 0.5f, 0, "Kotoran"},
        {M_DUST,    SM_SCRATCHES, false, 0.5f, 0, "Goresan"} }},
};
static const int kSmartCount = (int)(sizeof(kSmart) / sizeof(kSmart[0]));

static void smartNeeds(const std::vector<SmartMaskDef>& lib, int si, std::vector<int>& need) {
    need.clear();
    const SmartMat& sm = kSmart[si];
    for (int i = 0; i < sm.count; i++) {
        int mi = sm.L[i].mask;
        if (mi < 0 || mi >= (int)lib.size()) continue;
        std::vector<int> nn;
        maskGenNeeds(lib[mi].gen, nn);
        for (int t : nn) if (std::find(need.begin(), need.end(), t) == need.end()) need.push_back(t);
    }
}

// ================================================================ texture material (gambar)
struct TexMat {
    std::string name;
    GLuint c = 0, r = 0, m = 0;     // color, roughness, metallic (r/m opsional, dibaca dari kanal merah)
    float tile = 1.0f;
    std::string cFile, rFile, mFile;
};
static GLuint loadImageTex(const std::string& path, std::string& err) {
    stbi_set_flip_vertically_on_load(1);     // supaya gambar tegak di ruang UV (v naik)
    int w = 0, h = 0, n = 0;
    unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!d) { err = "Gagal membaca gambar: " + path; return 0; }
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(d);
    return t;
}

// ================================================================ camera
struct Camera {
    float yaw = 0.6f, pitch = 0.4f, dist = 5.0f;
    glm::vec3 target{0};
    glm::vec3 eye() const {
        return target + dist * glm::vec3(cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw));
    }
    glm::mat4 view() const { return glm::lookAt(eye(), target, glm::vec3(0, 1, 0)); }
    void fit(const glm::vec3& bmin, const glm::vec3& bmax) {
        target = (bmin + bmax) * 0.5f;
        float r = glm::length(bmax - bmin) * 0.5f;
        dist = std::max(r * 2.5f, 0.01f);
    }
};

// ================================================================ misc + UI helpers
static std::string g_dropped;
static void dropCallback(GLFWwindow*, int n, const char** paths) { if (n > 0) g_dropped = paths[0]; }

static std::string openFileDialog() {
#ifdef _WIN32
    char file[MAX_PATH] = {0};
    OPENFILENAMEA o{};
    o.lStructSize = sizeof(o);
    o.lpstrFilter = "Wavefront OBJ\0*.obj\0All\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&o)) return file;
#endif
    return "";
}
static std::string openImageDialog() {
#ifdef _WIN32
    char file[MAX_PATH] = {0};
    OPENFILENAMEA o{};
    o.lStructSize = sizeof(o);
    o.lpstrFilter = "Gambar\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&o)) return file;
#endif
    return "";
}
static std::string saveFileDialog() {
#ifdef _WIN32
    char file[MAX_PATH] = "texture.png";
    OPENFILENAMEA o{};
    o.lStructSize = sizeof(o);
    o.lpstrFilter = "PNG\0*.png\0";
    o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = "png";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&o)) return file;
    return "";
#else
    return "texture.png";
#endif
}
static std::string stripPng(const std::string& s) {
    if (s.size() >= 4) {
        std::string e = s.substr(s.size() - 4);
        for (auto& ch : e) ch = (char)std::tolower((unsigned char)ch);
        if (e == ".png") return s.substr(0, s.size() - 4);
    }
    return s;
}
static std::string baseName(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}

static const ImVec4 kAccent(0.20f, 0.55f, 0.95f, 1.0f);

static void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f; s.ChildRounding = 4.0f; s.FrameRounding = 4.0f; s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f; s.PopupRounding = 4.0f; s.ScrollbarRounding = 6.0f;
    s.WindowPadding = ImVec2(12, 10); s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7); s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowBorderSize = 1.0f; s.FrameBorderSize = 0.0f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]       = ImVec4(0.105f, 0.110f, 0.125f, 1.0f);
    c[ImGuiCol_ChildBg]        = ImVec4(0.085f, 0.090f, 0.105f, 1.0f);
    c[ImGuiCol_PopupBg]        = ImVec4(0.12f, 0.125f, 0.14f, 0.98f);
    c[ImGuiCol_Border]         = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.165f, 0.175f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive]  = ImVec4(0.24f, 0.27f, 0.32f, 1.0f);
    c[ImGuiCol_TitleBg]        = ImVec4(0.09f, 0.095f, 0.11f, 1.0f);
    c[ImGuiCol_TitleBgActive]  = ImVec4(0.09f, 0.095f, 0.11f, 1.0f);
    c[ImGuiCol_MenuBarBg]      = ImVec4(0.09f, 0.095f, 0.11f, 1.0f);
    c[ImGuiCol_Button]         = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_ButtonHovered]  = ImVec4(0.27f, 0.30f, 0.36f, 1.0f);
    c[ImGuiCol_ButtonActive]   = kAccent;
    c[ImGuiCol_Header]         = ImVec4(0.20f, 0.35f, 0.60f, 0.55f);
    c[ImGuiCol_HeaderHovered]  = ImVec4(0.22f, 0.40f, 0.70f, 0.70f);
    c[ImGuiCol_HeaderActive]   = ImVec4(0.22f, 0.45f, 0.80f, 0.90f);
    c[ImGuiCol_SliderGrab]     = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_CheckMark]      = kAccent;
    c[ImGuiCol_Tab]            = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
    c[ImGuiCol_TabHovered]     = ImVec4(0.22f, 0.40f, 0.70f, 1.0f);
    c[ImGuiCol_TabSelected]    = ImVec4(0.18f, 0.40f, 0.75f, 1.0f);
    c[ImGuiCol_Separator]      = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_PlotHistogram]  = kAccent;
}

static bool toggleBtn(const char* label, bool* v, float w) {
    bool on = *v;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.62f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.85f, 1.0f));
    }
    bool pressed = ImGui::Button(label, ImVec2(w, 0));
    if (on) ImGui::PopStyleColor(3);
    if (pressed) *v = !*v;
    return pressed;
}

int main(int argc, char** argv) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(1366, 768, "LitePainter", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "Gagal buat window (butuh OpenGL 3.3)\n"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetDropCallback(win, dropCallback);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io0 = ImGui::GetIO();
        io0.IniFilename = nullptr;
        ImFontConfig fc; fc.SizePixels = 16.0f;
        io0.Fonts->AddFontDefault(&fc);
    }
    applyStyle();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    GLuint meshProg = makeProgram(kMeshVS, kMeshFS);
    GLuint lineProg = makeProgram(kLineVS, kLineFS);
    GLuint paintProg = makeProgram(kPaintVS, kPaintFS);
    GLuint paintMaskProg = makeProgram(kPaintVS, kPaintMaskFS);
    GLuint gbufProg = makeProgram(kPaintVS, kGBufFS);

    Mesh mesh; Camera cam; Canvas canvas;
    canvas.maskProg = makeProgram(kPaintVS, kMaskFS);
    canvas.compProg = makeProgram(kFullVS, kCompFS);
    canvas.dilProg = makeProgram(kFullVS, kDilFS);
    glGenVertexArrays(1, &canvas.emptyVao);

    GLuint dummyWhite = makeTex(1, GL_R8, GL_RED, false);
    { unsigned char w = 255; glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &w); }

    std::vector<glm::vec3> gl;
    for (int i = -10; i <= 10; i++) {
        gl.push_back({(float)i, 0, -10}); gl.push_back({(float)i, 0, 10});
        gl.push_back({-10, 0, (float)i}); gl.push_back({10, 0, (float)i});
    }
    GLuint gvao, gvbo;
    glGenVertexArrays(1, &gvao); glGenBuffers(1, &gvbo);
    glBindVertexArray(gvao); glBindBuffer(GL_ARRAY_BUFFER, gvbo);
    glBufferData(GL_ARRAY_BUFFER, gl.size() * sizeof(glm::vec3), gl.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glBindVertexArray(0);

    std::string status = "Drag & drop file .obj ke window, atau File > Open OBJ.";
    bool wire = false, showGrid = true, gridAtBase = true, useAO = true;

    bool paintMode = true, eraser = false, symmetry = false, editMask = false;
    bool chColor = true, chRough = false, chMetal = false;
    bool exportORM = true, exportBake = true;
    float brushColor[3] = {0.85f, 0.2f, 0.2f};
    float valRough = 0.5f, valMetal = 0.0f;
    float baseColor[3] = {0.8f, 0.8f, 0.8f};
    float baseRough = 0.5f, baseMetal = 0.0f;
    float brushSizePct = 5.0f, hardness = 0.5f, opacity = 1.0f;
    const char* sizeNames[3] = {"512", "1024", "2048"};
    const int sizeVals[3] = {512, 1024, 2048};
    const char* blendNames[5] = {"Normal", "Multiply", "Screen", "Overlay", "Add"};
    const char* viewNames[5] = {"Base Color", "PBR", "Roughness", "Metallic", "Bake map"};
    const char* qualNames[3] = {"Rendah (16 sampel)", "Sedang (32 sampel)", "Tinggi (64 sampel)"};
    int sizeIdx = 1, viewMode = 0, bakeQ = 1, previewBake = BK_AO;
    int matSel = 0, smartSel = 0, maskSel = 0, mgSel = 0, customCount = 0;
    std::vector<SmartMaskDef> maskLib = builtinSmartMasks();
    MaskGen mg = maskLib[0].gen;
    bool mgAuto = false, mgDirty = false;
    std::function<void()> afterBake;     // dijalankan otomatis setelah bake selesai
    BakeParams bp;
    canvas.create(sizeVals[sizeIdx]);

    BakeJob job;
    BakeMaps bakeMaps;
    std::vector<Vertex> highVerts;      // high-poly untuk bake Normal/Height
    std::string highName;

    // alat baru: polygon fill, 3D path, ribbon, texture material
    int tool = 0, polyMode = 1;              // tool: 0 Brush, 1 Fill, 2 Path, 3 Ribbon
    float angleDeg = 30.0f;
    BVH pickBvh;
    Topo topo;
    Mesh scratch;
    std::vector<TexMat> texMats;
    int texSel = -1;
    bool useTexBrush = false;
    std::vector<glm::vec3> pathP, pathN;
    int pathSel = -1;
    bool pathDrag = false, pathSpline = true, pathClosed = false;
    int ribbonPattern = 0;
    float ribbonPct = 4.0f, ribbonSoft = 0.15f, ribbonTaper = 0.0f, ribbonDashPct = 8.0f, ribbonGapPct = 6.0f;
    const char* polyNames[5] = {"Triangle", "Polygon", "UV Island", "Mesh Shell", "Area sudut"};
    const char* patNames[2] = {"Solid", "Putus-putus"};

    bool stroke = false, strokeSnap = false, undilated = false;
    glm::vec2 lastMp(0);

    auto bakeStop = [&]() {
        if (job.master.joinable()) { job.cancel = true; job.master.join(); }
        job.running = false; job.done = false;
    };
    auto tryLoad = [&](const std::string& p) {
        if (p.empty()) return;
        bakeStop();
        if (loadOBJ(p, mesh, status)) {
            cam.fit(mesh.bmin, mesh.bmax);
            bakeMaps.clear();
            editMask = false;
            pathP.clear(); pathN.clear(); pathSel = -1; pathDrag = false;
            pickBvh.build(mesh.cpu, true);
            buildTopo(mesh, topo);
            canvas.create(sizeVals[sizeIdx]);
            canvas.buildMask(mesh);
        }
    };
    auto addTexMat = [&](const std::string& p) {
        if (p.empty()) return;
        if (texMats.size() >= 16) { status = "Maksimal 16 texture material."; return; }
        std::string err;
        GLuint t = loadImageTex(p, err);
        if (!t) { status = err; return; }
        TexMat m; m.name = baseName(p); m.c = t; m.cFile = p;
        texMats.push_back(m);
        texSel = (int)texMats.size() - 1;
        status = "Texture material ditambahkan: " + m.name;
    };
    auto setTexMap = [&](int which, const std::string& p) {   // 1 = roughness, 2 = metallic
        if (p.empty() || texSel < 0 || texSel >= (int)texMats.size()) return;
        std::string err;
        GLuint t = loadImageTex(p, err);
        if (!t) { status = err; return; }
        TexMat& m = texMats[texSel];
        GLuint* slot = (which == 1) ? &m.r : &m.m;
        if (*slot) glDeleteTextures(1, slot);
        *slot = t;
        (which == 1 ? m.rFile : m.mFile) = p;
        status = std::string(which == 1 ? "Roughness map: " : "Metallic map: ") + baseName(p);
    };
    auto removeTex = [&]() {
        if (texSel < 0 || texSel >= (int)texMats.size()) return;
        TexMat& m = texMats[texSel];
        if (m.c) glDeleteTextures(1, &m.c);
        if (m.r) glDeleteTextures(1, &m.r);
        if (m.m) glDeleteTextures(1, &m.m);
        texMats.erase(texMats.begin() + texSel);
        texSel = texMats.empty() ? -1 : std::min(texSel, (int)texMats.size() - 1);
    };
    auto loadHigh = [&](const std::string& p) {
        if (p.empty()) return;
        std::vector<Vertex> v; glm::vec3 bmin, bmax; bool uv = false; std::string err;
        if (!parseOBJ(p, v, bmin, bmax, uv, err)) { status = err; return; }
        highVerts.swap(v);
        highName = baseName(p);
        status = "High-poly dimuat: " + highName + " (" + std::to_string(highVerts.size() / 3) + " tris)";
    };
    auto startBake = [&]() {
        if (job.running || !mesh.vao || !mesh.hasUV) return;
        bool anyOn = false;
        for (int i = 0; i < BK_COUNT; i++) anyOn = anyOn || bp.on[i];
        if (!anyOn) { status = "Pilih minimal satu map."; return; }
        if ((bp.on[BK_NRMTAN] || bp.on[BK_HEIGHT]) && highVerts.empty()) { status = "Normal (Tangent) / Height butuh high-poly."; return; }
        bakeStop();
        bp.samples = bakeQ == 0 ? 16 : (bakeQ == 1 ? 32 : 64);
        job.P = bp;
        job.size = canvas.size;
        job.bmin = mesh.bmin; job.bmax = mesh.bmax;
        job.modelRadius = std::max(glm::length(mesh.bmax - mesh.bmin) * 0.5f, 1e-5f);
        renderGBuffer(mesh, gbufProg, job.size, job.pos, job.nrm);
        job.tris = mesh.cpu;
        job.high = highVerts;
        for (int i = 0; i < BK_COUNT; i++) { job.out[i].clear(); job.made[i] = false; }
        job.cancel = false; job.done = false; job.rowsDone = 0; job.stage = -1; job.running = true;
        job.master = std::thread(bakeMain, &job);
        status = "Bake berjalan...";
    };
    auto finishBake = [&]() {
        if (job.master.joinable()) job.master.join();
        job.done = false; job.running = false;
        if (job.cancel) { afterBake = nullptr; status = "Bake dibatalkan."; return; }
        bakeMaps.set(job.size, job);
        job.pos.clear(); job.pos.shrink_to_fit(); job.nrm.clear(); job.nrm.shrink_to_fit();
        job.tris.clear(); job.tris.shrink_to_fit(); job.high.clear(); job.high.shrink_to_fit();
        int first = -1;
        for (int i = 0; i < BK_COUNT; i++) { if (job.made[i]) { first = i; break; } job.out[i].clear(); }
        for (int i = 0; i < BK_COUNT; i++) job.out[i].clear();
        status = "Bake selesai.";
        if (first >= 0 && !afterBake) { previewBake = first; viewMode = 4; }
        if (afterBake) { std::function<void()> f = afterBake; afterBake = nullptr; f(); }
    };
    auto bakeToLayer = [&](int type, int blend, float op, const char* name) {
        if (!bakeMaps.has[type] || bakeMaps.size != canvas.size) { status = "Hasil bake tidak cocok dengan resolusi texture."; return; }
        if (canvas.layers.size() >= Canvas::kMaxLayers) { status = "Layer penuh (maks 8)."; return; }
        canvas.addLayer();
        Layer& L = canvas.layers[canvas.active];
        const std::vector<unsigned char>& src = bakeMaps.px[type];
        size_t cnt = (size_t)canvas.size * canvas.size;
        std::vector<unsigned char> rgba(cnt * 4);
        for (size_t i = 0; i < cnt; i++) {
            rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = src[i]; rgba[4 * i + 3] = 255;
        }
        canvas.writeTex(L.tex, rgba);
        L.blend = blend; L.opacity = op; L.name = name;
        canvas.dirty = true;
        status = std::string("Ditambahkan layer: ") + name;
    };
    auto mapsReady = [&](const std::vector<int>& need) -> bool {
        for (int t : need) if (!bakeMaps.has[t] || bakeMaps.size != canvas.size) return false;
        return true;
    };
    // bake otomatis map yang kurang, lalu jalankan fn setelah selesai
    auto startAutoBake = [&](const std::vector<int>& need, std::function<void()> fn) -> bool {
        if (!mesh.vao || !mesh.hasUV) { status = "Butuh model ber-UV untuk menghitung map bake."; return false; }
        if (job.running) { status = "Bake sedang berjalan. Coba lagi setelah selesai."; return false; }
        BakeParams saved = bp;
        bool needHigh = false;
        for (int i = 0; i < BK_COUNT; i++) bp.on[i] = false;
        std::string names;
        for (int t : need) {
            if (mapsReady(std::vector<int>{t})) continue;
            bp.on[t] = true;
            if (t == BK_NRMTAN || t == BK_HEIGHT) needHigh = true;
            if (!names.empty()) names += ", ";
            names += kBakeShort[t];
        }
        if (needHigh && highVerts.empty()) { bp = saved; status = "Map Height / Normal butuh high-poly (tab Bake)."; return false; }
        afterBake = fn;
        startBake();
        bp = saved;
        if (!job.running) { afterBake = nullptr; return false; }
        status = "Menghitung map: " + names + " ... efek diterapkan otomatis setelah selesai.";
        return true;
    };
    auto applyMaskGen = [&](Layer& L, const MaskGen& g) -> bool {
        std::vector<unsigned char> m;
        if (!genMaskStack(bakeMaps, canvas.size, g, m)) return false;
        canvas.ensureMask(L, 1.0f);
        canvas.writeMask(L, m);
        return true;
    };
    // pasang mask (smart mask / hasil editor) ke layer; map yang kurang dihitung otomatis
    auto requestMask = [&](int layerId, const MaskGen& g, const std::string& label) {
        std::vector<int> need;
        maskGenNeeds(g, need);
        std::function<void()> run = [&, layerId, g, label]() {
            Layer* L = canvas.findLayer(layerId);
            if (L && applyMaskGen(*L, g)) status = "Mask diterapkan: " + label;
        };
        if (mapsReady(need)) run(); else startAutoBake(need, run);
    };
    auto applyMaterial = [&](Layer& L, int mi) {
        const Material& m = kMats[mi];
        const float col[4] = {m.c[0], m.c[1], m.c[2], 1.0f};
        const float pbr[4] = {m.rough, 1.0f, m.metal, 1.0f};
        canvas.clearChannels(L, true, true, true, col, pbr);
    };
    auto applySmartNow = [&](int si) {
        const SmartMat& sm = kSmart[si];
        if (canvas.layers.size() + (size_t)sm.count > Canvas::kMaxLayers) {
            status = "Layer tidak cukup (butuh " + std::to_string(sm.count) + ", maks 8)."; return;
        }
        for (int i = 0; i < sm.count; i++) {
            const SmartLayer& sl = sm.L[i];
            canvas.addLayer();
            Layer& L = canvas.layers[canvas.active];
            applyMaterial(L, sl.mat);
            L.name = sl.name; L.opacity = sl.opacity; L.blend = sl.blend;
            if (sl.mask >= 0 && sl.mask < (int)maskLib.size()) {
                MaskGen g = maskLib[sl.mask].gen;
                if (sl.inv) g.invertAll = !g.invertAll;
                applyMaskGen(L, g);
            }
        }
        editMask = false;
        status = std::string("Smart material diterapkan: ") + sm.name;
    };
    auto requestSmart = [&](int si) {
        const SmartMat& sm = kSmart[si];
        if (canvas.layers.size() + (size_t)sm.count > Canvas::kMaxLayers) {
            status = "Layer tidak cukup (butuh " + std::to_string(sm.count) + ", maks 8). Hapus beberapa layer."; return;
        }
        std::vector<int> need;
        smartNeeds(maskLib, si, need);
        if (mapsReady(need)) applySmartNow(si);
        else startAutoBake(need, [&, si]() { applySmartNow(si); });
    };
    auto doExport = [&]() {
        std::string path = saveFileDialog();
        if (path.empty()) return;
        canvas.compose(baseColor, baseRough, baseMetal, true);
        undilated = false;
        std::vector<unsigned char> rgb, rough, metal;
        canvas.readFinal(rgb, rough, metal);
        std::string stem = stripPng(path);
        int sz = canvas.size;
        stbi_flip_vertically_on_write(1);
        bool ok = stbi_write_png((stem + "_BaseColor.png").c_str(), sz, sz, 3, rgb.data(), sz * 3) != 0;
        ok = ok && stbi_write_png((stem + "_Roughness.png").c_str(), sz, sz, 1, rough.data(), sz) != 0;
        ok = ok && stbi_write_png((stem + "_Metallic.png").c_str(), sz, sz, 1, metal.data(), sz) != 0;
        bool bakeFits = bakeMaps.size == sz;
        if (ok && exportORM) {
            std::vector<unsigned char> orm((size_t)sz * sz * 3);
            bool useBakedAO = bakeMaps.has[BK_AO] && bakeFits;
            for (size_t i = 0; i < (size_t)sz * sz; i++) {
                orm[3 * i] = useBakedAO ? bakeMaps.px[BK_AO][i] : 255;
                orm[3 * i + 1] = rough[i]; orm[3 * i + 2] = metal[i];
            }
            ok = stbi_write_png((stem + "_ORM.png").c_str(), sz, sz, 3, orm.data(), sz * 3) != 0;
        }
        if (ok && exportBake && bakeFits) {
            for (int t = 0; t < BK_COUNT; t++) {
                if (!bakeMaps.has[t]) continue;
                ok = ok && stbi_write_png((stem + "_" + kBakeFile[t] + ".png").c_str(), sz, sz, kBakeCh[t],
                                          bakeMaps.px[t].data(), sz * kBakeCh[t]) != 0;
            }
        }
        status = ok ? ("Export selesai: " + stem + "_*.png") : "Gagal menyimpan PNG.";
    };
    if (argc > 1) tryLoad(argv[1]);

    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (!g_dropped.empty()) {
            std::string ext;
            size_t dot = g_dropped.find_last_of('.');
            if (dot != std::string::npos) { ext = g_dropped.substr(dot); for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch); }
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") addTexMat(g_dropped);
            else tryLoad(g_dropped);
            g_dropped.clear();
        }
        if (job.done.load()) finishBake();

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // ---- layout ----
        const float winW = io.DisplaySize.x, winH = io.DisplaySize.y;
        const float menuH = ImGui::GetFrameHeight();
        const float leftW = 340.0f, rightW = 280.0f, statusH = 30.0f;
        const float sx = io.DisplayFramebufferScale.x, sy = io.DisplayFramebufferScale.y;
        int fbw, fbh; glfwGetFramebufferSize(win, &fbw, &fbh);
        const int vpx = (int)(leftW * sx), vpy = (int)(statusH * sy);
        const int vpw = std::max(1, (int)((winW - leftW - rightW) * sx));
        const int vph = std::max(1, (int)((winH - menuH - statusH) * sy));
        bool mouseInView = ImGui::IsMousePosValid() &&
                           io.MousePos.x >= leftW && io.MousePos.x <= winW - rightW &&
                           io.MousePos.y >= menuH && io.MousePos.y <= winH - statusH;

        // ---- kamera ----
        if (!io.WantCaptureMouse) {
            bool l = ImGui::IsMouseDown(0), m = ImGui::IsMouseDown(2), r = ImGui::IsMouseDown(1);
            bool shift = io.KeyShift;
            bool orbitMode = !paintMode || io.KeyAlt;
            if (l && orbitMode && !shift) {
                cam.yaw -= io.MouseDelta.x * 0.01f;
                cam.pitch += io.MouseDelta.y * 0.01f;
                cam.pitch = glm::clamp(cam.pitch, -1.55f, 1.55f);
            }
            if (m || r || (l && shift)) {
                glm::mat4 v = cam.view();
                glm::vec3 right(v[0][0], v[1][0], v[2][0]), up(v[0][1], v[1][1], v[2][1]);
                float s = cam.dist * 0.0015f;
                cam.target += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * s;
            }
            if (io.MouseWheel != 0 && mouseInView) cam.dist *= (io.MouseWheel > 0) ? 0.9f : 1.1f;
            cam.dist = std::max(cam.dist, 0.01f);
        }

        float aspect = (float)vpw / (float)vph;
        float nearP = std::max(cam.dist * 0.01f, 0.001f);
        const float fov = glm::radians(45.0f);
        glm::mat4 view = cam.view();
        glm::mat4 proj = glm::perspective(fov, aspect, nearP, cam.dist * 100.0f);
        glm::mat4 mvp = proj * view;

        auto pickAt = [&](glm::vec2 m) -> Hit {
            glm::vec4 vp((float)vpx, (float)vpy, (float)vpw, (float)vph);
            glm::vec3 a = glm::unProject(glm::vec3(m.x, fbh - m.y, 0.0f), view, proj, vp);
            glm::vec3 b = glm::unProject(glm::vec3(m.x, fbh - m.y, 1.0f), view, proj, vp);
            return pickBVH(pickBvh, a, glm::normalize(b - a));
        };

        // ---- shortcut ----
        if (!io.WantTextInput) {
            if (io.KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) canvas.redo(); else canvas.undo(); }
                if (ImGui::IsKeyPressed(ImGuiKey_Y)) canvas.redo();
                if (ImGui::IsKeyPressed(ImGuiKey_O, false)) tryLoad(openFileDialog());
            } else {
                if (ImGui::IsKeyPressed(ImGuiKey_E, false)) eraser = !eraser;
                if (ImGui::IsKeyPressed(ImGuiKey_B, false)) tool = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_G, false)) tool = 1;
                if (ImGui::IsKeyPressed(ImGuiKey_P, false)) tool = 2;
                if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool = 3;
                if (ImGui::IsKeyPressed(ImGuiKey_X, false)) symmetry = !symmetry;
                if (ImGui::IsKeyPressed(ImGuiKey_F, false) && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) brushSizePct = std::max(0.2f, brushSizePct * 0.85f);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) brushSizePct = std::min(30.0f, brushSizePct * 1.18f);
            }
        }

        // ---- painting & tools ----
        if (canvas.layers.empty()) canvas.addLayer();
        canvas.active = glm::clamp(canvas.active, 0, (int)canvas.layers.size() - 1);
        bool paintingMask = editMask && canvas.layers[canvas.active].hasMask;
        float modelRadius = glm::length(mesh.bmax - mesh.bmin) * 0.5f;
        float radiusWorld = std::max(brushSizePct / 100.0f * modelRadius, 1e-5f);
        float ribbonRadius = std::max(ribbonPct / 100.0f * modelRadius, 1e-5f);
        bool anyChannel = chColor || chRough || chMetal;
        bool meshOK = paintMode && mesh.vao && mesh.hasUV && !io.KeyAlt && !io.KeyShift;
        bool layerReady = (paintingMask || anyChannel) && canvas.layers[canvas.active].visible;
        bool canPaint = meshOK && layerReady;
        bool mouseOk = mouseInView && !io.WantCaptureMouse;
        glm::vec2 mp(io.MousePos.x * sx, io.MousePos.y * sy);
        if (tool != 0) stroke = false;
        if (tool < 2) pathDrag = false;

        auto toScreen = [&](const glm::vec3& w, ImVec2& out) -> bool {
            glm::vec3 s = glm::project(w, view, proj, glm::vec4((float)vpx, (float)vpy, (float)vpw, (float)vph));
            if (s.z < 0.0f || s.z > 1.0f) return false;
            out = ImVec2(s.x / sx, ((float)fbh - s.y) / sy);
            return true;
        };

        float curRadius = (tool == 3) ? ribbonRadius : radiusWorld;
        float distRef = cam.dist;
        if ((canPaint || (meshOK && tool >= 2)) && mouseOk) {
            Hit hv = pickAt(mp);
            if (hv.ok) distRef = glm::length(hv.p - cam.eye());
        }
        float brushPx = curRadius * ((float)vph * 0.5f) / (std::max(distRef, 1e-4f) * std::tan(fov * 0.5f));

        // set texture material ke program cat (unit 5,6,7)
        auto bindMat = [&](GLuint prog, bool useTex) {
            bool hc = false, hr = false, hm = false;
            float tile = 1.0f;
            GLuint tc = dummyWhite, tr = dummyWhite, tm = dummyWhite;
            if (useTex && texSel >= 0 && texSel < (int)texMats.size()) {
                const TexMat& t = texMats[texSel];
                if (t.c) { hc = true; tc = t.c; }
                if (t.r) { hr = true; tr = t.r; }
                if (t.m) { hm = true; tm = t.m; }
                tile = t.tile;
            }
            glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, tc);
            glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, tr);
            glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, tm);
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(glGetUniformLocation(prog, "uMatC"), 5);
            glUniform1i(glGetUniformLocation(prog, "uMatR"), 6);
            glUniform1i(glGetUniformLocation(prog, "uMatM"), 7);
            glUniform1i(glGetUniformLocation(prog, "uHasC"), hc ? 1 : 0);
            glUniform1i(glGetUniformLocation(prog, "uHasR"), hr ? 1 : 0);
            glUniform1i(glGetUniformLocation(prog, "uHasM"), hm ? 1 : 0);
            glUniform1f(glGetUniformLocation(prog, "uTile"), tile);
        };

        auto paintStamp = [&](const glm::vec3& p, const glm::vec3& n, float radius, float hard, float opac) {
            Layer& AL = canvas.layers[canvas.active];
            bool pm = editMask && AL.hasMask;
            if (!strokeSnap) { canvas.pushUndo(pm); strokeSnap = true; }
            glViewport(0, 0, canvas.size, canvas.size);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            if (pm) {
                glBindFramebuffer(GL_FRAMEBUFFER, AL.maskFbo);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUseProgram(paintMaskProg);
                glUniform1i(glGetUniformLocation(paintMaskProg, "uFill"), 0);
                glUniform1f(glGetUniformLocation(paintMaskProg, "uRadius"), radius);
                glUniform1f(glGetUniformLocation(paintMaskProg, "uHardness"), std::min(hard, 0.99f));
                glUniform1f(glGetUniformLocation(paintMaskProg, "uOpacity"), opac);
                glUniform1f(glGetUniformLocation(paintMaskProg, "uValue"), eraser ? 0.0f : 1.0f);
                GLint lh = glGetUniformLocation(paintMaskProg, "uHit");
                GLint ln = glGetUniformLocation(paintMaskProg, "uHitN");
                glUniform3f(lh, p.x, p.y, p.z); glUniform3f(ln, n.x, n.y, n.z);
                mesh.draw();
                if (symmetry) { glUniform3f(lh, -p.x, p.y, p.z); glUniform3f(ln, -n.x, n.y, n.z); mesh.draw(); }
            } else {
                glBindFramebuffer(GL_FRAMEBUFFER, AL.fbo);
                if (eraser) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
                else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glColorMaski(0, (GLboolean)chColor, (GLboolean)chColor, (GLboolean)chColor, (GLboolean)chColor);
                glColorMaski(1, (GLboolean)chRough, (GLboolean)chRough, (GLboolean)chMetal, (GLboolean)chMetal);
                glUseProgram(paintProg);
                glUniform1i(glGetUniformLocation(paintProg, "uFill"), 0);
                glUniform3fv(glGetUniformLocation(paintProg, "uColor"), 1, brushColor);
                glUniform1f(glGetUniformLocation(paintProg, "uRough"), valRough);
                glUniform1f(glGetUniformLocation(paintProg, "uMetal"), valMetal);
                glUniform1f(glGetUniformLocation(paintProg, "uRadius"), radius);
                glUniform1f(glGetUniformLocation(paintProg, "uHardness"), std::min(hard, 0.99f));
                glUniform1f(glGetUniformLocation(paintProg, "uOpacity"), opac);
                bindMat(paintProg, useTexBrush);
                GLint locHit = glGetUniformLocation(paintProg, "uHit");
                GLint locN = glGetUniformLocation(paintProg, "uHitN");
                glUniform3f(locHit, p.x, p.y, p.z);
                glUniform3f(locN, n.x, n.y, n.z);
                mesh.draw();
                if (symmetry) {
                    glUniform3f(locHit, -p.x, p.y, p.z);
                    glUniform3f(locN, -n.x, n.y, n.z);
                    mesh.draw();
                }
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            glDisable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            canvas.dirty = true;
        };
        auto stampAt = [&](glm::vec2 m) {
            Hit h = pickAt(m);
            if (h.ok) paintStamp(h.p, h.n, radiusWorld, hardness, opacity);
        };

        // isi sekumpulan segitiga (nullptr = seluruh mesh) dengan warna / texture material
        auto fillTris = [&](const std::vector<int>* tris, bool useTex, bool allowMask, bool erase, float opac) {
            if (!mesh.vao || !mesh.hasUV) return;
            Layer& AL = canvas.layers[canvas.active];
            bool pm = allowMask && editMask && AL.hasMask;
            const Mesh* srcMesh = &mesh;
            if (tris) {
                std::vector<Vertex> sv;
                sv.reserve(tris->size() * 3);
                for (int t : *tris) {
                    sv.push_back(mesh.cpu[3 * t]); sv.push_back(mesh.cpu[3 * t + 1]); sv.push_back(mesh.cpu[3 * t + 2]);
                }
                if (sv.empty()) return;
                scratch.upload(sv);
                srcMesh = &scratch;
            }
            canvas.pushUndo(pm);
            glViewport(0, 0, canvas.size, canvas.size);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            if (pm) {
                glBindFramebuffer(GL_FRAMEBUFFER, AL.maskFbo);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUseProgram(paintMaskProg);
                glUniform1i(glGetUniformLocation(paintMaskProg, "uFill"), 1);
                glUniform1f(glGetUniformLocation(paintMaskProg, "uOpacity"), opac);
                glUniform1f(glGetUniformLocation(paintMaskProg, "uValue"), erase ? 0.0f : 1.0f);
                srcMesh->draw();
            } else {
                glBindFramebuffer(GL_FRAMEBUFFER, AL.fbo);
                if (erase) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
                else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glColorMaski(0, (GLboolean)chColor, (GLboolean)chColor, (GLboolean)chColor, (GLboolean)chColor);
                glColorMaski(1, (GLboolean)chRough, (GLboolean)chRough, (GLboolean)chMetal, (GLboolean)chMetal);
                glUseProgram(paintProg);
                glUniform1i(glGetUniformLocation(paintProg, "uFill"), 1);
                glUniform3fv(glGetUniformLocation(paintProg, "uColor"), 1, brushColor);
                glUniform1f(glGetUniformLocation(paintProg, "uRough"), valRough);
                glUniform1f(glGetUniformLocation(paintProg, "uMetal"), valMetal);
                glUniform1f(glGetUniformLocation(paintProg, "uRadius"), 1.0f);
                glUniform1f(glGetUniformLocation(paintProg, "uHardness"), 0.0f);
                glUniform1f(glGetUniformLocation(paintProg, "uOpacity"), opac);
                glUniform3f(glGetUniformLocation(paintProg, "uHit"), 0.0f, 0.0f, 0.0f);
                glUniform3f(glGetUniformLocation(paintProg, "uHitN"), 0.0f, 1.0f, 0.0f);
                bindMat(paintProg, useTex);
                srcMesh->draw();
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }
            glDisable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            canvas.dirty = true;
        };

        // sampel sepanjang path (spline Catmull-Rom), opsional diproyeksikan ke permukaan model
        auto buildPathSamples = [&](float step, bool project, std::vector<glm::vec3>& P, std::vector<glm::vec3>& N,
                                    std::vector<float>& S, float& Ltot) {
            P.clear(); N.clear(); S.clear(); Ltot = 0.0f;
            int n = (int)pathP.size();
            if (n < 2) return;
            bool closed = pathClosed && n >= 3;
            int segs = closed ? n : n - 1;
            auto idxp = [&](int i) -> int { return closed ? ((i % n) + n) % n : std::max(0, std::min(n - 1, i)); };
            std::vector<glm::vec3> dp, dn;
            const int M = 16;
            for (int s = 0; s < segs; s++) {
                glm::vec3 p0 = pathP[idxp(s - 1)], p1 = pathP[idxp(s)], p2 = pathP[idxp(s + 1)], p3 = pathP[idxp(s + 2)];
                glm::vec3 n1 = pathN[idxp(s)], n2 = pathN[idxp(s + 1)];
                for (int j = 0; j < M; j++) {
                    float t = (float)j / (float)M;
                    glm::vec3 pos;
                    if (pathSpline) {
                        float t2 = t * t, t3 = t2 * t;
                        pos = 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                                      (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
                    } else pos = glm::mix(p1, p2, t);
                    glm::vec3 nn = n1 * (1.0f - t) + n2 * t;
                    float ln = glm::length(nn);
                    nn = ln > 1e-6f ? nn / ln : n1;
                    dp.push_back(pos); dn.push_back(nn);
                }
            }
            if (!closed) { dp.push_back(pathP[n - 1]); dn.push_back(pathN[n - 1]); }
            else { dp.push_back(dp[0]); dn.push_back(dn[0]); }
            std::vector<float> cum(dp.size(), 0.0f);
            for (size_t i = 1; i < dp.size(); i++) cum[i] = cum[i - 1] + glm::length(dp[i] - dp[i - 1]);
            Ltot = cum.back();
            if (Ltot < 1e-8f) return;
            int count = std::max(2, std::min(4000, (int)(Ltot / std::max(step, 1e-9f)) + 1));
            size_t seg = 0;
            for (int k = 0; k < count; k++) {
                float d = Ltot * (float)k / (float)(count - 1);
                while (seg + 2 < cum.size() && cum[seg + 1] < d) seg++;
                float span = cum[seg + 1] - cum[seg];
                float f = span > 1e-9f ? glm::clamp((d - cum[seg]) / span, 0.0f, 1.0f) : 0.0f;
                glm::vec3 p = glm::mix(dp[seg], dp[seg + 1], f);
                glm::vec3 nn = glm::mix(dn[seg], dn[seg + 1], f);
                float ln = glm::length(nn);
                nn = ln > 1e-6f ? nn / ln : glm::vec3(0, 1, 0);
                if (project && !pickBvh.nodes.empty()) {
                    float eps = modelRadius * 0.001f, maxT = modelRadius * 0.6f;
                    Hit h1 = pickBVH(pickBvh, p + nn * eps, -nn);
                    Hit h2 = pickBVH(pickBvh, p + nn * eps, nn);
                    bool ok1 = h1.ok && h1.t <= maxT, ok2 = h2.ok && h2.t <= maxT;
                    if (ok1 && (!ok2 || h1.t <= h2.t)) { p = h1.p; nn = h1.n; }
                    else if (ok2) { p = h2.p; nn = h2.n; }
                }
                P.push_back(p); N.push_back(nn); S.push_back(d / Ltot);
            }
        };

        // cat sepanjang path (Path = brush biasa, Ribbon = pita dengan lebar sendiri)
        auto applyPath = [&]() {
            if (pathP.size() < 2) { status = "Path butuh minimal 2 titik."; return; }
            if (!(mesh.vao && mesh.hasUV)) { status = "Butuh model ber-UV."; return; }
            if (!layerReady) { status = "Pilih kanal cat dan pastikan layer terlihat."; return; }
            bool rib = (tool == 3);
            float radius = rib ? ribbonRadius : radiusWorld;
            float step = std::max(radius * 0.25f, modelRadius * 0.0005f);
            std::vector<glm::vec3> P, N; std::vector<float> S; float Lt = 0.0f;
            buildPathSamples(step, true, P, N, S, Lt);
            if (P.empty()) { status = "Path kosong."; return; }
            float hard = rib ? std::min(1.0f - ribbonSoft, 0.99f) : hardness;
            float opac = rib ? 1.0f : opacity;
            float dashLen = std::max(ribbonDashPct / 100.0f * modelRadius, 1e-5f);
            float gapLen = std::max(ribbonGapPct / 100.0f * modelRadius, 1e-5f);
            strokeSnap = false;
            int stamps = 0;
            for (size_t k = 0; k < P.size(); k++) {
                float s = S[k], dist = s * Lt;
                if (rib && ribbonPattern == 1) {
                    float period = dashLen + gapLen;
                    if (std::fmod(dist, period) > dashLen) continue;
                }
                float rad = radius;
                if (ribbonTaper > 0.0f) {
                    float e = glm::clamp(std::min(s, 1.0f - s) / ribbonTaper, 0.0f, 1.0f);
                    e = e * e * (3.0f - 2.0f * e);
                    rad = radius * glm::mix(0.15f, 1.0f, e);
                }
                paintStamp(P[k], N[k], rad, hard, opac);
                stamps++;
            }
            status = std::string(rib ? "Ribbon" : "Path") + " diterapkan (" + std::to_string(stamps) + " stamp)";
        };

        // ---- Alat: Brush ----
        if (tool == 0) {
            if (canPaint && mouseOk && ImGui::IsMouseClicked(0)) {
                stroke = true; strokeSnap = false; lastMp = mp;
                stampAt(mp);
            } else if (stroke) {
                if (ImGui::IsMouseDown(0) && canPaint) {
                    glm::vec2 dlt = mp - lastMp;
                    float len = glm::length(dlt);
                    float step = std::max(2.0f, brushPx * 0.25f);
                    if (len >= step) {
                        glm::vec2 dir = dlt / len;
                        int n = std::min((int)(len / step), 64);
                        for (int i = 1; i <= n; i++) stampAt(lastMp + dir * (step * (float)i));
                        lastMp += dir * (step * (float)n);
                    }
                } else { stroke = false; canvas.dirty = true; }
            }
        }
        // ---- Alat: Polygon Fill ----
        else if (tool == 1) {
            if (canPaint && mouseOk && ImGui::IsMouseClicked(0)) {
                Hit h = pickAt(mp);
                if (h.ok && h.tri >= 0) {
                    std::vector<int> sel;
                    selectTris(mesh, topo, polyMode, h.tri, angleDeg, sel);
                    if (!sel.empty()) {
                        fillTris(&sel, useTexBrush, true, eraser, opacity);
                        status = "Fill: " + std::to_string(sel.size()) + " segitiga";
                    }
                }
            }
        }
        // ---- Alat: 3D Path / Ribbon ----
        else {
            if (meshOK && mouseOk && ImGui::IsMouseClicked(0)) {
                int nearIdx = -1; float bestD = 14.0f;
                for (int i = 0; i < (int)pathP.size(); i++) {
                    ImVec2 sp;
                    if (toScreen(pathP[i], sp)) {
                        float d = std::sqrt((sp.x - io.MousePos.x) * (sp.x - io.MousePos.x) + (sp.y - io.MousePos.y) * (sp.y - io.MousePos.y));
                        if (d < bestD) { bestD = d; nearIdx = i; }
                    }
                }
                if (nearIdx >= 0) { pathSel = nearIdx; pathDrag = true; }
                else {
                    Hit h = pickAt(mp);
                    if (h.ok) { pathP.push_back(h.p); pathN.push_back(h.n); pathSel = (int)pathP.size() - 1; }
                }
            }
            if (pathDrag) {
                if (ImGui::IsMouseDown(0) && pathSel >= 0 && pathSel < (int)pathP.size()) {
                    Hit h = pickAt(mp);
                    if (h.ok) { pathP[pathSel] = h.p; pathN[pathSel] = h.n; }
                } else pathDrag = false;
            }
            if (!io.WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) applyPath();
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                    if (!pathP.empty()) {
                        int k = (pathSel >= 0 && pathSel < (int)pathP.size()) ? pathSel : (int)pathP.size() - 1;
                        pathP.erase(pathP.begin() + k); pathN.erase(pathN.begin() + k);
                        pathSel = (int)pathP.size() - 1;
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { pathP.clear(); pathN.clear(); pathSel = -1; }
            }
        }

        // ---- kursor & overlay ----
        if (mouseOk && meshOK) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            if (tool == 1) {
                dl->AddLine(ImVec2(io.MousePos.x - 8, io.MousePos.y), ImVec2(io.MousePos.x + 8, io.MousePos.y), IM_COL32(255, 255, 255, 230), 1.5f);
                dl->AddLine(ImVec2(io.MousePos.x, io.MousePos.y - 8), ImVec2(io.MousePos.x, io.MousePos.y + 8), IM_COL32(255, 255, 255, 230), 1.5f);
            } else if (tool == 0 ? canPaint : true) {
                dl->AddCircle(io.MousePos, brushPx / sx,
                    paintingMask ? IM_COL32(255, 200, 80, 230) : IM_COL32(255, 255, 255, 220), 48, 1.5f);
            }
        }
        if (tool >= 2 && mesh.vao) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->PushClipRect(ImVec2(leftW, menuH), ImVec2(winW - rightW, winH - statusH), true);
            if (pathP.size() >= 2) {
                std::vector<glm::vec3> P, N; std::vector<float> S; float Lt = 0.0f;
                buildPathSamples(std::max(curRadius * 0.5f, modelRadius * 0.01f), true, P, N, S, Lt);
                ImVec2 prev; bool hasPrev = false;
                for (size_t k = 0; k < P.size(); k++) {
                    ImVec2 cur;
                    if (toScreen(P[k] + N[k] * (modelRadius * 0.002f), cur)) {
                        if (hasPrev) dl->AddLine(prev, cur, IM_COL32(255, 200, 60, 230), 2.0f);
                        prev = cur; hasPrev = true;
                    } else hasPrev = false;
                }
            }
            glm::vec3 eyeP = cam.eye();
            for (int i = 0; i < (int)pathP.size(); i++) {
                ImVec2 sp;
                if (!toScreen(pathP[i], sp)) continue;
                glm::vec3 dv = pathP[i] - eyeP;
                float dl2 = glm::length(dv);
                bool visible = true;
                if (dl2 > 1e-6f) {
                    Hit hh = pickBVH(pickBvh, eyeP, dv / dl2);
                    visible = !hh.ok || hh.t >= dl2 - modelRadius * 0.02f;
                }
                ImU32 col = (i == pathSel) ? IM_COL32(255, 140, 30, 255) : IM_COL32(255, 255, 255, visible ? 255 : 90);
                dl->AddCircleFilled(sp, 6.0f, col);
                dl->AddCircle(sp, 6.0f, IM_COL32(0, 0, 0, 255), 16, 1.5f);
            }
            dl->PopClipRect();
        }

        // ================= UI =================
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open OBJ...", "Ctrl+O")) tryLoad(openFileDialog());
                if (ImGui::MenuItem("Load high-poly...")) loadHigh(openFileDialog());
                if (ImGui::MenuItem("Export PNG...")) doExport();
                ImGui::Separator();
                if (ImGui::MenuItem("Keluar")) glfwSetWindowShouldClose(win, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) canvas.undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) canvas.redo();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Grid", nullptr, &showGrid);
                ImGui::MenuItem("Wireframe", nullptr, &wire);
                if (ImGui::MenuItem("Reset kamera", "F") && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Bantuan")) {
                ImGui::TextUnformatted("Klik kiri       : cat");
                ImGui::TextUnformatted("Alt + Kiri      : orbit");
                ImGui::TextUnformatted("Kanan/Tengah    : pan");
                ImGui::TextUnformatted("Shift + Kiri    : pan");
                ImGui::TextUnformatted("Scroll          : zoom");
                ImGui::TextUnformatted("[ ]             : ukuran brush");
                ImGui::TextUnformatted("E / X / F       : eraser / symmetry / frame");
                ImGui::TextUnformatted("Ctrl+Z / Ctrl+Y : undo / redo");
                ImGui::TextUnformatted("Drag layer      : geser urutan layer");
                ImGui::TextUnformatted("B / G / P / R   : Brush / Fill / Path / Ribbon");
                ImGui::TextUnformatted("Path: Enter terapkan, Del hapus titik, Esc bersihkan");
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // ---- panel kiri (tab) ----
        ImGui::SetNextWindowPos(ImVec2(0, menuH));
        ImGui::SetNextWindowSize(ImVec2(leftW, winH - menuH - statusH));
        ImGui::Begin("##left", nullptr, panelFlags);
        ImGui::PushItemWidth(leftW * 0.50f);
        if (ImGui::BeginTabBar("##tabs")) {
            // ------------------------------------------------ Paint
            if (ImGui::BeginTabItem("Paint")) {
                ImGui::SeparatorText("Alat");
                {
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w4 = (ImGui::GetContentRegionAvail().x - sp * 3.0f) / 4.0f;
                    bool t0 = tool == 0, t1 = tool == 1, t2 = tool == 2, t3 = tool == 3;
                    if (toggleBtn("Brush", &t0, w4)) tool = 0;
                    ImGui::SameLine();
                    if (toggleBtn("Fill", &t1, w4)) tool = 1;
                    ImGui::SameLine();
                    if (toggleBtn("Path", &t2, w4)) tool = 2;
                    ImGui::SameLine();
                    if (toggleBtn("Ribbon", &t3, w4)) tool = 3;
                }
                ImGui::TextDisabled("B Brush | G Fill | P Path | R Ribbon");
                ImGui::SeparatorText("Mode");
                ImGui::Checkbox("Mode Paint", &paintMode);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aktif: klik kiri = pakai alat, Alt+Kiri = orbit");
                if (paintingMask) ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1), "Sedang mengedit MASK: putih = tampil, Eraser = sembunyi");
                ImGui::SeparatorText("Kanal yang dicat");
                float sp = ImGui::GetStyle().ItemSpacing.x;
                float bw = (ImGui::GetContentRegionAvail().x - sp * 2.0f) / 3.0f;
                toggleBtn("Color", &chColor, bw); ImGui::SameLine();
                toggleBtn("Rough", &chRough, bw); ImGui::SameLine();
                toggleBtn("Metal", &chMetal, bw);
                if (!anyChannel && !paintingMask) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Pilih minimal 1 kanal.");
                ImGui::SeparatorText("Nilai");
                if (chColor) ImGui::ColorEdit3("Warna", brushColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
                if (chRough) ImGui::SliderFloat("Roughness", &valRough, 0.0f, 1.0f, "%.2f");
                if (chMetal) ImGui::SliderFloat("Metallic", &valMetal, 0.0f, 1.0f, "%.2f");
                if (!texMats.empty()) {
                    ImGui::Checkbox("Pakai texture material", &useTexBrush);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush / Fill / Path memakai texture material terpilih (tab Material)");
                }
                if (tool == 0 || tool == 2) {
                    ImGui::SeparatorText("Brush");
                    ImGui::SliderFloat("Ukuran", &brushSizePct, 0.2f, 30.0f, "%.1f %%");
                    ImGui::SliderFloat("Hardness", &hardness, 0.0f, 0.99f, "%.2f");
                    ImGui::SliderFloat("Opacity", &opacity, 0.05f, 1.0f, "%.2f");
                } else if (tool == 1) {
                    ImGui::SliderFloat("Opacity", &opacity, 0.05f, 1.0f, "%.2f");
                }
                float bw2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                toggleBtn("Eraser (E)", &eraser, bw2); ImGui::SameLine();
                toggleBtn("Symmetry X (X)", &symmetry, bw2);

                if (tool == 1) {
                    ImGui::SeparatorText("Polygon Fill");
                    ImGui::Combo("Area", &polyMode, polyNames, 5);
                    if (polyMode == 4) ImGui::SliderFloat("Sudut maks", &angleDeg, 1.0f, 89.0f, "%.0f deg");
                    ImGui::TextWrapped("Klik model untuk mengisi area. Memakai warna, kanal, dan texture material yang aktif.");
                    if (polyMode == 1 && mesh.vao && mesh.poly.empty()) ImGui::TextDisabled("Info polygon tidak tersedia.");
                    if (symmetry) ImGui::TextDisabled("Symmetry tidak berlaku untuk Fill.");
                }
                if (tool >= 2) {
                    ImGui::SeparatorText(tool == 2 ? "3D Path" : "Ribbon");
                    ImGui::Text("Titik: %d", (int)pathP.size());
                    ImGui::Checkbox("Spline halus", &pathSpline);
                    ImGui::SameLine();
                    ImGui::Checkbox("Tutup loop", &pathClosed);
                    if (tool == 3) {
                        ImGui::SliderFloat("Lebar", &ribbonPct, 0.3f, 30.0f, "%.1f %%");
                        ImGui::SliderFloat("Lembut tepi", &ribbonSoft, 0.0f, 0.9f, "%.2f");
                        ImGui::Combo("Pola", &ribbonPattern, patNames, 2);
                        if (ribbonPattern == 1) {
                            ImGui::SliderFloat("Panjang garis", &ribbonDashPct, 1.0f, 50.0f, "%.0f %%");
                            ImGui::SliderFloat("Jarak", &ribbonGapPct, 1.0f, 50.0f, "%.0f %%");
                        }
                    }
                    ImGui::SliderFloat("Taper ujung", &ribbonTaper, 0.0f, 0.5f, "%.2f");
                    if (ImGui::Button("Terapkan (Enter)", ImVec2(-1, 32))) applyPath();
                    float bw3 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                    if (ImGui::Button("Hapus titik (Del)", ImVec2(bw3, 0))) {
                        if (!pathP.empty()) {
                            int k = (pathSel >= 0 && pathSel < (int)pathP.size()) ? pathSel : (int)pathP.size() - 1;
                            pathP.erase(pathP.begin() + k); pathN.erase(pathN.begin() + k);
                            pathSel = (int)pathP.size() - 1;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Bersihkan (Esc)", ImVec2(bw3, 0))) { pathP.clear(); pathN.clear(); pathSel = -1; }
                    ImGui::TextWrapped("Klik model = tambah titik. Seret titik = geser. Garis kuning = hasil (mengikuti permukaan).%s",
                                       tool == 3 ? " Ribbon selalu opacity 1." : "");
                }
                ImGui::EndTabItem();
            }
            // ------------------------------------------------ Material (material, smart material, mask)
            if (ImGui::BeginTabItem("Material")) {
                ImGui::SeparatorText("Material");
                if (ImGui::BeginChild("##mats", ImVec2(0, 150), true)) {
                    for (int i = 0; i < M_COUNT; i++) {
                        ImGui::PushID(i);
                        ImGui::ColorButton("##sw", ImVec4(kMats[i].c[0], kMats[i].c[1], kMats[i].c[2], 1.0f),
                                           ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
                        ImGui::SameLine();
                        if (ImGui::Selectable(kMats[i].name, matSel == i)) matSel = i;
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                {
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                    if (ImGui::Button("Isi layer aktif", ImVec2(w2, 0))) {
                        canvas.pushUndo(false);
                        applyMaterial(canvas.layers[canvas.active], matSel);
                        status = std::string("Material: ") + kMats[matSel].name;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Layer baru", ImVec2(w2, 0))) {
                        if (canvas.layers.size() >= Canvas::kMaxLayers) status = "Layer penuh (maks 8).";
                        else { canvas.addLayer(); applyMaterial(canvas.layers[canvas.active], matSel); canvas.layers[canvas.active].name = kMats[matSel].name; }
                    }
                }

                ImGui::SeparatorText("Texture Material");
                if (ImGui::BeginChild("##tmats", ImVec2(0, 100), true)) {
                    if (texMats.empty()) ImGui::TextDisabled("Belum ada. Klik Tambah atau drag gambar ke window.");
                    for (int i = 0; i < (int)texMats.size(); i++) {
                        ImGui::PushID(2000 + i);
                        ImGui::Image((ImTextureID)(intptr_t)texMats[i].c, ImVec2(22, 22), ImVec2(0, 1), ImVec2(1, 0));
                        ImGui::SameLine();
                        if (ImGui::Selectable(texMats[i].name.c_str(), texSel == i)) texSel = i;
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                {
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                    if (ImGui::Button("Tambah gambar...", ImVec2(w2, 0))) addTexMat(openImageDialog());
                    ImGui::SameLine();
                    bool noSel = texSel < 0 || texSel >= (int)texMats.size();
                    if (noSel) ImGui::BeginDisabled();
                    if (ImGui::Button("Hapus", ImVec2(w2, 0))) removeTex();
                    if (noSel) ImGui::EndDisabled();
                    if (!noSel && texSel >= 0 && texSel < (int)texMats.size()) {
                        TexMat& tm = texMats[texSel];
                        ImGui::SliderFloat("Tiling", &tm.tile, 0.1f, 20.0f, "%.1f");
                        ImGui::TextDisabled("Color: %s", baseName(tm.cFile).c_str());
                        ImGui::TextDisabled("Roughness: %s", tm.r ? baseName(tm.rFile).c_str() : "(nilai slider)");
                        ImGui::TextDisabled("Metallic: %s", tm.m ? baseName(tm.mFile).c_str() : "(nilai slider)");
                        if (ImGui::Button("Set Roughness...", ImVec2(w2, 0))) setTexMap(1, openImageDialog());
                        ImGui::SameLine();
                        if (ImGui::Button("Set Metallic...", ImVec2(w2, 0))) setTexMap(2, openImageDialog());
                        ImGui::Checkbox("Pakai di brush / fill / path", &useTexBrush);
                        if (ImGui::Button("Isi layer aktif", ImVec2(w2, 0))) {
                            if (!mesh.vao || !mesh.hasUV) status = "Butuh model ber-UV.";
                            else { fillTris(nullptr, true, false, false, 1.0f); status = std::string("Texture material: ") + tm.name; }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Layer baru", ImVec2(w2, 0))) {
                            if (!mesh.vao || !mesh.hasUV) status = "Butuh model ber-UV.";
                            else if (canvas.layers.size() >= Canvas::kMaxLayers) status = "Layer penuh (maks 8).";
                            else {
                                canvas.addLayer();
                                canvas.layers[canvas.active].name = tm.name;
                                fillTris(nullptr, true, false, false, 1.0f);
                            }
                        }
                        ImGui::TextDisabled("Mengisi kanal yang dicentang di tab Paint.");
                    }
                }

                ImGui::SeparatorText("Smart Material");
                if (ImGui::BeginChild("##smart", ImVec2(0, 140), true)) {
                    for (int i = 0; i < kSmartCount; i++) if (ImGui::Selectable(kSmart[i].name, smartSel == i)) smartSel = i;
                }
                ImGui::EndChild();
                smartSel = glm::clamp(smartSel, 0, kSmartCount - 1);
                ImGui::TextWrapped("%s", kSmart[smartSel].desc);
                {
                    std::vector<int> need;
                    smartNeeds(maskLib, smartSel, need);
                    std::string s;
                    for (int t : need) {
                        if (!s.empty()) s += ", ";
                        s += kBakeShort[t];
                        if (bakeMaps.has[t] && bakeMaps.size == canvas.size) s += " (siap)";
                    }
                    ImGui::TextDisabled("Butuh map: %s", s.empty() ? "-" : s.c_str());
                }
                if (ImGui::Button("Terapkan Smart Material", ImVec2(-1, 32))) requestSmart(smartSel);
                ImGui::TextDisabled("Map yang belum ada dihitung otomatis.");

                ImGui::SeparatorText("Mask layer aktif");
                {
                    Layer& L = canvas.layers[canvas.active];
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                    ImGui::Text("Layer: %s", L.name.c_str());
                    if (!L.hasMask) {
                        if (ImGui::Button("Mask putih", ImVec2(w2, 0))) canvas.ensureMask(L, 1.0f);
                        ImGui::SameLine();
                        if (ImGui::Button("Mask hitam", ImVec2(w2, 0))) canvas.ensureMask(L, 0.0f);
                    } else {
                        if (ImGui::Checkbox("Mask aktif", &L.maskOn)) canvas.dirty = true;
                        if (ImGui::Button("Isi putih", ImVec2(w2, 0))) { canvas.pushUndo(true); canvas.fillMask(L, 1.0f); }
                        ImGui::SameLine();
                        if (ImGui::Button("Isi hitam", ImVec2(w2, 0))) { canvas.pushUndo(true); canvas.fillMask(L, 0.0f); }
                        if (ImGui::Button("Invert", ImVec2(w2, 0))) { canvas.pushUndo(true); canvas.invertMask(L); }
                        ImGui::SameLine();
                        if (ImGui::Button("Hapus mask", ImVec2(w2, 0))) { canvas.removeMask(L); editMask = false; }
                    }
                }
                ImGui::TextDisabled("Smart Mask dan generator ada di tab Mask.");
                ImGui::EndTabItem();
            }
            // ------------------------------------------------ Mask (Smart Mask + MG Mask Editor)
            if (ImGui::BeginTabItem("Mask")) {
                ImGui::SeparatorText("Smart Masks");
                ImGui::TextDisabled("Tarik ke sebuah layer di panel kanan, atau pilih lalu Terapkan.");
                if (ImGui::BeginChild("##smasks", ImVec2(0, 150), true)) {
                    for (int i = 0; i < (int)maskLib.size(); i++) {
                        ImGui::PushID(3000 + i);
                        if (ImGui::Selectable(maskLib[i].name.c_str(), maskSel == i)) { maskSel = i; mg = maskLib[i].gen; mgSel = 0; }
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                            ImGui::SetDragDropPayload("SMASK", &i, sizeof(int));
                            ImGui::Text("Smart Mask: %s", maskLib[i].name.c_str());
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", maskLib[i].desc.c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                maskSel = glm::clamp(maskSel, 0, (int)maskLib.size() - 1);
                ImGui::TextWrapped("%s", maskLib[maskSel].desc.c_str());
                {
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                    if (ImGui::Button("Terapkan ke layer aktif", ImVec2(w2, 0)))
                        requestMask(canvas.layers[canvas.active].id, mg, maskLib[maskSel].name);
                    ImGui::SameLine();
                    if (ImGui::Button("Simpan sebagai preset", ImVec2(w2, 0))) {
                        SmartMaskDef d; d.name = "Custom " + std::to_string(++customCount); d.desc = "Preset buatan sendiri";
                        d.gen = mg; d.builtin = false;
                        maskLib.push_back(d);
                        maskSel = (int)maskLib.size() - 1;
                    }
                }

                ImGui::SeparatorText("MG Mask Editor");
                if (ImGui::BeginChild("##stages", ImVec2(0, 110), true)) {
                    for (int k = 0; k < (int)mg.st.size(); k++) {
                        ImGui::PushID(4000 + k);
                        if (ImGui::Checkbox("##en", &mg.st[k].enabled)) mgDirty = true;
                        ImGui::SameLine();
                        std::string lab = std::to_string(k + 1) + ". " + kGenNames[mg.st[k].type];
                        if (k > 0) lab += std::string("  [") + kOpNames[mg.st[k].op] + "]";
                        if (ImGui::Selectable(lab.c_str(), mgSel == k)) mgSel = k;
                        ImGui::PopID();
                    }
                    if (mg.st.empty()) ImGui::TextDisabled("Belum ada tahap. Klik Tambah.");
                }
                ImGui::EndChild();
                {
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w4 = (ImGui::GetContentRegionAvail().x - sp * 3.0f) / 4.0f;
                    if (ImGui::Button("Tambah", ImVec2(w4, 0))) {
                        if (mg.st.size() < 6) { mg.st.push_back(mkStage(GEN_DIRT, 0.10f, 0.55f, 0, 1.0f, 0.0f)); mgSel = (int)mg.st.size() - 1; mgDirty = true; }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Hapus", ImVec2(w4, 0))) {
                        if (!mg.st.empty()) { mg.st.erase(mg.st.begin() + glm::clamp(mgSel, 0, (int)mg.st.size() - 1)); mgSel = std::max(0, mgSel - 1); mgDirty = true; }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Naik", ImVec2(w4, 0))) {
                        if (mgSel > 0 && mgSel < (int)mg.st.size()) { std::swap(mg.st[mgSel], mg.st[mgSel - 1]); mgSel--; mgDirty = true; }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Turun", ImVec2(w4, 0))) {
                        if (mgSel >= 0 && mgSel + 1 < (int)mg.st.size()) { std::swap(mg.st[mgSel], mg.st[mgSel + 1]); mgSel++; mgDirty = true; }
                    }
                }
                if (!mg.st.empty()) {
                    mgSel = glm::clamp(mgSel, 0, (int)mg.st.size() - 1);
                    GenStage& s = mg.st[mgSel];
                    mgDirty |= ImGui::Combo("Jenis", &s.type, kGenNames, GEN_TYPE_COUNT);
                    int needMap = genStageMap(s.type);
                    if (needMap >= 0)
                        ImGui::TextDisabled("Map: %s%s", kBakeShort[needMap], (bakeMaps.has[needMap] && bakeMaps.size == canvas.size) ? " (siap)" : " (belum di-bake)");
                    mgDirty |= ImGui::SliderFloat("Batas bawah", &s.lo, -1.0f, 1.0f, "%.2f");
                    mgDirty |= ImGui::SliderFloat("Batas atas", &s.hi, -1.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rentang kecil = tepi keras, rentang lebar = pinggiran halus & efek lebih luas");
                    mgDirty |= ImGui::SliderFloat("Bobot", &s.strength, 0.0f, 1.0f, "%.2f");
                    mgDirty |= ImGui::Checkbox("Invert tahap", &s.invert);
                    if (mgSel > 0) mgDirty |= ImGui::Combo("Gabung", &s.op, kOpNames, 6);
                    if (s.type != GEN_NOISE) mgDirty |= ImGui::SliderFloat("Variasi acak", &s.jitter, 0.0f, 1.0f, "%.2f");
                    if (s.type == GEN_NOISE || s.jitter > 0.0f) {
                        ImGui::TextDisabled("Procedural noise");
                        mgDirty |= ImGui::SliderFloat("Skala", &s.scale, 1.0f, 200.0f, "%.0f");
                        mgDirty |= ImGui::SliderInt("Oktaf", &s.octaves, 1, 6);
                        mgDirty |= ImGui::SliderInt("Seed", &s.seed, 0, 99);
                        if (s.type == GEN_NOISE) {
                            mgDirty |= ImGui::SliderFloat("Regangan", &s.stretch, 1.0f, 40.0f, "%.1f");
                            mgDirty |= ImGui::SliderFloat("Sudut", &s.angle, -90.0f, 90.0f, "%.0f");
                        }
                    }
                }
                ImGui::SeparatorText("Hasil akhir");
                mgDirty |= ImGui::SliderFloat("Blur", &mg.blur, 0.0f, 8.0f, "%.0f px");
                mgDirty |= ImGui::SliderFloat("Ketebalan", &mg.level, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Di atas 0.5 = efek lebih tebal / luas (misal karat), di bawah = lebih tipis");
                mgDirty |= ImGui::SliderFloat("Kontras", &mg.contrast, 0.2f, 4.0f, "%.2f");
                mgDirty |= ImGui::Checkbox("Invert hasil", &mg.invertAll);
                ImGui::Checkbox("Auto-update ke layer aktif", &mgAuto);
                if (ImGui::Button("Terapkan editor ke layer aktif", ImVec2(-1, 30)))
                    requestMask(canvas.layers[canvas.active].id, mg, "MG Mask Editor");
                {
                    Layer& AL = canvas.layers[canvas.active];
                    if (AL.hasMask) {
                        ImGui::Text("Mask layer: %s", AL.name.c_str());
                        ImGui::Image((ImTextureID)(intptr_t)AL.maskTex, ImVec2(96, 96), ImVec2(0, 1), ImVec2(1, 0));
                    } else ImGui::TextDisabled("Layer aktif belum punya mask.");
                }
                ImGui::EndTabItem();
            }
            // ------------------------------------------------ Bake
            if (ImGui::BeginTabItem("Bake")) {
                bool running = job.running.load();
                bool canBake = mesh.vao && mesh.hasUV && !running;
                bool highLoaded = !highVerts.empty();

                ImGui::SeparatorText("Pilih map");
                if (ImGui::BeginTable("##bkmaps", 2)) {
                    for (int i = 0; i < BK_COUNT; i++) {
                        ImGui::TableNextColumn();
                        bool needHigh = (i == BK_NRMTAN || i == BK_HEIGHT);
                        if (needHigh && !highLoaded) ImGui::BeginDisabled();
                        ImGui::Checkbox(kBakeShort[i], &bp.on[i]);
                        if (needHigh && !highLoaded) ImGui::EndDisabled();
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Normal (Tangent) dan Height butuh high-poly.");

                ImGui::SeparatorText("High-poly");
                if (highLoaded) ImGui::Text("%s (%d tris)", highName.c_str(), (int)(highVerts.size() / 3));
                else ImGui::TextDisabled("Belum ada high-poly.");
                {
                    float sp = ImGui::GetStyle().ItemSpacing.x;
                    float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                    if (ImGui::Button("Load high-poly...", ImVec2(w2, 0))) loadHigh(openFileDialog());
                    ImGui::SameLine();
                    if (!highLoaded) ImGui::BeginDisabled();
                    if (ImGui::Button("Hapus", ImVec2(w2, 0))) {
                        highVerts.clear(); highVerts.shrink_to_fit(); highName.clear();
                        bp.on[BK_NRMTAN] = false; bp.on[BK_HEIGHT] = false;
                    }
                    if (!highLoaded) ImGui::EndDisabled();
                }

                if (bp.on[BK_AO] || bp.on[BK_THICK]) {
                    ImGui::SeparatorText("Kualitas (AO / Thickness)");
                    ImGui::Combo("Sampel", &bakeQ, qualNames, 3);
                }
                if (bp.on[BK_AO]) {
                    ImGui::SeparatorText("AO");
                    ImGui::SliderFloat("Jarak AO", &bp.aoDistPct, 2.0f, 100.0f, "%.0f %%");
                    ImGui::SliderFloat("Kontras", &bp.power, 0.5f, 3.0f, "%.2f");
                }
                if (bp.on[BK_THICK]) {
                    ImGui::SeparatorText("Thickness");
                    ImGui::SliderFloat("Jarak maks", &bp.thickDistPct, 2.0f, 100.0f, "%.0f %%");
                }
                if (bp.on[BK_CURV]) {
                    ImGui::SeparatorText("Curvature");
                    ImGui::SliderInt("Radius", &bp.curvRadius, 1, 8, "%d px");
                    ImGui::SliderFloat("Kekuatan", &bp.curvStrength, 0.2f, 10.0f, "%.1f");
                }
                if (bp.on[BK_NRMTAN] || bp.on[BK_HEIGHT]) {
                    ImGui::SeparatorText("Normal / Height (cage)");
                    ImGui::SliderFloat("Jarak cage", &bp.cagePct, 0.5f, 30.0f, "%.1f %%");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seberapa jauh sinar dicari ke high-poly. Model high & low harus sejajar.");
                }
                ImGui::SeparatorText("Umum");
                ImGui::Checkbox("Balik normal", &bp.flip);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centang jika hasil terbalik / hitam semua");
                ImGui::TextDisabled("Resolusi bake = resolusi texture (%d px)", canvas.size);
                if (!mesh.vao) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Load model dulu.");
                else if (!mesh.hasUV) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Model tidak punya UV.");
                if (!canBake) ImGui::BeginDisabled();
                if (ImGui::Button("Bake", ImVec2(-1, 34))) startBake();
                if (!canBake) ImGui::EndDisabled();
                if (running) {
                    int st = job.stage.load();
                    std::string stName = st == -1 ? "Membangun BVH (low-poly)..." : (st == -2 ? "Membangun BVH (high-poly)..." :
                                         (st == -3 ? "Padding seam..." : std::string("Menghitung ") + kBakeShort[st] + "..."));
                    bool rows = st == BK_AO || st == BK_THICK || st == BK_NRMTAN || st == BK_HEIGHT;
                    float frac = rows ? (float)job.rowsDone.load() / (float)std::max(1, job.size) : (st == -1 || st == -2 ? 0.0f : 1.0f);
                    ImGui::ProgressBar(frac, ImVec2(-1, 0), stName.c_str());
                    if (ImGui::Button("Batal", ImVec2(-1, 0))) job.cancel = true;
                }

                ImGui::SeparatorText("Hasil");
                if (!bakeMaps.any()) ImGui::TextDisabled("Belum ada hasil bake.");
                for (int i = 0; i < BK_COUNT; i++) if (bakeMaps.has[i]) ImGui::BulletText("%s (%d px)", kBakeShort[i], bakeMaps.size);
                if (bakeMaps.has[BK_AO])
                    if (ImGui::Button("AO -> Layer (Multiply)", ImVec2(-1, 0))) bakeToLayer(BK_AO, 1, 1.0f, "AO");
                if (bakeMaps.has[BK_CURV])
                    if (ImGui::Button("Curvature -> Layer (Overlay)", ImVec2(-1, 0))) bakeToLayer(BK_CURV, 3, 0.5f, "Curvature");
                if (bakeMaps.any()) {
                    if (bakeMaps.has[BK_AO]) ImGui::Checkbox("Pakai AO di preview", &useAO);
                    if (ImGui::Button("Hapus semua hasil bake", ImVec2(-1, 0))) bakeMaps.clear();
                }
                ImGui::EndTabItem();
            }
            // ------------------------------------------------ Texture / Export
            if (ImGui::BeginTabItem("Texture")) {
                ImGui::SeparatorText("Resolusi");
                if (ImGui::Combo("Ukuran", &sizeIdx, sizeNames, 3)) {
                    bakeStop();
                    bakeMaps.clear();
                    editMask = false;
                    canvas.create(sizeVals[sizeIdx]);
                    canvas.buildMask(mesh);
                }
                ImGui::TextDisabled("Ganti resolusi = reset layer & hasil bake");
                ImGui::SeparatorText("Nilai dasar (latar)");
                if (ImGui::ColorEdit3("Color", baseColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) canvas.dirty = true;
                if (ImGui::SliderFloat("Roughness", &baseRough, 0.0f, 1.0f, "%.2f")) canvas.dirty = true;
                if (ImGui::SliderFloat("Metallic", &baseMetal, 0.0f, 1.0f, "%.2f")) canvas.dirty = true;
                ImGui::SeparatorText("Export");
                ImGui::Checkbox("Sertakan ORM (glTF)", &exportORM);
                ImGui::Checkbox("Sertakan map hasil bake", &exportBake);
                if (ImGui::Button("Export PNG...", ImVec2(-1, 32))) doExport();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();
        ImGui::End();

        // ---- panel kanan: layer + preview ----
        ImGui::SetNextWindowPos(ImVec2(winW - rightW, menuH));
        ImGui::SetNextWindowSize(ImVec2(rightW, winH - menuH - statusH));
        ImGui::Begin("##right", nullptr, panelFlags);
        ImGui::PushItemWidth(rightW * 0.48f);
        ImGui::SeparatorText("Layer");
        {
            // daftar layer: seret baris dengan mouse untuk menggeser ke atas / bawah
            int n = (int)canvas.layers.size();
            for (int i = n - 1; i >= 0; i--) {
                Layer& L = canvas.layers[i];
                ImGui::PushID(L.id);
                if (ImGui::Checkbox("##vis", &L.visible)) canvas.dirty = true;
                ImGui::SameLine();
                std::string label = L.name + (L.hasMask ? "  [mask]" : "");
                if (ImGui::Selectable(label.c_str(), canvas.active == i)) canvas.active = i;
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SMASK")) {
                        int pi = *(const int*)pl->Data;
                        if (pi >= 0 && pi < (int)maskLib.size()) {
                            canvas.active = i; maskSel = pi; mg = maskLib[pi].gen; mgSel = 0;
                            requestMask(L.id, mg, maskLib[pi].name);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                bool activeItem = ImGui::IsItemActive();
                bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (activeItem) canvas.active = i;
                if (activeItem && !hovered) {
                    float dy = ImGui::GetMouseDragDelta(0).y;
                    if (dy != 0.0f) {
                        int j = i + (dy < 0.0f ? 1 : -1);   // tarik ke atas = naik di daftar
                        if (j >= 0 && j < n) {
                            std::swap(canvas.layers[i], canvas.layers[j]);
                            canvas.active = j;
                            canvas.dirty = true;
                            ImGui::ResetMouseDragDelta(0);
                        }
                    }
                    break;   // daftar berubah, hentikan iterasi frame ini
                }
            }
            ImGui::TextDisabled("Tarik layer dengan mouse untuk geser");
        }
        {
            Layer& A = canvas.layers[canvas.active];
            if (ImGui::SliderFloat("Opacity", &A.opacity, 0.0f, 1.0f, "%.2f")) canvas.dirty = true;
            if (ImGui::Combo("Blend", &A.blend, blendNames, 5)) canvas.dirty = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend mode berlaku untuk kanal Color");
            if (!A.hasMask) editMask = false;
            float sp = ImGui::GetStyle().ItemSpacing.x;
            float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
            if (!A.hasMask) ImGui::BeginDisabled();
            bool tl = !editMask, tm = editMask;
            if (toggleBtn("Cat Layer", &tl, w2)) editMask = false;
            ImGui::SameLine();
            if (toggleBtn("Cat Mask", &tm, w2)) editMask = true;
            if (!A.hasMask) ImGui::EndDisabled();
        }
        {
            float sp = ImGui::GetStyle().ItemSpacing.x;
            float w4 = (ImGui::GetContentRegionAvail().x - sp * 3.0f) / 4.0f;
            if (ImGui::Button("Add", ImVec2(w4, 0))) canvas.addLayer();
            ImGui::SameLine();
            if (ImGui::Button("Del", ImVec2(w4, 0))) { canvas.deleteActive(); editMask = false; }
            ImGui::SameLine();
            if (ImGui::Button("Up", ImVec2(w4, 0))) canvas.moveActive(+1);
            ImGui::SameLine();
            if (ImGui::Button("Down", ImVec2(w4, 0))) canvas.moveActive(-1);
            float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
            if (ImGui::Button("Fill", ImVec2(w2, 0))) {
                Layer& L = canvas.layers[canvas.active];
                if (editMask && L.hasMask) { canvas.pushUndo(true); canvas.fillMask(L, 1.0f); }
                else {
                    canvas.pushUndo(false);
                    const float col[4] = {brushColor[0], brushColor[1], brushColor[2], 1.0f};
                    const float pbr[4] = {valRough, 1.0f, valMetal, 1.0f};
                    canvas.clearChannels(L, chColor, chRough, chMetal, col, pbr);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(w2, 0))) {
                Layer& L = canvas.layers[canvas.active];
                if (editMask && L.hasMask) { canvas.pushUndo(true); canvas.fillMask(L, 0.0f); }
                else {
                    canvas.pushUndo(false);
                    const float z[4] = {0, 0, 0, 0};
                    canvas.clearChannels(L, chColor, chRough, chMetal, z, z);
                }
            }
            ImGui::TextDisabled("Fill/Clear: kanal aktif (atau mask)");
            if (ImGui::Button("Undo", ImVec2(w2, 0))) canvas.undo();
            ImGui::SameLine();
            if (ImGui::Button("Redo", ImVec2(w2, 0))) canvas.redo();
        }
        ImGui::SeparatorText("Preview");
        ImGui::Combo("Mode", &viewMode, viewNames, 5);
        if (viewMode == 4) {
            if (!bakeMaps.any()) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Belum ada hasil bake (tab Bake).");
            else {
                if (!bakeMaps.has[previewBake]) previewBake = bakeMaps.firstHas();
                if (ImGui::BeginCombo("Map", kBakeShort[previewBake])) {
                    for (int i = 0; i < BK_COUNT; i++) {
                        if (!bakeMaps.has[i]) continue;
                        if (ImGui::Selectable(kBakeShort[i], previewBake == i)) previewBake = i;
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::Checkbox("Wireframe", &wire);
        ImGui::Checkbox("Grid", &showGrid);
        ImGui::Checkbox("Grid di dasar model", &gridAtBase);
        if (ImGui::Button("Reset kamera (F)", ImVec2(-1, 0)) && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
        ImGui::PopItemWidth();
        ImGui::End();

        // ---- status bar ----
        ImGui::SetNextWindowPos(ImVec2(0, winH - statusH));
        ImGui::SetNextWindowSize(ImVec2(winW, statusH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 6));
        ImGui::Begin("##status", nullptr, panelFlags | ImGuiWindowFlags_NoScrollbar);
        {
            std::string line = status;
            size_t nl = line.find('\n');
            if (nl != std::string::npos) line = line.substr(0, nl);
            ImGui::TextUnformatted(line.c_str());
            char info[128];
            std::snprintf(info, sizeof(info), "%d tris | %d px | %.0f FPS", mesh.triangles, canvas.size, io.Framerate);
            ImGui::SameLine(winW - ImGui::CalcTextSize(info).x - 16.0f);
            ImGui::TextDisabled("%s", info);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // ---- auto-update mask dari MG Mask Editor ----
        if (mgDirty && mgAuto && !ImGui::IsAnyItemActive()) {
            mgDirty = false;
            requestMask(canvas.layers[canvas.active].id, mg, "MG Mask Editor");
        }

        // ---- komposit layer ----
        bool busy = stroke || ImGui::IsAnyItemActive();
        if (canvas.dirty) { canvas.compose(baseColor, baseRough, baseMetal, !busy); undilated = busy; }
        else if (undilated && !busy) { canvas.compose(baseColor, baseRough, baseMetal, true); undilated = false; }

        // ---- render 3D ----
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbw, fbh);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glClearColor(0.16f, 0.17f, 0.19f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(vpx, vpy, vpw, vph);
        glEnable(GL_DEPTH_TEST);

        if (showGrid) {
            glm::vec3 origin(0.0f); float gscale = 1.0f;
            if (mesh.vao && gridAtBase) {
                origin = glm::vec3((mesh.bmin.x + mesh.bmax.x) * 0.5f, mesh.bmin.y, (mesh.bmin.z + mesh.bmax.z) * 0.5f);
                gscale = std::max(modelRadius * 0.2f, 1e-4f);
            }
            glUseProgram(lineProg);
            glUniformMatrix4fv(glGetUniformLocation(lineProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3fv(glGetUniformLocation(lineProg, "uOrigin"), 1, glm::value_ptr(origin));
            glUniform1f(glGetUniformLocation(lineProg, "uScale"), gscale);
            glUniform3f(glGetUniformLocation(lineProg, "uColor"), 0.3f, 0.3f, 0.33f);
            glBindVertexArray(gvao); glDrawArrays(GL_LINES, 0, (GLsizei)gl.size()); glBindVertexArray(0);
        }

        if (mesh.vao) {
            glUseProgram(meshProg);
            glm::vec3 eye = cam.eye();
            glm::vec3 light = glm::normalize(glm::normalize(eye - cam.target) + glm::vec3(0.5f, 0.8f, 0.2f));
            bool aoOK = bakeMaps.has[BK_AO] && bakeMaps.size > 0;
            bool bakeOK = bakeMaps.has[previewBake];
            glUniformMatrix4fv(glGetUniformLocation(meshProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3fv(glGetUniformLocation(meshProg, "uEye"), 1, glm::value_ptr(eye));
            glUniform3fv(glGetUniformLocation(meshProg, "uLight"), 1, glm::value_ptr(light));
            glUniform1i(glGetUniformLocation(meshProg, "uView"), viewMode);
            glUniform1i(glGetUniformLocation(meshProg, "uUseAO"), (useAO && aoOK) ? 1 : 0);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, canvas.dispC);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, canvas.dispP);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, aoOK ? bakeMaps.tex[BK_AO] : dummyWhite);
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, bakeOK ? bakeMaps.tex[previewBake] : dummyWhite);
            glUniform1i(glGetUniformLocation(meshProg, "uTex"), 0);
            glUniform1i(glGetUniformLocation(meshProg, "uTexP"), 1);
            glUniform1i(glGetUniformLocation(meshProg, "uTexAO"), 2);
            glUniform1i(glGetUniformLocation(meshProg, "uTexBake"), 3);
            glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
            mesh.draw();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glActiveTexture(GL_TEXTURE0);
        }

        ImGui::Render();
        glViewport(0, 0, fbw, fbh);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    bakeStop();
    for (auto& t : texMats) { if (t.c) glDeleteTextures(1, &t.c); if (t.r) glDeleteTextures(1, &t.r); if (t.m) glDeleteTextures(1, &t.m); }
    scratch.destroy();
    mesh.destroy(); canvas.destroy(); bakeMaps.clear();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
