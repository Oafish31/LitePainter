// LitePainter - Tahap 4: kanal PBR (Base Color / Roughness / Metallic) + preview PBR
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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
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

// uView: 0 = Base Color, 1 = PBR, 2 = Roughness, 3 = Metallic
static const char* kMeshFS = R"(#version 330 core
in vec3 vP; in vec3 vN; in vec2 vUV;
uniform vec3 uEye; uniform vec3 uLight;
uniform sampler2D uTex; uniform sampler2D uTexP;
uniform int uView;
out vec4 frag;
const float PI = 3.14159265;

// lingkungan prosedural sederhana (langit + tanah + matahari), makin kasar makin blur
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
  vec3 n = normalize(vN);
  vec3 v = normalize(uEye - vP);
  if (dot(n, v) < 0.0) n = -n;
  vec3 base = texture(uTex, vUV).rgb;
  vec2 rm = texture(uTexP, vUV).rg;

  if (uView == 0) {
    float d = max(dot(n, v), 0.0);
    float sky = 0.5 + 0.5 * n.y;
    frag = vec4(base * (0.3 + 0.5 * d + 0.2 * sky), 1.0);
    return;
  }
  if (uView >= 2) {
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

  vec3 col = direct + amb;
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

// Paint pass (MRT): mesh digambar di ruang UV. Output premultiplied.
//   outColor = (rgb*a, a)
//   outPbr   = (rough*a, a, metal*a, a)   -> R,G = roughness(+alpha), B,A = metallic(+alpha)
static const char* kPaintVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
out vec3 vP; out vec3 vN;
void main(){ vP = aPos; vN = aNrm; gl_Position = vec4(aUV * 2.0 - 1.0, 0.0, 1.0); }
)";
static const char* kPaintFS = R"(#version 330 core
in vec3 vP; in vec3 vN;
uniform vec3 uHit; uniform vec3 uHitN; uniform vec3 uColor;
uniform float uRadius; uniform float uHardness; uniform float uOpacity;
uniform float uRough; uniform float uMetal;
layout(location=0) out vec4 outColor;
layout(location=1) out vec4 outPbr;
void main(){
  float d = distance(vP, uHit) / uRadius;
  if (d >= 1.0) discard;
  if (dot(normalize(vN), uHitN) < 0.1) discard;
  float a = (1.0 - smoothstep(uHardness, 1.0, d)) * uOpacity;
  outColor = vec4(uColor * a, a);
  outPbr = vec4(uRough * a, a, uMetal * a, a);
}
)";
static const char* kMaskFS = R"(#version 330 core
out vec4 frag;
void main(){ frag = vec4(1.0); }
)";
static const char* kFullVS = R"(#version 330 core
void main(){
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
// Komposit satu layer. Hasil: outC = base color, outP = (roughness, metallic, 0, 1)
static const char* kCompFS = R"(#version 330 core
uniform sampler2D uDst; uniform sampler2D uLayer;
uniform sampler2D uDstP; uniform sampler2D uLayerP;
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
  vec4 dst = texelFetch(uDst, c, 0);
  vec4 L = texelFetch(uLayer, c, 0);
  float a = L.a * uOpacity;
  vec3 s = (L.a > 0.0001) ? (L.rgb / L.a) : vec3(0.0);
  outC = vec4(mix(dst.rgb, blendFn(dst.rgb, s), a), 1.0);

  vec4 dp = texelFetch(uDstP, c, 0);
  vec4 P = texelFetch(uLayerP, c, 0);
  float ra = P.g * uOpacity;
  float rv = (P.g > 0.0001) ? (P.r / P.g) : 0.0;
  float ma = P.a * uOpacity;
  float mv = (P.a > 0.0001) ? (P.b / P.a) : 0.0;
  outP = vec4(mix(dp.r, rv, ra), mix(dp.g, mv, ma), 0.0, 1.0);
}
)";
// Padding seam untuk kedua texture sekaligus
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

