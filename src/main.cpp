// LitePainter - Tahap 5: baking (AO + Curvature) dan UI baru
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
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>
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

// uView: 0 Base Color, 1 PBR, 2 Roughness, 3 Metallic, 4 AO, 5 Curvature
static const char* kMeshFS = R"(#version 330 core
in vec3 vP; in vec3 vN; in vec2 vUV;
uniform vec3 uEye; uniform vec3 uLight;
uniform sampler2D uTex; uniform sampler2D uTexP;
uniform sampler2D uTexAO; uniform sampler2D uTexCv;
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
  if (uView == 4) { frag = vec4(vec3(texture(uTexAO, vUV).r), 1.0); return; }
  if (uView == 5) { frag = vec4(vec3(texture(uTexCv, vUV).r), 1.0); return; }

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
// G-buffer di ruang UV: posisi + normal tiap texel (untuk baking)
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
    if (!mesh.hasUV) msg += " | PERINGATAN: tidak ada UV, painting nonaktif.";
    return true;
}

// ================================================================ raycast (klik brush)
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

// ================================================================ BVH (untuk baking)
struct BVH {
    struct Node { glm::vec3 bmin; int left; glm::vec3 bmax; int count; };   // count>0 = daun (left = indeks pertama)
    std::vector<Node> nodes;
    std::vector<glm::vec3> v0, e1, e2;

