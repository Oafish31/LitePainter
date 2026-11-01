// LitePainter - Tahap 2: painting langsung di model (Base Color)
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
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
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
static const char* kMeshFS = R"(#version 330 core
in vec3 vP; in vec3 vN; in vec2 vUV;
uniform vec3 uLight; uniform vec3 uEye; uniform sampler2D uTex;
out vec4 frag;
void main(){
  vec3 n = normalize(vN);
  if (dot(n, normalize(uEye - vP)) < 0.0) n = -n;   // lighting dua sisi
  float d = max(dot(n, normalize(uLight)), 0.0);
  float h = 0.5 + 0.5 * n.y;
  vec3 base = texture(uTex, vUV).rgb;
  frag = vec4(base * (0.3 * h + 0.7 * d), 1.0);
}
)";
static const char* kLineVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main(){ gl_Position = uMVP * vec4(aPos,1.0); }
)";
static const char* kLineFS = R"(#version 330 core
uniform vec3 uColor; out vec4 frag;
void main(){ frag = vec4(uColor,1.0); }
)";
// Paint pass: mesh digambar di ruang UV, jarak diukur di ruang 3D
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
out vec4 frag;
void main(){
  float d = distance(vP, uHit) / uRadius;
  if (d >= 1.0) discard;
  if (dot(normalize(vN), uHitN) < 0.1) discard;     // jangan tembus ke sisi belakang
  float a = 1.0 - smoothstep(uHardness, 1.0, d);
  frag = vec4(uColor, a * uOpacity);
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
    std::vector<Vertex> cpu;   // salinan CPU untuk raycast

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

// ================================================================ canvas (texture + undo)
struct Canvas {
    GLuint tex = 0, fbo = 0;
    int size = 0;
    bool dirty = false;
    std::vector<std::vector<unsigned char>> undoS, redoS;

    size_t maxStates() const { return size >= 2048 ? 10 : (size >= 1024 ? 25 : 40); }

    void destroy() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        fbo = tex = 0; undoS.clear(); redoS.clear();
    }
    void create(int sz, const float c[3]) {
        destroy();
        size = sz;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fill(c[0], c[1], c[2]);
        mips();
    }
    void fill(float r, float g, float b) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, size, size);
        glDisable(GL_BLEND);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        dirty = true;
    }
    void mips() { glBindTexture(GL_TEXTURE_2D, tex); glGenerateMipmap(GL_TEXTURE_2D); dirty = false; }
    void read(std::vector<unsigned char>& out) {
        out.resize((size_t)size * size * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    void write(const std::vector<unsigned char>& in) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, in.data());
        dirty = true;
    }
    void pushUndo() {
        undoS.emplace_back(); read(undoS.back());
        redoS.clear();
        if (undoS.size() > maxStates()) undoS.erase(undoS.begin());
    }
    void undo() {
        if (undoS.empty()) return;
        redoS.emplace_back(); read(redoS.back());
        write(undoS.back()); undoS.pop_back();
    }
    void redo() {
        if (redoS.empty()) return;
        undoS.emplace_back(); read(undoS.back());
        write(redoS.back()); redoS.pop_back();
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

    Mesh mesh; Camera cam; Canvas canvas;
    std::string status = "Drag & drop file .obj ke window, atau klik Open OBJ.";
    bool wire = false, showGrid = true;

    // brush & canvas settings
    bool paintMode = true, eraser = false, symmetry = false;
    float brushColor[3] = {0.85f, 0.2f, 0.2f};
    float baseColor[3] = {0.8f, 0.8f, 0.8f};
    float brushSizePct = 5.0f, hardness = 0.5f, opacity = 1.0f;
    const char* sizeNames[3] = {"512", "1024", "2048"};
    const int sizeVals[3] = {512, 1024, 2048};
    int sizeIdx = 1;
    canvas.create(sizeVals[sizeIdx], baseColor);

    bool stroke = false, strokeSnap = false;
    glm::vec2 lastMp(0);

    auto tryLoad = [&](const std::string& p) {
        if (p.empty()) return;
        if (loadOBJ(p, mesh, status)) {
            cam.fit(mesh.bmin, mesh.bmax);
            canvas.create(sizeVals[sizeIdx], baseColor);
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
        bool canPaint = paintMode && mesh.vao && mesh.hasUV && !io.KeyAlt && !io.KeyShift;
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
            glBindFramebuffer(GL_FRAMEBUFFER, canvas.fbo);
            glViewport(0, 0, canvas.size, canvas.size);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            glUseProgram(paintProg);
            const float* col = eraser ? baseColor : brushColor;
            glUniform3fv(glGetUniformLocation(paintProg, "uColor"), 1, col);
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
            } else stroke = false;
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
        ImGui::ColorEdit3("Warna", brushColor);
        ImGui::SliderFloat("Ukuran (%)", &brushSizePct, 0.2f, 30.0f);
        ImGui::SliderFloat("Hardness", &hardness, 0.0f, 0.99f);
        ImGui::SliderFloat("Opacity", &opacity, 0.05f, 1.0f);
        ImGui::Checkbox("Eraser", &eraser);
        ImGui::SameLine();
        ImGui::Checkbox("Symmetry X", &symmetry);

        ImGui::SeparatorText("Texture");
        if (ImGui::Combo("Resolusi", &sizeIdx, sizeNames, 3)) canvas.create(sizeVals[sizeIdx], baseColor);
        ImGui::ColorEdit3("Warna dasar", baseColor);
        if (ImGui::Button("Fill brush")) { canvas.pushUndo(); canvas.fill(brushColor[0], brushColor[1], brushColor[2]); }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { canvas.pushUndo(); canvas.fill(baseColor[0], baseColor[1], baseColor[2]); }
        if (ImGui::Button("Undo")) canvas.undo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) canvas.redo();
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", (int)canvas.undoS.size());
        if (ImGui::Button("Export PNG...")) {
            std::string path = saveFileDialog();
            if (!path.empty()) {
                std::vector<unsigned char> px; canvas.read(px);
                stbi_flip_vertically_on_write(1);
                if (stbi_write_png(path.c_str(), canvas.size, canvas.size, 4, px.data(), canvas.size * 4))
                    status = "Tersimpan: " + path;
                else status = "Gagal menyimpan PNG.";
            }
        }

        ImGui::SeparatorText("View");
        ImGui::Checkbox("Wireframe", &wire);
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &showGrid);
        if (ImGui::Button("Reset kamera") && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
        ImGui::TextDisabled("Kanan/Tengah/Shift+Kiri: pan | Scroll: zoom | Ctrl+Z/Y");
        ImGui::End();

        // ---- render ----
        if (canvas.dirty) canvas.mips();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbw, fbh);
        glDisable(GL_BLEND);
        glClearColor(0.16f, 0.17f, 0.19f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        if (showGrid) {
            glUseProgram(lineProg);
            glUniformMatrix4fv(glGetUniformLocation(lineProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3f(glGetUniformLocation(lineProg, "uColor"), 0.3f, 0.3f, 0.33f);
            glBindVertexArray(gvao); glDrawArrays(GL_LINES, 0, (GLsizei)gl.size()); glBindVertexArray(0);
        }

        if (mesh.vao) {
            glUseProgram(meshProg);
            glm::vec3 eye = cam.eye();
            glm::vec3 light = (eye - cam.target) + glm::vec3(0, cam.dist * 0.5f, 0);
            glUniformMatrix4fv(glGetUniformLocation(meshProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3fv(glGetUniformLocation(meshProg, "uLight"), 1, glm::value_ptr(light));
            glUniform3fv(glGetUniformLocation(meshProg, "uEye"), 1, glm::value_ptr(eye));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, canvas.tex);
            glUniform1i(glGetUniformLocation(meshProg, "uTex"), 0);
            glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
            mesh.draw();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
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
