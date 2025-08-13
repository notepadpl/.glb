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

// --- Globalne zmienne do obracania modelem ---
float rotX = 0, rotY = 0;
bool mouseDown = false;
int lastX, lastY;

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

struct ModelGL {
    std::vector<MeshGL> meshes;
    GLuint textureID = 0;
};

ModelGL myModel;
GLuint shaderProgram;

GLint attrPositionLoc;
GLint attrNormalLoc;
GLint attrTexcoordLoc;
GLint uniformMVPLoc;
GLint uniformModelLoc;
GLint uniformTextureLoc;
GLint uniformRotXLoc;
GLint uniformRotYLoc;

// --- Kompilacja i tworzenie programu shaderowego ---
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

GLuint CreateShaderProgram() {
    const char* vertexSrc = R"(
        attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_texcoord;

uniform mat4 u_mvp;
uniform mat4 u_model;
uniform float u_rotX;
uniform float u_rotY;

varying vec3 v_normal;
varying vec2 v_texcoord;

void main() {
    float cx = cos(u_rotX), sx = sin(u_rotX);
    float cy = cos(u_rotY), sy = sin(u_rotY);

    mat4 Rx = mat4(
        1.0, 0.0, 0.0, 0.0,
        0.0, cx,  -sx, 0.0,
        0.0, sx,  cx,  0.0,
        0.0, 0.0, 0.0, 1.0
    );
    mat4 Ry = mat4(
        cy, 0.0, sy, 0.0,
        0.0, 1.0, 0.0, 0.0,
        -sy, 0.0, cy, 0.0,
        0.0, 0.0, 0.0, 1.0
    );

    mat4 rotatedModel = Ry * Rx * u_model;

    gl_Position = u_mvp * rotatedModel * vec4(a_position, 1.0);

    // ✅ Znormalizowane normalne
    v_normal = normalize(mat3(rotatedModel) * a_normal);
    v_texcoord = a_texcoord;
}

    )";

    // Fragment Shader - Twoja oryginalna wersja z Assimp
const char* fragmentSrc = R"(
    precision mediump float;

uniform sampler2D tex;

varying vec3 v_normal;
varying vec2 v_texcoord;

void main() {
    vec3 texColor = texture2D(tex, v_texcoord).rgb;

    // ✅ Światło skierowane z kamery (Z+)
    vec3 lightDir = normalize(vec3(0.0, 0.0, 1.0));
    float diff = max(dot(normalize(v_normal), lightDir), 0.0);

    // ✅ Lekko podświetlona tekstura
    vec3 color = texColor * (0.3 + 0.7 * diff);

    gl_FragColor = vec4(color, 1.0);
}

)";

    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
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

// --- Ładowanie tekstury z GLTF ---
GLuint LoadTextureFromGLTF(const tinygltf::Model& model, int textureIndex) {
    if (textureIndex == -1 || textureIndex >= model.textures.size()) {
        std::cerr << "Niepoprawny indeks tekstury.\n";
        return 0;
    }

    const auto& texture = model.textures[textureIndex];
    if (texture.source < 0 || texture.source >= model.images.size()) {
        std::cerr << "Niepoprawny indeks zrodla obrazu.\n";
        return 0;
    }
    const auto& image = model.images[texture.source];

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 image.width, image.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, image.image.data());
    
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}

// --- Wczytywanie danych z GLTF ---
bool LoadModelToOpenGL(const tinygltf::Model& model, ModelGL& modelGL) {
    if (model.meshes.empty()) {
        std::cerr << "Brak meshy w modelu!\n";
        return false;
    }
    
    for (const auto& mesh : model.meshes) {
        if (mesh.primitives.empty()) {
            std::cerr << "Brak prymitywow w jednym z meshy!\n";
            continue;
        }

        for (const auto& primitive : mesh.primitives) {
            MeshGL newMesh;

            auto findAccessorIndex = [&](const std::string& name) -> int {
                auto it = primitive.attributes.find(name);
                return (it != primitive.attributes.end()) ? it->second : -1;
            };

            int posIndex = findAccessorIndex("POSITION");
            int normIndex = findAccessorIndex("NORMAL");
            int texIndex = findAccessorIndex("TEXCOORD_0");

            if (posIndex == -1 || primitive.indices == -1) {
                std::cerr << "Pominieto prymityw - brakuje atrybutow POSITION lub indeksow!\n";
                continue;
            }

            const auto& posAccessor = model.accessors[posIndex];
            const auto& normAccessor = (normIndex != -1) ? model.accessors[normIndex] : tinygltf::Accessor{};
            const auto& texAccessor = (texIndex != -1) ? model.accessors[texIndex] : tinygltf::Accessor{};

            const auto& posView = model.bufferViews[posAccessor.bufferView];
            const auto& normView = (normIndex != -1) ? model.bufferViews[normAccessor.bufferView] : tinygltf::BufferView{};
            const auto& texView = (texIndex != -1) ? model.bufferViews[texAccessor.bufferView] : tinygltf::BufferView{};

            const auto& posBuffer = model.buffers[posView.buffer];
            const auto& normBuffer = (normIndex != -1) ? model.buffers[normView.buffer] : tinygltf::Buffer{};
            const auto& texBuffer = (texIndex != -1) ? model.buffers[texView.buffer] : tinygltf::Buffer{};

            const float* positions = reinterpret_cast<const float*>(&posBuffer.data[posView.byteOffset + posAccessor.byteOffset]);
            const float* normals = (normIndex != -1) ? reinterpret_cast<const float*>(&normBuffer.data[normView.byteOffset + normAccessor.byteOffset]) : nullptr;
            const float* texcoords = (texIndex != -1) ? reinterpret_cast<const float*>(&texBuffer.data[texView.byteOffset + texAccessor.byteOffset]) : nullptr;

            int vertexCount = posAccessor.count;
            std::vector<Vertex> vertices(vertexCount);

            for (int i = 0; i < vertexCount; ++i) {
                vertices[i].position = glm::vec3(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
                if (normals) {
                    vertices[i].normal = glm::vec3(normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]);
                } else {
                    vertices[i].normal = glm::vec3(0.0f, 0.0f, 0.0f);
                }
                if (texcoords) {
                    vertices[i].texcoord = glm::vec2(texcoords[i * 2 + 0], texcoords[i * 2 + 1]);
                } else {
                    vertices[i].texcoord = glm::vec2(0.0f, 0.0f);
                }
            }

            const auto& indexAccessor = model.accessors[primitive.indices];
            const auto& indexView = model.bufferViews[indexAccessor.bufferView];
            const auto& indexBuffer = model.buffers[indexView.buffer];

            const unsigned short* indices = reinterpret_cast<const unsigned short*>(
                &indexBuffer.data[indexView.byteOffset + indexAccessor.byteOffset]);

            newMesh.indexCount = indexAccessor.count;

            glGenBuffers(1, &newMesh.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, newMesh.vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);

            glGenBuffers(1, &newMesh.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, newMesh.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned short) * newMesh.indexCount, indices, GL_STATIC_DRAW);

            modelGL.meshes.push_back(newMesh);

            if (primitive.material >= 0) {
                const auto& material = model.materials[primitive.material];
                if (material.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                    modelGL.textureID = LoadTextureFromGLTF(model, material.pbrMetallicRoughness.baseColorTexture.index);
                }
            }
        }
    }
    return !modelGL.meshes.empty();
}