    void build(const std::vector<Vertex>& verts) {
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
        v0.resize(nt); e1.resize(nt); e2.resize(nt);
        for (int i = 0; i < nt; i++) {
            int t = idx[i];
            v0[i] = verts[3 * t].p;
            e1[i] = verts[3 * t + 1].p - v0[i];
            e2[i] = verts[3 * t + 2].p - v0[i];
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
    bool anyHit(const glm::vec3& o, const glm::vec3& d, float tmax) const {
        glm::vec3 inv(1.0f / (std::fabs(d.x) < 1e-9f ? 1e-9f : d.x),
                      1.0f / (std::fabs(d.y) < 1e-9f ? 1e-9f : d.y),
                      1.0f / (std::fabs(d.z) < 1e-9f ? 1e-9f : d.z));
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
};

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
    GLuint tex = 0, texP = 0, fbo = 0;
    float opacity = 1.0f;
    int blend = 0;
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
    void pushUndo() {
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
struct BakeParams {
    bool ao = true, curv = true;
    int samples = 32;
    float distPct = 30.0f, power = 1.0f;
    bool flip = false;
    int curvRadius = 2;
    float curvStrength = 2.0f;
};

struct BakeJob {
    std::atomic<bool> running{false}, done{false}, cancel{false};
    std::atomic<int> rowsDone{0}, stage{0};   // stage: 0 BVH, 1 AO, 2 Curvature, 3 Padding
    std::thread master;
    int size = 0;
    BakeParams P;
    float modelRadius = 1.0f;
    std::vector<float> pos, nrm;      // RGBA32F per texel dari G-buffer
    std::vector<Vertex> tris;
    std::vector<unsigned char> aoOut, curvOut;
    bool hasAO = false, hasCurv = false;
};

struct BakeMaps {   // hasil bake di sisi GPU/CPU
    GLuint ao = 0, curv = 0;
    bool hasAO = false, hasCurv = false;
    int size = 0;
    std::vector<unsigned char> aoPx, curvPx;

    static GLuint upload(int sz, const std::vector<unsigned char>& d) {
        GLuint t = makeTex(sz, GL_R8, GL_RED, true);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sz, sz, GL_RED, GL_UNSIGNED_BYTE, d.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        return t;
    }
    void clear() {
        if (ao) glDeleteTextures(1, &ao);
        if (curv) glDeleteTextures(1, &curv);
        ao = curv = 0; hasAO = hasCurv = false; size = 0;
        aoPx.clear(); curvPx.clear();
    }
    void set(int sz, const std::vector<unsigned char>* a, const std::vector<unsigned char>* c) {
        if (size != sz) clear();
        size = sz;
        if (a) { if (ao) glDeleteTextures(1, &ao); aoPx = *a; ao = upload(sz, aoPx); hasAO = true; }
        if (c) { if (curv) glDeleteTextures(1, &curv); curvPx = *c; curv = upload(sz, curvPx); hasCurv = true; }
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
// Lebarkan piksel valid keluar island UV (padding seam) untuk map 1 kanal
static void dilateU8(std::vector<unsigned char>& img, const std::vector<unsigned char>& cover, int sz, int passes) {
    std::vector<unsigned char> valid = cover, nimg, nvalid;
    for (int p = 0; p < passes; p++) {
        nimg = img; nvalid = valid;
        bool any = false;
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (valid[i]) continue;
                int sum = 0, cnt = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= sz || yy >= sz) continue;
                        size_t j = (size_t)yy * sz + xx;
                        if (valid[j]) { sum += img[j]; cnt++; }
                    }
                }
                if (cnt > 0) { nimg[i] = (unsigned char)(sum / cnt); nvalid[i] = 1; any = true; }
            }
        }
        img.swap(nimg); valid.swap(nvalid);
        if (!any) break;
    }
}

static void bakeMain(BakeJob* J) {
    const int sz = J->size;
    const size_t n = (size_t)sz * sz;
    const BakeParams P = J->P;
    const float R = J->modelRadius;
    std::vector<unsigned char> cover(n);
    for (size_t i = 0; i < n; i++) cover[i] = J->pos[4 * i + 3] > 0.5f ? 1 : 0;

    // ---------------- AO ----------------
    if (P.ao && !J->cancel) {
        J->stage = 0;
        BVH bvh;
        bvh.build(J->tris);
        J->stage = 1; J->rowsDone = 0;
        J->aoOut.assign(n, 255);
        const float maxDist = std::max(P.distPct / 100.0f * R, 1e-5f);
        const float eps = R * 0.001f;
        const int S = P.samples;
        std::atomic<int> nextRow(0);
        auto worker = [&]() {
            for (;;) {
                if (J->cancel) return;
                int y = nextRow.fetch_add(1);
                if (y >= sz) return;
                for (int x = 0; x < sz; x++) {
                    size_t i = (size_t)y * sz + x;
                    if (!cover[i]) continue;
                    glm::vec3 N(J->nrm[4 * i], J->nrm[4 * i + 1], J->nrm[4 * i + 2]);
                    float ln = glm::length(N);
                    if (ln < 1e-5f) continue;
                    N /= ln;
                    if (P.flip) N = -N;
                    glm::vec3 Pp(J->pos[4 * i], J->pos[4 * i + 1], J->pos[4 * i + 2]);
                    glm::vec3 org = Pp + N * eps;
                    glm::vec3 up = std::fabs(N.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                    glm::vec3 T = glm::normalize(glm::cross(up, N));
                    glm::vec3 B = glm::cross(N, T);
                    float rnd = (float)(hash2((unsigned)x, (unsigned)y) & 0xFFFFu) / 65536.0f;
                    int hits = 0;
                    for (int s = 0; s < S; s++) {
                        float u1 = ((float)s + 0.5f) / (float)S;
                        float u2 = radicalInverse2((unsigned)s) + rnd;
                        u2 -= std::floor(u2);
                        float r = std::sqrt(u1);
                        float phi = 6.2831853f * u2;
                        glm::vec3 d = T * (r * std::cos(phi)) + B * (r * std::sin(phi)) + N * std::sqrt(1.0f - u1);
                        if (bvh.anyHit(org, d, maxDist)) hits++;
                    }
                    float ao = 1.0f - (float)hits / (float)S;
                    ao = std::pow(ao, P.power);
                    J->aoOut[i] = (unsigned char)(glm::clamp(ao, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                J->rowsDone.fetch_add(1);
            }
        };
        unsigned hc = std::thread::hardware_concurrency();
        int nt = hc > 1 ? (int)hc - 1 : 1;
        std::vector<std::thread> th;
        for (int i = 0; i < nt; i++) th.emplace_back(worker);
        for (auto& t : th) t.join();
        if (!J->cancel) {
            J->stage = 3;
            dilateU8(J->aoOut, cover, sz, std::max(4, sz / 128));
            J->hasAO = true;
        }
    }

    // ---------------- Curvature (dari posisi + normal texel) ----------------
    if (P.curv && !J->cancel) {
        J->stage = 2;
        const int k = P.curvRadius;
        std::vector<float> cf(n, 0.5f);
        auto ok = [&](int x, int y) { return x >= 0 && y >= 0 && x < sz && y < sz && cover[(size_t)y * sz + x]; };
        auto pos3 = [&](int x, int y) { size_t i = (size_t)y * sz + x; return glm::vec3(J->pos[4 * i], J->pos[4 * i + 1], J->pos[4 * i + 2]); };
        auto nrm3 = [&](int x, int y) { size_t i = (size_t)y * sz + x; return glm::vec3(J->nrm[4 * i], J->nrm[4 * i + 1], J->nrm[4 * i + 2]); };
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                size_t i = (size_t)y * sz + x;
                if (!cover[i]) continue;
                float c = 0.0f; int cnt = 0;
                if (ok(x - k, y) && ok(x + k, y)) {
                    glm::vec3 dP = pos3(x + k, y) - pos3(x - k, y), dN = nrm3(x + k, y) - nrm3(x - k, y);
                    float d2 = glm::dot(dP, dP);
                    if (d2 > 1e-12f) { c += glm::dot(dN, dP) / d2; cnt++; }
                }
                if (ok(x, y - k) && ok(x, y + k)) {
                    glm::vec3 dP = pos3(x, y + k) - pos3(x, y - k), dN = nrm3(x, y + k) - nrm3(x, y - k);
                    float d2 = glm::dot(dP, dP);
                    if (d2 > 1e-12f) { c += glm::dot(dN, dP) / d2; cnt++; }
                }
                if (cnt > 0) c /= (float)cnt;
                cf[i] = 0.5f + 0.5f * std::tanh(c * R * P.curvStrength * 0.15f);
            }
        }
        // haluskan sedikit (box 3x3, 2 kali)
        for (int pass = 0; pass < 2; pass++) {
            std::vector<float> tmp = cf;
            for (int y = 0; y < sz; y++) {
                for (int x = 0; x < sz; x++) {
                    size_t i = (size_t)y * sz + x;
                    if (!cover[i]) continue;
                    float sum = 0.0f; int cnt = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            if (ok(x + dx, y + dy)) { sum += cf[(size_t)(y + dy) * sz + (x + dx)]; cnt++; }
                    if (cnt > 0) tmp[i] = sum / (float)cnt;
                }
            }
            cf.swap(tmp);
        }
        J->curvOut.assign(n, 128);
        for (size_t i = 0; i < n; i++) J->curvOut[i] = (unsigned char)(glm::clamp(cf[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        if (!J->cancel) {
            J->stage = 3;
            dilateU8(J->curvOut, cover, sz, std::max(4, sz / 128));
            J->hasCurv = true;
        }
    }
    J->done = true;
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

// Tombol toggle: biru saat aktif
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
    GLuint gbufProg = makeProgram(kPaintVS, kGBufFS);

    Mesh mesh; Camera cam; Canvas canvas;
    canvas.maskProg = makeProgram(kPaintVS, kMaskFS);
    canvas.compProg = makeProgram(kFullVS, kCompFS);
    canvas.dilProg = makeProgram(kFullVS, kDilFS);
    glGenVertexArrays(1, &canvas.emptyVao);

    // texture dummy 1x1 untuk AO / curvature saat belum ada hasil bake
    GLuint dummyWhite = makeTex(1, GL_R8, GL_RED, false);
    { unsigned char w = 255; glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &w); }
    GLuint dummyGray = makeTex(1, GL_R8, GL_RED, false);
    { unsigned char g = 128; glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &g); }

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

    std::string status = "Drag & drop file .obj ke window, atau File > Open OBJ.";
    bool wire = false, showGrid = true, gridAtBase = true, useAO = true;

    bool paintMode = true, eraser = false, symmetry = false;
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
    const char* viewNames[6] = {"Base Color", "PBR", "Roughness", "Metallic", "AO (bake)", "Curvature (bake)"};
    const char* qualNames[3] = {"Rendah (16 sampel)", "Sedang (32 sampel)", "Tinggi (64 sampel)"};
    int sizeIdx = 1, viewMode = 0, bakeQ = 1;
    BakeParams bp;
    canvas.create(sizeVals[sizeIdx]);

    BakeJob job;
    BakeMaps bakeMaps;

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
            canvas.create(sizeVals[sizeIdx]);
            canvas.buildMask(mesh);
        }
    };
    auto startBake = [&]() {
        if (job.running || !mesh.vao || !mesh.hasUV) return;
        if (!bp.ao && !bp.curv) { status = "Pilih AO atau Curvature dulu."; return; }
        bakeStop();
        bp.samples = bakeQ == 0 ? 16 : (bakeQ == 1 ? 32 : 64);
        job.P = bp;
        job.size = canvas.size;
        job.modelRadius = std::max(glm::length(mesh.bmax - mesh.bmin) * 0.5f, 1e-5f);
        renderGBuffer(mesh, gbufProg, job.size, job.pos, job.nrm);
        job.tris = mesh.cpu;
        job.aoOut.clear(); job.curvOut.clear();
        job.hasAO = job.hasCurv = false;
        job.cancel = false; job.done = false; job.rowsDone = 0; job.stage = 0; job.running = true;
        job.master = std::thread(bakeMain, &job);
        status = "Bake berjalan...";
    };
    auto finishBake = [&]() {
        if (job.master.joinable()) job.master.join();
        job.done = false; job.running = false;
        if (job.cancel) { status = "Bake dibatalkan."; return; }
        bakeMaps.set(job.size, job.hasAO ? &job.aoOut : nullptr, job.hasCurv ? &job.curvOut : nullptr);
        job.pos.clear(); job.pos.shrink_to_fit(); job.nrm.clear(); job.nrm.shrink_to_fit();
        job.tris.clear(); job.tris.shrink_to_fit();
        status = "Bake selesai.";
        if (job.hasAO) viewMode = 4;
        else if (job.hasCurv) viewMode = 5;
    };
    auto bakeToLayer = [&](const std::vector<unsigned char>& src, int blend, float op, const char* name) {
        if (src.empty() || bakeMaps.size != canvas.size) { status = "Hasil bake tidak cocok dengan resolusi texture."; return; }
        if (canvas.layers.size() >= Canvas::kMaxLayers) { status = "Layer penuh (maks 6)."; return; }
        canvas.addLayer();
        Layer& L = canvas.layers[canvas.active];
        std::vector<unsigned char> rgba(src.size() * 4);
        for (size_t i = 0; i < src.size(); i++) {
            rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = src[i]; rgba[4 * i + 3] = 255;
        }
        canvas.writeTex(L.tex, rgba);
        L.blend = blend; L.opacity = op; L.name = name;
        canvas.dirty = true;
        status = std::string("Ditambahkan layer: ") + name;
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
            bool useBakedAO = bakeMaps.hasAO && bakeFits;
            for (size_t i = 0; i < (size_t)sz * sz; i++) {
                orm[3 * i] = useBakedAO ? bakeMaps.aoPx[i] : 255;
                orm[3 * i + 1] = rough[i]; orm[3 * i + 2] = metal[i];
            }
            ok = stbi_write_png((stem + "_ORM.png").c_str(), sz, sz, 3, orm.data(), sz * 3) != 0;
        }
        if (ok && exportBake && bakeFits) {
            if (bakeMaps.hasAO) ok = ok && stbi_write_png((stem + "_AO.png").c_str(), sz, sz, 1, bakeMaps.aoPx.data(), sz) != 0;
            if (bakeMaps.hasCurv) ok = ok && stbi_write_png((stem + "_Curvature.png").c_str(), sz, sz, 1, bakeMaps.curvPx.data(), sz) != 0;
        }
        status = ok ? ("Export selesai: " + stem + "_*.png") : "Gagal menyimpan PNG.";
    };
    if (argc > 1) tryLoad(argv[1]);

    const ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (!g_dropped.empty()) { tryLoad(g_dropped); g_dropped.clear(); }
        if (job.done.load()) finishBake();

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // ---- layout: panel kiri, panel kanan, status bawah, viewport di tengah ----
        const float winW = io.DisplaySize.x, winH = io.DisplaySize.y;
        const float menuH = ImGui::GetFrameHeight();
        const float leftW = 330.0f, rightW = 270.0f, statusH = 30.0f;
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

        // ---- matriks ----
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
            return raycast(mesh, a, glm::normalize(b - a));
        };