static bool loadOBJ(const std::string& path, Mesh& mesh, std::string& msg) {
    tinyobj::ObjReaderConfig cfg;
    cfg.triangulate = true;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg)) { msg = "Gagal load: " + reader.Error(); return false; }

    const auto& a = reader.GetAttrib();
    std::vector<Vertex> verts;
    glm::vec3 bmin(1e30f), bmax(-1e30f);

    for (const auto& shape : reader.GetShapes()) {
        const auto& idx = shape.mesh.indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            Vertex tri[3];
            for (int k = 0; k < 3; k++) {
                const auto& id = idx[i + k];
                Vertex& v = tri[k];
                v.p = { a.vertices[3 * id.vertex_index], a.vertices[3 * id.vertex_index + 1], a.vertices[3 * id.vertex_index + 2] };
                v.n = glm::vec3(0);
                if (id.normal_index >= 0)
                    v.n = { a.normals[3 * id.normal_index], a.normals[3 * id.normal_index + 1], a.normals[3 * id.normal_index + 2] };
                v.uv = glm::vec2(0);
                if (id.texcoord_index >= 0)
                    v.uv = { a.texcoords[2 * id.texcoord_index], a.texcoords[2 * id.texcoord_index + 1] };
                bmin = glm::min(bmin, v.p); bmax = glm::max(bmax, v.p);
            }
            glm::vec3 fn = glm::cross(tri[1].p - tri[0].p, tri[2].p - tri[0].p);
            if (glm::length(fn) > 0) fn = glm::normalize(fn);
            for (int k = 0; k < 3; k++) {
                if (glm::length(tri[k].n) < 1e-6f) tri[k].n = fn;
                verts.push_back(tri[k]);
            }
        }
    }
    if (verts.empty()) { msg = "File OBJ kosong / tidak ada segitiga."; return false; }
    mesh.upload(verts);
    mesh.bmin = bmin; mesh.bmax = bmax;
    mesh.triangles = (int)verts.size() / 3;
    mesh.hasUV = !a.texcoords.empty();
    msg = "Loaded: " + path;
    if (!mesh.hasUV) msg += "\nPERINGATAN: model tidak punya UV, painting nonaktif.";
    return true;
}

// ================================================================ raycast
struct Hit { bool ok = false; float t = 1e30f; glm::vec3 p{0}, n{0, 1, 0}; };

static Hit raycast(const Mesh& m, const glm::vec3& o, const glm::vec3& d) {
    Hit best;
    const auto& v = m.cpu;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        const glm::vec3& a = v[i].p; const glm::vec3& b = v[i + 1].p; const glm::vec3& c = v[i + 2].p;
        glm::vec3 e1 = b - a, e2 = c - a, pv = glm::cross(d, e2);
        float det = glm::dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        float inv = 1.0f / det;
        glm::vec3 tv = o - a;
        float u = glm::dot(tv, pv) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        glm::vec3 qv = glm::cross(tv, e1);
        float w = glm::dot(d, qv) * inv;
        if (w < 0.0f || u + w > 1.0f) continue;
        float t = glm::dot(e2, qv) * inv;
        if (t > 1e-5f && t < best.t) {
            best.ok = true; best.t = t; best.p = o + d * t;
            glm::vec3 n = (1.0f - u - w) * v[i].n + u * v[i + 1].n + w * v[i + 2].n;
            best.n = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0, 1, 0);
        }
    }
    return best;
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
static GLuint makeFbo2(GLuint t0, GLuint t1) {   // 2 attachment (MRT)
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
    GLuint tex = 0;    // base color RGBA8, premultiplied
    GLuint texP = 0;   // (rough*a, a, metal*a, a)
    GLuint fbo = 0;    // MRT: attachment0 = tex, attachment1 = texP
    float opacity = 1.0f;
    int blend = 0;     // 0 Normal,1 Multiply,2 Screen,3 Overlay,4 Add (berlaku untuk Base Color)
    bool visible = true;
};
struct UndoEntry { int layerId = 0; std::vector<unsigned char> px, pxP; };

struct Canvas {
    static constexpr size_t kMaxLayers = 6;
    int size = 0, active = 0, nextId = 1, cur = 0;
    bool dirty = false, maskValid = false;
    std::vector<Layer> layers;
    GLuint compC[2] = {0, 0}, compP[2] = {0, 0}, compFbo[2] = {0, 0};
    GLuint dispC = 0, dispP = 0, dispCFbo = 0, dispPFbo = 0;
    GLuint mask = 0, maskFbo = 0;
    GLuint compProg = 0, dilProg = 0, maskProg = 0, emptyVao = 0;
    std::vector<UndoEntry> undoS, redoS;

