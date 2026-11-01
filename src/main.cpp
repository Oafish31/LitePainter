// LitePainter - Tahap 1: viewport 3D + load OBJ
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

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

// ---------------------------------------------------------------- shader
static const char* kMeshVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
out vec3 vN; out vec2 vUV;
void main(){ vN = aNrm; vUV = aUV; gl_Position = uMVP * vec4(aPos,1.0); }
)";
static const char* kMeshFS = R"(#version 330 core
in vec3 vN; in vec2 vUV;
uniform vec3 uLight; uniform vec3 uColor;
out vec4 frag;
void main(){
  vec3 n = normalize(vN);
  float d = max(dot(n, normalize(uLight)), 0.0);
  float h = 0.5 + 0.5 * n.y;               // cahaya langit sederhana
  vec3 c = uColor * (0.25 * h + 0.75 * d);
  frag = vec4(pow(c, vec3(1.0/2.2)), 1.0);
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

// ---------------------------------------------------------------- mesh
struct Vertex { glm::vec3 p; glm::vec3 n; glm::vec2 uv; };

struct Mesh {
    GLuint vao = 0, vbo = 0;
    int count = 0;
    glm::vec3 bmin{0}, bmax{0};
    int triangles = 0;

    void destroy() {
        if (vbo) glDeleteBuffers(1, &vbo);
        if (vao) glDeleteVertexArrays(1, &vao);
        vao = vbo = 0; count = 0;
    }
    void upload(const std::vector<Vertex>& v) {
        destroy();
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
    msg = "Loaded: " + path;
    return true;
}

// ---------------------------------------------------------------- camera
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

// ---------------------------------------------------------------- misc
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

    Mesh mesh; Camera cam;
    std::string status = "Drag & drop file .obj ke window, atau klik Open OBJ.";
    bool wire = false, showGrid = true;
    float color[3] = {0.8f, 0.8f, 0.8f};

    auto tryLoad = [&](const std::string& p) {
        if (p.empty()) return;
        if (loadOBJ(p, mesh, status)) cam.fit(mesh.bmin, mesh.bmax);
    };
    if (argc > 1) tryLoad(argv[1]);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (!g_dropped.empty()) { tryLoad(g_dropped); g_dropped.clear(); }

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // ---- input kamera ----
        if (!io.WantCaptureMouse) {
            bool l = ImGui::IsMouseDown(0), m = ImGui::IsMouseDown(2), r = ImGui::IsMouseDown(1);
            bool shift = io.KeyShift;
            if (l && !shift) {
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

        // ---- UI ----
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::Begin("LitePainter");
        if (ImGui::Button("Open OBJ...")) tryLoad(openFileDialog());
        ImGui::TextWrapped("%s", status.c_str());
        if (mesh.vao) ImGui::Text("Triangles: %d", mesh.triangles);
        ImGui::Separator();
        ImGui::Checkbox("Wireframe", &wire);
        ImGui::Checkbox("Grid", &showGrid);
        ImGui::ColorEdit3("Warna", color);
        if (ImGui::Button("Reset kamera") && mesh.vao) cam.fit(mesh.bmin, mesh.bmax);
        ImGui::Separator();
        ImGui::TextDisabled("Kiri: orbit | Kanan/Tengah/Shift+Kiri: pan | Scroll: zoom");
        ImGui::End();

        // ---- render ----
        int w, h; glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.16f, 0.17f, 0.19f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        float aspect = h > 0 ? (float)w / h : 1.0f;
        float nearP = std::max(cam.dist * 0.01f, 0.001f);
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, nearP, cam.dist * 100.0f);
        glm::mat4 mvp = proj * cam.view();

        if (showGrid) {
            glUseProgram(lineProg);
            glUniformMatrix4fv(glGetUniformLocation(lineProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3f(glGetUniformLocation(lineProg, "uColor"), 0.3f, 0.3f, 0.33f);
            glBindVertexArray(gvao); glDrawArrays(GL_LINES, 0, (GLsizei)gl.size()); glBindVertexArray(0);
        }

        if (mesh.vao) {
            glUseProgram(meshProg);
            glm::vec3 light = (cam.eye() - cam.target) + glm::vec3(0, cam.dist * 0.5f, 0);
            glUniformMatrix4fv(glGetUniformLocation(meshProg, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform3fv(glGetUniformLocation(meshProg, "uLight"), 1, glm::value_ptr(light));
            glUniform3fv(glGetUniformLocation(meshProg, "uColor"), 1, color);
            glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
            mesh.draw();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    mesh.destroy();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