        // ---- shortcut ----
        if (!io.WantTextInput) {
            if (io.KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) canvas.redo(); else canvas.undo(); }
                if (ImGui::IsKeyPressed(ImGuiKey_Y)) canvas.redo();
                if (ImGui::IsKeyPressed(ImGuiKey_O, false)) tryLoad(openFileDialog());
            } else {
                if (ImGui::IsKeyPressed(ImGuiKey_E, false)) eraser = !eraser;
                if (ImGui::IsKeyPressed(ImGuiKey_X, false)) symmetry = !symmetry;
                if (ImGui::IsKeyPressed(ImGuiKey_F, false) && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) brushSizePct = std::max(0.2f, brushSizePct * 0.85f);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) brushSizePct = std::min(30.0f, brushSizePct * 1.18f);
            }
        }

        // ---- painting ----
        float modelRadius = glm::length(mesh.bmax - mesh.bmin) * 0.5f;
        float radiusWorld = std::max(brushSizePct / 100.0f * modelRadius, 1e-5f);
        bool anyChannel = chColor || chRough || chMetal;
        bool canPaint = paintMode && mesh.vao && mesh.hasUV && !io.KeyAlt && !io.KeyShift
                        && anyChannel && !canvas.layers.empty() && canvas.layers[canvas.active].visible;
        bool mouseOk = mouseInView && !io.WantCaptureMouse;
        glm::vec2 mp(io.MousePos.x * sx, io.MousePos.y * sy);

        float distRef = cam.dist;
        if (canPaint && mouseOk) {
            Hit hv = pickAt(mp);
            if (hv.ok) distRef = glm::length(hv.p - cam.eye());
        }
        float brushPx = radiusWorld * ((float)vph * 0.5f) / (std::max(distRef, 1e-4f) * std::tan(fov * 0.5f));

        auto paintStamp = [&](const glm::vec3& p, const glm::vec3& n) {
            if (!strokeSnap) { canvas.pushUndo(); strokeSnap = true; }
            glBindFramebuffer(GL_FRAMEBUFFER, canvas.layers[canvas.active].fbo);
            glViewport(0, 0, canvas.size, canvas.size);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            if (eraser) glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
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

        if (canPaint && mouseOk) {
            ImGui::GetForegroundDrawList()->AddCircle(io.MousePos, brushPx / sx,
                IM_COL32(255, 255, 255, 220), 48, 1.5f);
        }

        // ================= UI =================
        // ---- menu bar ----
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open OBJ...", "Ctrl+O")) tryLoad(openFileDialog());
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
                ImGui::SeparatorText("Mode");
                ImGui::Checkbox("Mode Paint", &paintMode);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aktif: klik kiri = cat, Alt+Kiri = orbit");
                ImGui::SeparatorText("Kanal yang dicat");
                float sp = ImGui::GetStyle().ItemSpacing.x;
                float bw = (ImGui::GetContentRegionAvail().x - sp * 2.0f) / 3.0f;
                toggleBtn("Color", &chColor, bw); ImGui::SameLine();
                toggleBtn("Rough", &chRough, bw); ImGui::SameLine();
                toggleBtn("Metal", &chMetal, bw);
                if (!anyChannel) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Pilih minimal 1 kanal.");
                ImGui::SeparatorText("Nilai");
                if (chColor) ImGui::ColorEdit3("Warna", brushColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
                if (chRough) ImGui::SliderFloat("Roughness", &valRough, 0.0f, 1.0f, "%.2f");
                if (chMetal) ImGui::SliderFloat("Metallic", &valMetal, 0.0f, 1.0f, "%.2f");
                ImGui::SeparatorText("Brush");
                ImGui::SliderFloat("Ukuran", &brushSizePct, 0.2f, 30.0f, "%.1f %%");
                ImGui::SliderFloat("Hardness", &hardness, 0.0f, 0.99f, "%.2f");
                ImGui::SliderFloat("Opacity", &opacity, 0.05f, 1.0f, "%.2f");
                float bw2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
                toggleBtn("Eraser (E)", &eraser, bw2); ImGui::SameLine();
                toggleBtn("Symmetry X (X)", &symmetry, bw2);
                ImGui::EndTabItem();
            }
            // ------------------------------------------------ Bake
            if (ImGui::BeginTabItem("Bake")) {
                bool running = job.running.load();
                bool canBake = mesh.vao && mesh.hasUV && !running;
                ImGui::SeparatorText("Map");
                ImGui::Checkbox("Ambient Occlusion", &bp.ao);
                ImGui::Checkbox("Curvature", &bp.curv);
                if (bp.ao) {
                    ImGui::SeparatorText("Pengaturan AO");
                    ImGui::Combo("Kualitas", &bakeQ, qualNames, 3);
                    ImGui::SliderFloat("Jarak", &bp.distPct, 2.0f, 100.0f, "%.0f %%");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jarak maksimum penghalang, relatif ke ukuran model");
                    ImGui::SliderFloat("Kontras", &bp.power, 0.5f, 3.0f, "%.2f");
                    ImGui::Checkbox("Balik normal", &bp.flip);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Centang jika hasil AO terlihat terbalik/hitam semua");
                }
                if (bp.curv) {
                    ImGui::SeparatorText("Pengaturan Curvature");
                    ImGui::SliderInt("Radius", &bp.curvRadius, 1, 8, "%d px");
                    ImGui::SliderFloat("Kekuatan", &bp.curvStrength, 0.2f, 10.0f, "%.1f");
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Resolusi bake = resolusi texture (%d px)", canvas.size);
                if (!mesh.vao) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Load model dulu.");
                else if (!mesh.hasUV) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Model tidak punya UV.");
                if (!canBake) ImGui::BeginDisabled();
                if (ImGui::Button("Bake", ImVec2(-1, 34))) startBake();
                if (!canBake) ImGui::EndDisabled();
                if (running) {
                    int st = job.stage.load();
                    const char* stName = st == 0 ? "Membangun BVH..." : (st == 1 ? "Menghitung AO..." : (st == 2 ? "Menghitung curvature..." : "Padding seam..."));
                    float frac = st == 1 ? (float)job.rowsDone.load() / (float)std::max(1, job.size) : (st == 0 ? 0.0f : 1.0f);
                    ImGui::ProgressBar(frac, ImVec2(-1, 0), stName);
                    if (ImGui::Button("Batal", ImVec2(-1, 0))) job.cancel = true;
                }
                ImGui::SeparatorText("Hasil");
                if (!bakeMaps.hasAO && !bakeMaps.hasCurv) ImGui::TextDisabled("Belum ada hasil bake.");
                if (bakeMaps.hasAO) {
                    ImGui::Text("AO: siap (%d px)", bakeMaps.size);
                    if (ImGui::Button("AO -> Layer (Multiply)", ImVec2(-1, 0))) bakeToLayer(bakeMaps.aoPx, 1, 1.0f, "AO");
                }
                if (bakeMaps.hasCurv) {
                    ImGui::Text("Curvature: siap (%d px)", bakeMaps.size);
                    if (ImGui::Button("Curvature -> Layer (Overlay)", ImVec2(-1, 0))) bakeToLayer(bakeMaps.curvPx, 3, 0.5f, "Curvature");
                }
                if (bakeMaps.hasAO || bakeMaps.hasCurv) {
                    ImGui::Checkbox("Pakai AO di preview", &useAO);
                    if (ImGui::Button("Hapus hasil bake", ImVec2(-1, 0))) bakeMaps.clear();
                }
                ImGui::EndTabItem();
            }
            // ------------------------------------------------ Texture / Export
            if (ImGui::BeginTabItem("Texture")) {
                ImGui::SeparatorText("Resolusi");
                if (ImGui::Combo("Ukuran", &sizeIdx, sizeNames, 3)) {
                    bakeStop();
                    bakeMaps.clear();
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
                ImGui::Checkbox("Sertakan AO & Curvature (bake)", &exportBake);
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
            if (ImGui::SliderFloat("Opacity", &A.opacity, 0.0f, 1.0f, "%.2f")) canvas.dirty = true;
            if (ImGui::Combo("Blend", &A.blend, blendNames, 5)) canvas.dirty = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend mode berlaku untuk kanal Color");
        }
        {
            float sp = ImGui::GetStyle().ItemSpacing.x;
            float w4 = (ImGui::GetContentRegionAvail().x - sp * 3.0f) / 4.0f;
            if (ImGui::Button("Add", ImVec2(w4, 0))) canvas.addLayer();
            ImGui::SameLine();
            if (ImGui::Button("Del", ImVec2(w4, 0))) canvas.deleteActive();
            ImGui::SameLine();
            if (ImGui::Button("Up", ImVec2(w4, 0))) canvas.moveActive(+1);
            ImGui::SameLine();
            if (ImGui::Button("Down", ImVec2(w4, 0))) canvas.moveActive(-1);
            float w2 = (ImGui::GetContentRegionAvail().x - sp) / 2.0f;
            if (ImGui::Button("Fill", ImVec2(w2, 0))) {
                canvas.pushUndo();
                const float col[4] = {brushColor[0], brushColor[1], brushColor[2], 1.0f};
                const float pbr[4] = {valRough, 1.0f, valMetal, 1.0f};
                canvas.clearChannels(canvas.layers[canvas.active], chColor, chRough, chMetal, col, pbr);
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(w2, 0))) {
                canvas.pushUndo();
                const float z[4] = {0, 0, 0, 0};
                canvas.clearChannels(canvas.layers[canvas.active], chColor, chRough, chMetal, z, z);
            }
            ImGui::TextDisabled("Fill/Clear: kanal yang aktif saja");
            if (ImGui::Button("Undo", ImVec2(w2, 0))) canvas.undo();
            ImGui::SameLine();
            if (ImGui::Button("Redo", ImVec2(w2, 0))) canvas.redo();
        }
        ImGui::SeparatorText("Preview");
        ImGui::Combo("Mode", &viewMode, viewNames, 6);
        if ((viewMode == 4 && !bakeMaps.hasAO) || (viewMode == 5 && !bakeMaps.hasCurv))
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Belum di-bake (tab Bake).");
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

        // ---- komposit layer ----
        bool busy = stroke || ImGui::IsAnyItemActive();
        if (canvas.dirty) { canvas.compose(baseColor, baseRough, baseMetal, !busy); undilated = busy; }
        else if (undilated && !busy) { canvas.compose(baseColor, baseRough, baseMetal, true); undilated = false; }

        // ---- render 3D (di area tengah) ----
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
            glUniformMatrix4fv(glGetUniformLocation(meshProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3fv(glGetUniformLocation(meshProg, "uEye"), 1, glm::value_ptr(eye));
            glUniform3fv(glGetUniformLocation(meshProg, "uLight"), 1, glm::value_ptr(light));
            glUniform1i(glGetUniformLocation(meshProg, "uView"), viewMode);
            glUniform1i(glGetUniformLocation(meshProg, "uUseAO"), (useAO && bakeMaps.hasAO && bakeMaps.size > 0) ? 1 : 0);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, canvas.dispC);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, canvas.dispP);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, bakeMaps.hasAO ? bakeMaps.ao : dummyWhite);
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, bakeMaps.hasCurv ? bakeMaps.curv : dummyGray);
            glUniform1i(glGetUniformLocation(meshProg, "uTex"), 0);
            glUniform1i(glGetUniformLocation(meshProg, "uTexP"), 1);
            glUniform1i(glGetUniformLocation(meshProg, "uTexAO"), 2);
            glUniform1i(glGetUniformLocation(meshProg, "uTexCv"), 3);
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
    mesh.destroy(); canvas.destroy(); bakeMaps.clear();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
