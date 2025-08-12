#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <emscripten.h>
#include <iostream>
#include <vector>

#include "tiny_gltf.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

SDL_Window* window = nullptr;
SDL_GLContext context;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texcoord;
};

struct MeshGL {
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

MeshGL meshGL;
GLuint shaderProgram;
GLuint textureID;

GLint attrPositionLoc;
GLint attrNormalLoc;
GLint attrTexcoordLoc;
GLint uniformMVPLoc;
GLint uniformModelLoc;
GLint uniformTextureLoc;

// Kompilacja shaderów
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
        attribute vec3 a_normal;
        attribute vec2 a_texcoord;

        uniform mat4 u_mvp;
        uniform mat4 u_model;

        varying vec3 v_normal;
        varying vec2 v_texcoord;

        void main() {
            gl_Position = u_mvp * vec4(a_position, 1.0);
            v_normal = mat3(u_model) * a_normal;
            v_texcoord = a_texcoord;
        }
    )";

    const char* fragmentSrc = R"(
        precision mediump float;

        varying vec3 v_normal;
        varying vec2 v_texcoord;

        uniform sampler2D u_texture;

        void main() {
            vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
            float light = max(dot(normalize(v_normal), lightDir), 0.0);
            vec4 texColor = texture2D(u_texture, v_texcoord);
            gl_FragColor = vec4(texColor.rgb * light, texColor.a);
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

// Ładowanie tekstury z GLTF
GLuint LoadTextureFromGLTF(const tinygltf::Model& model, int textureIndex) {
    const auto& texture = model.textures[textureIndex];
    const auto& image = model.images[texture.source];

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 image.width, image.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, image.image.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}

// Wczytaj dane z GLTF
bool LoadMeshToOpenGL(const tinygltf::Model& model) {
    if (model.meshes.empty()) return false;

    const auto& mesh = model.meshes[0];
    const auto& primitive = mesh.primitives[0];

    int posIndex = primitive.attributes.at("POSITION");
    int normIndex = primitive.attributes.at("NORMAL");
    int texIndex = primitive.attributes.at("TEXCOORD_0");

    const auto& posAccessor = model.accessors[posIndex];
    const auto& normAccessor = model.accessors[normIndex];
    const auto& texAccessor = model.accessors[texIndex];

    const auto& posView = model.bufferViews[posAccessor.bufferView];
    const auto& normView = model.bufferViews[normAccessor.bufferView];
    const auto& texView = model.bufferViews[texAccessor.bufferView];

    const auto& posBuffer = model.buffers[posView.buffer];
    const auto& normBuffer = model.buffers[normView.buffer];
    const auto& texBuffer = model.buffers[texView.buffer];

    const float* positions = reinterpret_cast<const float*>(&posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
    const float* normals   = reinterpret_cast<const float*>(&normBuffer.data[normView.byteOffset + normAccessor.byteOffset]);
    const float* texcoords = reinterpret_cast<const float*>(&texBuffer.data[texView.byteOffset + texAccessor.byteOffset]);

    int vertexCount = posAccessor.count;
    std::vector<Vertex> vertices(vertexCount);

    for (int i = 0; i < vertexCount; ++i) {
        vertices[i].position = glm::vec3(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
        vertices[i].normal   = glm::vec3(normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]);
        vertices[i].texcoord = glm::vec2(texcoords[i * 2 + 0], texcoords[i * 2 + 1]);
    }

    const auto& indexAccessor = model.accessors[primitive.indices];
    const auto& indexView = model.bufferViews[indexAccessor.bufferView];
    const auto& indexBuffer = model.buffers[indexView.buffer];

    const unsigned short* indices = reinterpret_cast<const unsigned short*>(
        &indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);

    meshGL.indexCount = indexAccessor.count;

    // Buffory
    glGenBuffers(1, &meshGL.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, meshGL.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &meshGL.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshGL.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned short) * meshGL.indexCount, indices, GL_STATIC_DRAW);

    return true;
}

// Pętla renderująca
void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            emscripten_cancel_main_loop();
    }

    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);

    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    glViewport(0, 0, width, height);

    glm::mat4 projection = glm::perspective(glm::radians(45.0f), width / (float)height, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 1, 3), glm::vec3(0), glm::vec3(0,1,0));
    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 mvp = projection * view * model;

    glUniformMatrix4fv(uniformMVPLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(uniformModelLoc, 1, GL_FALSE, glm::value_ptr(model));

    glBindBuffer(GL_ARRAY_BUFFER, meshGL.vbo);
    glEnableVertexAttribArray(attrPositionLoc);
    glVertexAttribPointer(attrPositionLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));

    glEnableVertexAttribArray(attrNormalLoc);
    glVertexAttribPointer(attrNormalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));

    glEnableVertexAttribArray(attrTexcoordLoc);
    glVertexAttribPointer(attrTexcoordLoc, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texcoord));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshGL.ebo);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glUniform1i(uniformTextureLoc, 0);

    glDrawElements(GL_TRIANGLES, meshGL.indexCount, GL_UNSIGNED_SHORT, 0);

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
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    window = SDL_CreateWindow("GLB Viewer with Lighting", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_OPENGL);
    if (!window) return 1;

    context = SDL_GL_CreateContext(window);
    if (!context) return 1;

    glEnable(GL_DEPTH_TEST);

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, "assets/vr_room_light_baked.glb")) {
        std::cerr << "Failed to load model: " << err << std::endl;
        return 1;
    }
    if (!warn.empty()) std::cout << "GLTF Warning: " << warn << std::endl;

    shaderProgram = CreateShaderProgram();

    attrPositionLoc = glGetAttribLocation(shaderProgram, "a_position");
    attrNormalLoc = glGetAttribLocation(shaderProgram, "a_normal");
    attrTexcoordLoc = glGetAttribLocation(shaderProgram, "a_texcoord");
    uniformMVPLoc = glGetUniformLocation(shaderProgram, "u_mvp");
    uniformModelLoc = glGetUniformLocation(shaderProgram, "u_model");
    uniformTextureLoc = glGetUniformLocation(shaderProgram, "u_texture");

    if (!LoadMeshToOpenGL(model)) return 1;

    // Ładowanie tekstury
    int texIndex = model.materials[0].pbrMetallicRoughness.baseColorTexture.index;
    textureID = LoadTextureFromGLTF(model, texIndex);

    emscripten_set_main_loop(main_loop, 0, true);

    return 0;
}
