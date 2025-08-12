#include <SDL2/SDL.h>
#include <GLES2/gl2.h> // WebGL to OpenGL ES2.0
#include <emscripten.h>
#include <iostream>
#include "tiny_gltf.h"

SDL_Window* window = nullptr;
SDL_GLContext context;

void LoadGLBModel(const std::string& filename) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
    if (!warn.empty()) std::cout << "Warning: " << warn << std::endl;
    if (!err.empty()) std::cerr << "Error: " << err << std::endl;
    if (!ret) {
        std::cerr << "Failed to load GLB file: " << filename << std::endl;
        return;
    }

    std::cout << "Loaded GLB file: " << filename << std::endl;
}

void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            emscripten_cancel_main_loop(); // zatrzymaj pętlę w przeglądarce
    }

    glClearColor(0.2f, 0.2f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

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

    LoadGLBModel("asserts/vr_room_light_baked.glb"); // pamiętaj o załączeniu w systemie plików

    emscripten_set_main_loop(main_loop, 0, true);
    return 0;
}