    size_t maxStates() const { return size >= 2048 ? 8 : (size >= 1024 ? 20 : 40); }

    void freeLayer(Layer& L) {
        if (L.fbo) glDeleteFramebuffers(1, &L.fbo);
        if (L.tex) glDeleteTextures(1, &L.tex);
        if (L.texP) glDeleteTextures(1, &L.texP);
        L.fbo = L.tex = L.texP = 0;
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
        dispCFbo = dispPFbo = dispC = dispP = maskFbo = mask = 0;
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
        addLayer();
        dirty = true;
    }
    // Isi/hapus hanya kanal yang aktif (c = color, r = roughness, m = metallic)
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
    void moveActive(int dir) {   // +1 = naik, -1 = turun
        int j = active + dir;
        if (j < 0 || j >= (int)layers.size()) return;
        std::swap(layers[active], layers[j]);
        active = j; dirty = true;
    }
    Layer* findLayer(int id) {
        for (auto& L : layers) if (L.id == id) return &L;
        return nullptr;
    }
    void readAtt(const Layer& L, int att, std::vector<unsigned char>& out) {
        out.resize((size_t)size * size * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
        glReadBuffer((GLenum)(GL_COLOR_ATTACHMENT0 + att));
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    void writeTex(GLuint t, const std::vector<unsigned char>& in) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, in.data());
        dirty = true;
    }
    void pushUndo() {   // snapshot layer aktif
        UndoEntry e; e.layerId = layers[active].id;
        readAtt(layers[active], 0, e.px);
        readAtt(layers[active], 1, e.pxP);
        undoS.push_back(std::move(e));
        redoS.clear();
        if (undoS.size() > maxStates()) undoS.erase(undoS.begin());
    }
    void undo() {
        while (!undoS.empty()) {
            UndoEntry e = std::move(undoS.back()); undoS.pop_back();
            Layer* L = findLayer(e.layerId);
            if (!L) continue;
            UndoEntry r; r.layerId = e.layerId;
            readAtt(*L, 0, r.px); readAtt(*L, 1, r.pxP);
            redoS.push_back(std::move(r));
            writeTex(L->tex, e.px); writeTex(L->texP, e.pxP);
            return;
        }
    }
    void redo() {
        while (!redoS.empty()) {
            UndoEntry e = std::move(redoS.back()); redoS.pop_back();
            Layer* L = findLayer(e.layerId);
            if (!L) continue;
            UndoEntry r; r.layerId = e.layerId;
            readAtt(*L, 0, r.px); readAtt(*L, 1, r.pxP);
            undoS.push_back(std::move(r));
            writeTex(L->tex, e.px); writeTex(L->texP, e.pxP);
            return;
        }
    }
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
    // Gabungkan semua layer -> dispC / dispP.  dilate = isi area di luar UV (padding seam)
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
        GLint locOp = glGetUniformLocation(compProg, "uOpacity");
        GLint locMode = glGetUniformLocation(compProg, "uMode");
        for (auto& L : layers) {
            if (!L.visible || L.opacity <= 0.0f) continue;
            glBindFramebuffer(GL_FRAMEBUFFER, compFbo[1 - cur]);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, compC[cur]);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, L.tex);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, compP[cur]);
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, L.texP);
            glUniform1f(locOp, L.opacity);
            glUniform1i(locMode, L.blend);
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

        // salin ke texture tampilan (punya mipmap)
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
    // Baca hasil akhir: color RGB, roughness (1 kanal), metallic (1 kanal)
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

// ================================================================ misc
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