// --- Pętla renderująca ---
void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            emscripten_cancel_main_loop();
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            lastX = event.button.x;
            lastY = event.button.y;
        } else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            mouseDown = false;
        } else if (event.type == SDL_MOUSEMOTION && mouseDown) {
            rotY += (event.motion.x - lastX) * 0.01f;
            rotX += (event.motion.y - lastY) * 0.01f;
            lastX = event.motion.x;
            lastY = event.motion.y;
        }
    }

    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram);

    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    glViewport(0, 0, width, height);

    glm::mat4 projection = glm::perspective(glm::radians(45.0f), width / (float)height, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0,1,0));
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::scale(model, glm::vec3(1.0f)); 
    glm::mat4 mvp = projection * view * model;

    glUniformMatrix4fv(uniformMVPLoc, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(uniformModelLoc, 1, GL_FALSE, glm::value_ptr(model));
    glUniform1f(uniformRotXLoc, rotX);
    glUniform1f(uniformRotYLoc, rotY);
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D, myModel.textureID);
glUniform1i(uniformTextureLoc, 0);


    for (const auto& mesh : myModel.meshes) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);

        glEnableVertexAttribArray(attrPositionLoc);
        glVertexAttribPointer(attrPositionLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));

        glEnableVertexAttribArray(attrNormalLoc);
        glVertexAttribPointer(attrNormalLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));

        glEnableVertexAttribArray(attrTexcoordLoc);
        glVertexAttribPointer(attrTexcoordLoc, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texcoord));

        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_SHORT, 0);
    }

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
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, "asserts/el.glb")) {
        std::cerr << "Failed to load model: " << err << std::endl;
        return 1;
    }
    if (!warn.empty()) std::cout << "GLTF Warning: " << warn << std::endl;
    
    std::cout << "Liczba scen: " << model.scenes.size() << std::endl;
    std::cout << "Liczba meshy: " << model.meshes.size() << std::endl;
    std::cout << "Liczba buforow: " << model.buffers.size() << std::endl;

    shaderProgram = CreateShaderProgram();
    if (!shaderProgram) return 1;

    attrPositionLoc = glGetAttribLocation(shaderProgram, "a_position");
    attrNormalLoc = glGetAttribLocation(shaderProgram, "a_normal");
    attrTexcoordLoc = glGetAttribLocation(shaderProgram, "a_texcoord");
    uniformMVPLoc = glGetUniformLocation(shaderProgram, "u_mvp");
    uniformModelLoc = glGetUniformLocation(shaderProgram, "u_model");
    uniformTextureLoc = glGetUniformLocation(shaderProgram, "u_texture");
    uniformRotXLoc = glGetUniformLocation(shaderProgram, "u_rotX");
    uniformRotYLoc = glGetUniformLocation(shaderProgram, "u_rotY");
    
    std::cout << "a_position location: " << attrPositionLoc << std::endl;
    std::cout << "a_normal location: " << attrNormalLoc << std::endl;
    std::cout << "a_texcoord location: " << attrTexcoordLoc << std::endl;
    std::cout << "uniformMVP location: " << uniformMVPLoc << std::endl;
    std::cout << "uniformRotX location: " << uniformRotXLoc << std::endl;
    std::cout << "uniformRotY location: " << uniformRotYLoc << std::endl;

    if (!LoadModelToOpenGL(model, myModel)) return 1;
    
    std::cout << "Model zaladowany. Liczba meshy: " << myModel.meshes.size() << std::endl;

    emscripten_set_main_loop(main_loop, 0, true);

    return 0;
}
