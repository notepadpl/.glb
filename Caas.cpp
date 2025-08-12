#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <iostream>
#include "tiny_gltf.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

SDL_Window* window = nullptr;
SDL_GLContext context;

struct MeshGL {
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

MeshGL meshGL;
GLuint shaderProgram;
GLint attrPositionLoc;
GLint uniformMVPLoc;

// Kompilacja shadera
GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Tworzenie programu shaderowego
GLuint CreateShaderProgram() {
    const char* vertexSrc = R"(
        attribute vec3 a_position;
        uniform mat4 u_mvp;
        void main() {
            gl_Position = u_mvp * vec4(a_position, 1.0);
        }
    )";

    const char* fragmentSrc = R"(
        precision mediump float;
        void main() {
            gl_FragColor = vec4(1.0);
        }
    )";

    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glLinkProgram(program);

    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program, 512, nullptr, log);
        std::cerr << "Program link error: " << log << std::endl;
        glDeleteProgram(program);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// Załaduj mesh z tinygltf i utwórz buffory OpenGL
bool LoadMeshToOpenGL(const tinygltf::Model& model) {
    if (model.meshes.empty()) {
        std::cerr << "Model has no meshes\n";
        return false;
    }

    const tinygltf::Mesh& mesh = model.meshes[0];
    if (mesh.primitives.empty()) {
        std::cerr << "Mesh has no primitives\n";
        return false;
    }

    const tinygltf::Primitive& primitive = mesh.primitives[0];

    // Pobierz pozycje wierzchołków
    auto posAccessorIt = primitive.attributes.find("POSITION");
    if (posAccessorIt == primitive.attributes.end()) {
        std::cerr << "No POSITION attribute\n";
        return false;
    }
    const tinygltf::Accessor& posAccessor = model.accessors[posAccessorIt->second];
    const tinygltf::BufferView& posView = model.bufferViews[posAccessor.bufferView];
    const tinygltf::Buffer& posBuffer = model.buffers[posView.buffer];

    const float* positions = reinterpret_cast<const float*>(
        &posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
    int vertexCount = posAccessor.count;

    // Pobierz indeksy
    const tinygltf::Accessor& indexAccessor = model.accessors[primitive.indices];
    const tinygltf::BufferView& indexView = model.bufferViews[indexAccessor.bufferView];
    const tinygltf::Buffer& indexBuffer = model.buffers[indexView.buffer];

    // Załóżmy unsigned short (GL_UNSIGNED_SHORT)
    const unsigned short* indices = reinterpret_cast<const unsigned short*>(
        &indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);
    int indexCount = indexAccessor.count;

    meshGL.indexCount = indexCount;

    // Buffory OpenGL
    glGenBuffers(1, &meshGL.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, meshGL.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertexCount * 3 * sizeof(float), positions, GL_STATIC_DRAW);

    glGenBuffers(1, &meshGL.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshGL.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCount * sizeof(unsigned short), indices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    return true;
}

void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            emscripten_cancel_main_loop();
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);

    // Ustaw macierze (używam GLM)
    glm::mat4 projection = glm::ortho(-1.f, 1.f, -1.f, 1.f, 0.1f, 10.f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 2), glm::vec3(0), glm::vec3(0,1,0));
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = projection * view * model;

    glUniformMatrix4fv(uniformMVPLoc, 1, GL_FALSE, &mvp[0][0]);

    glBindBuffer(GL_ARRAY_BUFFER, meshGL.vbo);
    glEnableVertexAttribArray(attrPositionLoc);
    glVertexAttribPointer(attrPositionLoc, 3, GL_FLOAT, GL_FALSE, 0, 0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshGL.ebo);

    glDrawElements(GL_TRIANGLES, meshGL.indexCount, GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(attrPositionLoc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    SDL_GL_SwapWindow(window);
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    window = SDL_CreateWindow("GLB + SDL + Emscripten",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              800, 600, SDL_WINDOW_OPENGL);
    if (!window) {
        std::cerr << "Window creation failed." << std::endl;
        return 1;
    }

    context = SDL_GL_CreateContext(window);
    if (!context) {
        std::cerr << "GL context creation failed." << std::endl;
        return 1;
    }
int width, height;
SDL_GetWindowSize(window, &width, &height);
glViewport(0, 0, width, height);

    // Załaduj model GLB
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, "asserts/vr_room_light_baked.glb")) {
        std::cerr << "Failed to load GLB: " << err << std::endl;
        return 1;
    }
    if (!warn.empty()) std::cout << "Warning: " << warn << std::endl;

    shaderProgram = CreateShaderProgram();
    attrPositionLoc = 0; // przypisane w BindAttribLocation
    uniformMVPLoc = glGetUniformLocation(shaderProgram, "u_mvp");

    if (!LoadMeshToOpenGL(model)) {
        std::cerr << "Failed to load mesh data\n";
        return 1;
    }

    glEnable(GL_DEPTH_TEST);

    emscripten_set_main_loop(main_loop, 0, true);

    return 0;
}