int main(int argc, char** argv) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(1280, 720, "LitePainter", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "Gagal buat window (butuh OpenGL 3.3)\n"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetDropCallback(win, dropCallback);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    GLuint meshProg = makeProgram(kMeshVS, kMeshFS);
    GLuint lineProg = makeProgram(kLineVS, kLineFS);
    GLuint paintProg = makeProgram(kPaintVS, kPaintFS);

    Mesh mesh; Camera cam; Canvas canvas;
    canvas.maskProg = makeProgram(kPaintVS, kMaskFS);
    canvas.compProg = makeProgram(kFullVS, kCompFS);
    canvas.dilProg = makeProgram(kFullVS, kDilFS);
    glGenVertexArrays(1, &canvas.emptyVao);

    // grid
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

    std::string status = "Drag & drop file .obj ke window, atau klik Open OBJ.";
    bool wire = false, showGrid = true, gridAtBase = true;

    bool paintMode = true, eraser = false, symmetry = false;
    bool chColor = true, chRough = false, chMetal = false;
    bool exportORM = true;
    float brushColor[3] = {0.85f, 0.2f, 0.2f};
    float valRough = 0.5f, valMetal = 0.0f;
    float baseColor[3] = {0.8f, 0.8f, 0.8f};
    float baseRough = 0.5f, baseMetal = 0.0f;
    float brushSizePct = 5.0f, hardness = 0.5f, opacity = 1.0f;
    const char* sizeNames[3] = {"512", "1024", "2048"};
    const int sizeVals[3] = {512, 1024, 2048};
    const char* blendNames[5] = {"Normal", "Multiply", "Screen", "Overlay", "Add"};
    const char* viewNames[4] = {"Base Color", "PBR", "Roughness", "Metallic"};
    int sizeIdx = 1, viewMode = 0;
    canvas.create(sizeVals[sizeIdx]);

    bool stroke = false, strokeSnap = false, undilated = false;
    glm::vec2 lastMp(0);

    auto tryLoad = [&](const std::string& p) {
        if (p.empty()) return;
        if (loadOBJ(p, mesh, status)) {
            cam.fit(mesh.bmin, mesh.bmax);
            canvas.create(sizeVals[sizeIdx]);
            canvas.buildMask(mesh);
        }
    };
    if (argc > 1) tryLoad(argv[1]);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (!g_dropped.empty()) { tryLoad(g_dropped); g_dropped.clear(); }

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

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
            if (io.MouseWheel != 0) cam.dist *= (io.MouseWheel > 0) ? 0.9f : 1.1f;
            cam.dist = std::max(cam.dist, 0.01f);
        }

        // ---- matriks ----
        int fbw, fbh; glfwGetFramebufferSize(win, &fbw, &fbh);
        float aspect = fbh > 0 ? (float)fbw / fbh : 1.0f;
        float nearP = std::max(cam.dist * 0.01f, 0.001f);
        const float fov = glm::radians(45.0f);
        glm::mat4 view = cam.view();
        glm::mat4 proj = glm::perspective(fov, aspect, nearP, cam.dist * 100.0f);
        glm::mat4 mvp = proj * view;

        auto pickAt = [&](glm::vec2 m) -> Hit {
            glm::vec4 vp(0, 0, (float)fbw, (float)fbh);
            glm::vec3 a = glm::unProject(glm::vec3(m.x, fbh - m.y, 0.0f), view, proj, vp);
            glm::vec3 b = glm::unProject(glm::vec3(m.x, fbh - m.y, 1.0f), view, proj, vp);
            return raycast(mesh, a, glm::normalize(b - a));
        };

        // ---- painting ----
        float modelRadius = glm::length(mesh.bmax - mesh.bmin) * 0.5f;
        float radiusWorld = std::max(brushSizePct / 100.0f * modelRadius, 1e-5f);
        bool anyChannel = chColor || chRough || chMetal;
        bool canPaint = paintMode && mesh.vao && mesh.hasUV && !io.KeyAlt && !io.KeyShift
                        && anyChannel && !canvas.layers.empty() && canvas.layers[canvas.active].visible;
        bool mouseOk = ImGui::IsMousePosValid() && !io.WantCaptureMouse;
        glm::vec2 mp(io.MousePos.x * io.DisplayFramebufferScale.x, io.MousePos.y * io.DisplayFramebufferScale.y);

        float distRef = cam.dist;
        if (canPaint && mouseOk) {
            Hit hv = pickAt(mp);
            if (hv.ok) distRef = glm::length(hv.p - cam.eye());
        }
        float brushPx = radiusWorld * (fbh * 0.5f) / (std::max(distRef, 1e-4f) * std::tan(fov * 0.5f));

        auto paintStamp = [&](const glm::vec3& p, const glm::vec3& n) {
            if (!strokeSnap) { canvas.pushUndo(); strokeSnap = true; }
            glBindFramebuffer(GL_FRAMEBUFFER, canvas.layers[canvas.active].fbo);
            glViewport(0, 0, canvas.size, canvas.size);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            if (eraser) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);   // hapus -> transparan
            else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);           // premultiplied
            // hanya kanal yang dicentang yang tertulis
            glColorMaski(0, (GLboolean)chColor, (GLboolean)chColor, (GLboolean)chColor, (GLboolean)chColor);
            glColorMaski(1, (GLboolean)chRough, (GLboolean)chRough, (GLboolean)chMetal, (GLboolean)chMetal);
            glUseProgram(paintProg);
            glUniform3fv(glGetUniformLocation(paintProg, "uColor"), 1, brushColor);
            glUniform1f(glGetUniformLocation(paintProg, "uRough"), valRough);
            glUniform1f(glGetUniformLocation(paintProg, "uMetal"), valMetal);
            glUniform1f(glGetUniformLocation(paintProg, "uRadius"), radiusWorld);
            glUniform1f(glGetUniformLocation(paintProg, "uHardness"), std::min(hardness, 0.99f));
            glUniform1f(glGetUniformLocation(paintProg, "uOpacity"), opacity);
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
            glDisable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            canvas.dirty = true;
        };
        auto stampAt = [&](glm::vec2 m) {
            Hit h = pickAt(m);
            if (h.ok) paintStamp(h.p, h.n);
        };

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

        // ---- shortcut undo/redo ----
        if (!io.WantTextInput && io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) canvas.redo(); else canvas.undo(); }
            if (ImGui::IsKeyPressed(ImGuiKey_Y)) canvas.redo();
        }

        // ---- cursor brush ----
        if (canPaint && mouseOk) {
            ImGui::GetForegroundDrawList()->AddCircle(io.MousePos, brushPx / io.DisplayFramebufferScale.x,
                IM_COL32(255, 255, 255, 220), 48, 1.5f);
        }

        // ---- UI ----
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::Begin("LitePainter");
        if (ImGui::Button("Open OBJ...")) tryLoad(openFileDialog());
        ImGui::TextWrapped("%s", status.c_str());
        if (mesh.vao) ImGui::Text("Triangles: %d", mesh.triangles);

        ImGui::SeparatorText("Brush");
        ImGui::Checkbox("Mode Paint (Alt+Kiri = orbit)", &paintMode);
        ImGui::Text("Kanal:");
        ImGui::SameLine(); ImGui::Checkbox("Color", &chColor);
        ImGui::SameLine(); ImGui::Checkbox("Rough", &chRough);
        ImGui::SameLine(); ImGui::Checkbox("Metal", &chMetal);
        if (!anyChannel) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Pilih minimal 1 kanal.");
        ImGui::ColorEdit3("Warna", brushColor);
        ImGui::SliderFloat("Roughness", &valRough, 0.0f, 1.0f);
        ImGui::SliderFloat("Metallic", &valMetal, 0.0f, 1.0f);
        ImGui::SliderFloat("Ukuran (%)", &brushSizePct, 0.2f, 30.0f);
        ImGui::SliderFloat("Hardness", &hardness, 0.0f, 0.99f);
        ImGui::SliderFloat("Opacity", &opacity, 0.05f, 1.0f);
        ImGui::Checkbox("Eraser", &eraser);
        ImGui::SameLine();
        ImGui::Checkbox("Symmetry X", &symmetry);

        ImGui::SeparatorText("Layer");
        for (int i = (int)canvas.layers.size() - 1; i >= 0; i--) {
            Layer& L = canvas.layers[i];
            ImGui::PushID(L.id);
            if (ImGui::Checkbox("##vis", &L.visible)) canvas.dirty = true;
            ImGui::SameLine();
            if (ImGui::Selectable(L.name.c_str(), canvas.active == i)) canvas.active = i;
            ImGui::PopID();
        }
        {
            Layer& A = canvas.layers[canvas.active];
            if (ImGui::SliderFloat("Opacity layer", &A.opacity, 0.0f, 1.0f)) canvas.dirty = true;
            if (ImGui::Combo("Blend (Color)", &A.blend, blendNames, 5)) canvas.dirty = true;
        }
        if (ImGui::Button("Add")) canvas.addLayer();
        ImGui::SameLine();
        if (ImGui::Button("Delete")) canvas.deleteActive();
        ImGui::SameLine();
        if (ImGui::Button("Naik")) canvas.moveActive(+1);
        ImGui::SameLine();
        if (ImGui::Button("Turun")) canvas.moveActive(-1);
        if (ImGui::Button("Fill layer")) {
            canvas.pushUndo();
            const float col[4] = {brushColor[0], brushColor[1], brushColor[2], 1.0f};
            const float pbr[4] = {valRough, 1.0f, valMetal, 1.0f};
            canvas.clearChannels(canvas.layers[canvas.active], chColor, chRough, chMetal, col, pbr);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear layer")) {
            canvas.pushUndo();
            const float z[4] = {0, 0, 0, 0};
            canvas.clearChannels(canvas.layers[canvas.active], chColor, chRough, chMetal, z, z);
        }
        ImGui::TextDisabled("Fill/Clear hanya untuk kanal yang dicentang");

        ImGui::SeparatorText("Texture");
        if (ImGui::Combo("Resolusi", &sizeIdx, sizeNames, 3)) {
            canvas.create(sizeVals[sizeIdx]);
            canvas.buildMask(mesh);
        }
        ImGui::TextDisabled("(ganti resolusi = reset semua layer)");
        if (ImGui::ColorEdit3("Color dasar", baseColor)) canvas.dirty = true;
        if (ImGui::SliderFloat("Roughness dasar", &baseRough, 0.0f, 1.0f)) canvas.dirty = true;
        if (ImGui::SliderFloat("Metallic dasar", &baseMetal, 0.0f, 1.0f)) canvas.dirty = true;
        if (ImGui::Button("Undo")) canvas.undo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) canvas.redo();
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", (int)canvas.undoS.size());

        ImGui::Checkbox("Sertakan ORM (glTF)", &exportORM);
        if (ImGui::Button("Export PNG...")) {
            std::string path = saveFileDialog();
            if (!path.empty()) {
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
                if (ok && exportORM) {
                    std::vector<unsigned char> orm((size_t)sz * sz * 3);
                    for (size_t i = 0; i < (size_t)sz * sz; i++) { orm[3 * i] = 255; orm[3 * i + 1] = rough[i]; orm[3 * i + 2] = metal[i]; }
                    ok = stbi_write_png((stem + "_ORM.png").c_str(), sz, sz, 3, orm.data(), sz * 3) != 0;
                }
                status = ok ? ("Tersimpan: " + stem + "_BaseColor / _Roughness / _Metallic" + (exportORM ? " / _ORM" : "") + ".png")
                            : "Gagal menyimpan PNG.";
            }
        }

        ImGui::SeparatorText("View");
        ImGui::Combo("Preview", &viewMode, viewNames, 4);
        ImGui::Checkbox("Wireframe", &wire);
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &showGrid);
        ImGui::Checkbox("Grid di dasar model", &gridAtBase);
        if (ImGui::Button("Reset kamera") && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
        ImGui::TextDisabled("Kanan/Tengah/Shift+Kiri: pan | Scroll: zoom | Ctrl+Z/Y");
        ImGui::End();

        // ---- komposit layer (padding hanya saat tidak sedang melukis) ----
        bool busy = stroke || ImGui::IsAnyItemActive();
        if (canvas.dirty) { canvas.compose(baseColor, baseRough, baseMetal, !busy); undilated = busy; }
        else if (undilated && !busy) { canvas.compose(baseColor, baseRough, baseMetal, true); undilated = false; }

        // ---- render ----
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbw, fbh);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND);
        glClearColor(0.16f, 0.17f, 0.19f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
            glUniformMatrix4fv(glGetUniformLocation(meshProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3fv(glGetUniformLocation(meshProg, "uEye"), 1, glm::value_ptr(eye));
            glUniform3fv(glGetUniformLocation(meshProg, "uLight"), 1, glm::value_ptr(light));
            glUniform1i(glGetUniformLocation(meshProg, "uView"), viewMode);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, canvas.dispC);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, canvas.dispP);
            glUniform1i(glGetUniformLocation(meshProg, "uTex"), 0);
            glUniform1i(glGetUniformLocation(meshProg, "uTexP"), 1);
            glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
            mesh.draw();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glActiveTexture(GL_TEXTURE0);
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    mesh.destroy(); canvas.destroy();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
