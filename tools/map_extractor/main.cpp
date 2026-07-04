#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include "extractor.h"
#include "preview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <thread>
#include <atomic>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

static char g_input_path[1024] = "";
static char g_output_dir[1024] = "";
static char g_ship_name[128] = "my_ship";
static float g_alpha_threshold = 0.05f;
static float g_normal_strength = 1.0f;
static float g_depth_contrast = 1.0f;
static float g_world_scale = 0.06f;
static int g_ship_size_w = 2048;
static int g_ship_size_h = 2048;

static image_t g_input = {};
static extracted_maps_t g_maps = {};
static bool g_maps_dirty = true;

static preview_context_t g_preview_ctx = {};
static float g_star_angle = 0.785398f; // 45 degrees, light from top-left
static bool g_auto_rotate = false;
static bool g_preview_dirty = true;
static bool g_slider_active = false;
static float g_preview_size = 512.0f;
static normal_algorithm_t g_normal_algo = NORMAL_ALGORITHM_SDF_TRUE_EDT;
static SDL_GPUTexture* g_preview_sampler_tex = NULL;
static float g_thumb_zoom = 1.0f;

static float thumb_zoom_step(float current, bool direction)
{
    const float steps[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
    const int count = sizeof(steps) / sizeof(steps[0]);
    int idx = 0;
    for (int i = 0; i < count; ++i)
    {
        if (fabsf(current - steps[i]) < 0.001f) { idx = i; break; }
        if (current < steps[i]) { idx = i; break; }
        idx = i;
    }
    if (direction)
    {
        idx = (idx + 1 < count) ? idx + 1 : count - 1;
    }
    else
    {
        idx = (idx > 0) ? idx - 1 : 0;
    }
    return steps[idx];
}

static std::atomic<bool> g_worker_running{ false };
static std::atomic<bool> g_worker_done{ false };
static extracted_maps_t g_worker_maps = {};
static std::thread g_worker_thread;
static float g_worker_alpha = 0.0f;
static float g_worker_normal_strength = 0.0f;
static float g_worker_depth_contrast = 0.0f;
static normal_algorithm_t g_worker_algo = NORMAL_ALGORITHM_SDF_TRUE_EDT;

static image_t image_copy(const image_t* src)
{
    image_t out = {};
    if (!src || !src->rgba || src->w <= 0 || src->h <= 0) return out;
    out.w = src->w;
    out.h = src->h;
    size_t sz = (size_t)src->w * src->h * 4;
    out.rgba = (unsigned char*)malloc(sz);
    if (out.rgba) memcpy(out.rgba, src->rgba, sz);
    return out;
}

static void worker_extract_maps(image_t input, float alpha_threshold, float normal_strength, float depth_contrast,
                               normal_algorithm_t algo)
{
    g_worker_maps = extract_maps(input, alpha_threshold, normal_strength, depth_contrast, algo);
    image_free(&input);
    g_worker_done.store(true);
}

static void start_worker()
{
    if (g_worker_running.load()) return;
    image_t input = image_copy(&g_input);
    if (!input.rgba) return;
    g_worker_running.store(true);
    g_worker_done.store(false);
    g_worker_alpha = g_alpha_threshold;
    g_worker_normal_strength = g_normal_strength;
    g_worker_depth_contrast = g_depth_contrast;
    g_worker_algo = g_normal_algo;
    extracted_maps_free(&g_worker_maps);
    g_worker_maps = {};
    g_worker_thread = std::thread(worker_extract_maps, input, g_alpha_threshold, g_normal_strength, g_depth_contrast,
                                  g_normal_algo);
}

static void finish_worker()
{
    if (!g_worker_done.load()) return;
    if (g_worker_thread.joinable()) g_worker_thread.join();

    bool stale = (g_worker_alpha != g_alpha_threshold ||
                  g_worker_normal_strength != g_normal_strength ||
                  g_worker_depth_contrast != g_depth_contrast ||
                  g_worker_algo != g_normal_algo);

    if (stale)
    {
        extracted_maps_free(&g_worker_maps);
        g_worker_maps = {};
        g_worker_done.store(false);
        g_worker_running.store(false);
        g_maps_dirty = true;
        return;
    }

    extracted_maps_free(&g_maps);
    g_maps = g_worker_maps;
    g_worker_maps = {};
    g_worker_done.store(false);
    g_worker_running.store(false);
    g_maps_dirty = false;
    g_preview_dirty = true;
}

static void imgui_open_file_dialog(char* out, size_t out_size)
{
    out[0] = '\0';
#ifdef _WIN32
    OPENFILENAMEA ofn = {};
    char buf[1024] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrFilter = "PNG Image\0*.png\0All Files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
    {
        strncpy(out, buf, out_size - 1);
    }
#endif
}

static void imgui_save_folder_dialog(char* out, size_t out_size)
{
    out[0] = '\0';
#ifdef _WIN32
    BROWSEINFOA bi = {};
    bi.lpszTitle = "Select output folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (pidl)
    {
        SHGetPathFromIDListA(pidl, out);
        CoTaskMemFree(pidl);
    }
#endif
}

static bool export_all(const char* folder, const char* name)
{
    if (!g_maps.diffuse.rgba) return false;

    char path[1024];
    char subdir[1024];
    snprintf(subdir, sizeof(subdir), "%s/%s", folder, name);

#ifdef _WIN32
    CreateDirectoryA(subdir, NULL);
#else
    mkdir(subdir, 0755);
#endif

    snprintf(path, sizeof(path), "%s/%s_diffuse_map.png", subdir, name);
    if (!image_save_png(path, &g_maps.diffuse)) return false;

    snprintf(path, sizeof(path), "%s/%s_normal_map.png", subdir, name);
    if (!image_save_png(path, &g_maps.normal)) return false;

    snprintf(path, sizeof(path), "%s/%s_depth_map.png", subdir, name);
    if (!image_save_png(path, &g_maps.depth)) return false;

    snprintf(path, sizeof(path), "%s/%s_position_map.png", subdir, name);
    if (!image_save_png(path, &g_maps.position)) return false;

    snprintf(path, sizeof(path), "%s/%s.ship", subdir, name);
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# Black Stride ship definition (.ship format).\n");
    fprintf(f, "# Generated by map_extractor.exe from %s\n\n", g_input_path);
    fprintf(f, "name \"%s\"\n", name);
    fprintf(f, "faction 2\n");
    fprintf(f, "world_scale %.3f\n", g_world_scale);
    fprintf(f, "size %d %d\n\n", g_ship_size_w, g_ship_size_h);
    fprintf(f, "layer mapped assets/ships/%s/%s_diffuse_map.png assets/ships/%s/%s_normal_map.png assets/ships/%s/%s_depth_map.png assets/ships/%s/%s_position_map.png 0 0 1 0 0\n\n", name, name, name, name, name, name, name, name);
    fprintf(f, "# Box collider matching the art size.\n");
    fprintf(f, "collider %d %d\n", -g_ship_size_w / 2, -g_ship_size_h / 2);
    fprintf(f, "collider %d %d\n",  g_ship_size_w / 2, -g_ship_size_h / 2);
    fprintf(f, "collider %d %d\n",  g_ship_size_w / 2,  g_ship_size_h / 2);
    fprintf(f, "collider %d %d\n", -g_ship_size_w / 2,  g_ship_size_h / 2);
    fclose(f);

    return true;
}

// Upload a CPU image to a GPU texture for ImGui preview.
static SDL_GPUTexture* create_texture(SDL_GPUDevice* device, image_t* img, SDL_GPUCommandBuffer* cmd)
{
    if (!img || !img->rgba) return NULL;
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = (Uint32)img->w;
    info.height = (Uint32)img->h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(device, &info);
    if (!tex) return NULL;

    SDL_GPUTransferBufferCreateInfo tinfo = {};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = (Uint32)(img->w * img->h * 4);
    SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(device, &tinfo);
    if (!tbuf)
    {
        SDL_ReleaseGPUTexture(device, tex);
        return NULL;
    }

    void* dst = SDL_MapGPUTransferBuffer(device, tbuf, false);
    memcpy(dst, img->rgba, (size_t)img->w * img->h * 4);
    SDL_UnmapGPUTransferBuffer(device, tbuf);

    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = tbuf;
    src.offset = 0;
    src.pixels_per_row = (Uint32)img->w;
    src.rows_per_layer = (Uint32)img->h;
    SDL_GPUTextureRegion dst_r = {};
    dst_r.texture = tex;
    dst_r.w = (Uint32)img->w;
    dst_r.h = (Uint32)img->h;
    dst_r.d = 1;
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);
    if (copy_pass)
    {
        SDL_UploadToGPUTexture(copy_pass, &src, &dst_r, false);
        SDL_EndGPUCopyPass(copy_pass);
    }
    SDL_ReleaseGPUTransferBuffer(device, tbuf);
    img->internal = tex;
    return tex;
}

int main(int argc, char* argv[])
{
    // Headless preview mode: --preview <input.png> <output_screenshot.png> [star_angle_deg]
    if (argc > 1 && strcmp(argv[1], "--preview") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr, "Usage: map_extractor --preview <input.png> <output.png> [star_angle_deg]\n");
            return 1;
        }
        const char* input_path = argv[2];
        const char* output_path = argv[3];
        float star_deg = 45.0f;
        if (argc > 4) star_deg = (float)atof(argv[4]);
        float star_rad = star_deg * ((float)M_PI / 180.0f);

        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, FALSE, NULL);
        if (!device)
        {
            fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }

        preview_context_t preview = {};
        if (!preview_init(&preview, device))
        {
            fprintf(stderr, "preview_init failed\n");
            SDL_DestroyGPUDevice(device);
            SDL_Quit();
            return 1;
        }

        image_t input = image_load(input_path);
        if (!input.rgba)
        {
            fprintf(stderr, "Failed to load image: %s\n", input_path);
            preview_shutdown(&preview);
            SDL_DestroyGPUDevice(device);
            SDL_Quit();
            return 1;
        }

        extracted_maps_t maps = extract_maps(input, g_alpha_threshold, g_normal_strength, g_depth_contrast,
                                               NORMAL_ALGORITHM_SDF_TRUE_EDT);
        image_free(&input);
        if (!maps.diffuse.rgba)
        {
            fprintf(stderr, "Map extraction failed\n");
            preview_shutdown(&preview);
            SDL_DestroyGPUDevice(device);
            SDL_Quit();
            return 1;
        }

        SDL_GPUCommandBuffer* upload_cmd = SDL_AcquireGPUCommandBuffer(device);
        create_texture(device, &maps.diffuse, upload_cmd);
        create_texture(device, &maps.normal, upload_cmd);
        create_texture(device, &maps.depth, upload_cmd);
        create_texture(device, &maps.position, upload_cmd);
        SDL_SubmitGPUCommandBuffer(upload_cmd);

        image_t screenshot = preview_render_to_image(&preview, &maps, star_rad, (int)g_preview_size, (int)g_preview_size);
        int saved = 0;
        if (screenshot.rgba)
        {
            saved = image_save_png(output_path, &screenshot);
        }

        image_free(&screenshot);
        extracted_maps_free(&maps);
        preview_shutdown(&preview);
        SDL_DestroyGPUDevice(device);
        SDL_Quit();

        if (!saved)
        {
            fprintf(stderr, "Failed to save screenshot: %s\n", output_path);
            return 1;
        }
        printf("Saved lit preview to %s\n", output_path);
        return 0;
    }

    // Headless export mode: --export <input.png> <output_dir> [ship_name]
    if (argc > 1 && strcmp(argv[1], "--export") == 0)
    {
        if (argc < 4)
        {
            fprintf(stderr, "Usage: map_extractor --export <input.png> <output_dir> [ship_name]\n");
            return 1;
        }
        strncpy(g_input_path, argv[2], sizeof(g_input_path) - 1);
        strncpy(g_output_dir, argv[3], sizeof(g_output_dir) - 1);
        if (argc > 4) strncpy(g_ship_name, argv[4], sizeof(g_ship_name) - 1);

        g_input = image_load(g_input_path);
        if (!g_input.rgba)
        {
            fprintf(stderr, "Failed to load image: %s\n", g_input_path);
            return 1;
        }
        g_ship_size_w = g_input.w;
        g_ship_size_h = g_input.h;
        g_maps = extract_maps(g_input, g_alpha_threshold, g_normal_strength, g_depth_contrast,
                              NORMAL_ALGORITHM_SDF_TRUE_EDT);
        if (!g_maps.diffuse.rgba)
        {
            fprintf(stderr, "Map extraction failed\n");
            return 1;
        }
        if (!export_all(g_output_dir, g_ship_name))
        {
            fprintf(stderr, "Export failed\n");
            return 1;
        }
        printf("Exported maps and ship definition to %s/%s\n", g_output_dir, g_ship_name);
        return 0;
    }

    // GUI mode: optional initial image path.
    if (argc > 1 && argv[1])
    {
        strncpy(g_input_path, argv[1], sizeof(g_input_path) - 1);
        const char* name = strrchr(g_input_path, '\\');
        if (!name) name = strrchr(g_input_path, '/');
        if (!name) name = g_input_path;
        else name += 1;
        strncpy(g_ship_name, name, sizeof(g_ship_name) - 1);
        char* dot = strrchr(g_ship_name, '.');
        if (dot) *dot = '\0';

        g_input = image_load(g_input_path);
        if (g_input.rgba)
        {
            g_ship_size_w = g_input.w;
            g_ship_size_h = g_input.h;
            g_maps_dirty = true;
        }
    }

    SDL_Log("entering GUI mode");

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("4-Map Extractor", 1400, 900, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, FALSE, NULL);
    if (!device)
    {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!preview_init(&g_preview_ctx, device))
    {
        SDL_Log("preview_init failed; lit preview will be unavailable.");
    }

    SDL_GPUTexture* tex_diffuse = NULL;
    SDL_GPUTexture* tex_normal = NULL;
    SDL_GPUTexture* tex_depth = NULL;
    SDL_GPUTexture* tex_position = NULL;
    unsigned char* last_maps = NULL;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    snprintf(g_output_dir, sizeof(g_output_dir), "C:\\dev\\blackstride\\assets\\ships");

    SDL_Log("starting main loop");
    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT)
            {
                running = false;
            }
            if (e.type == SDL_EVENT_WINDOW_RESIZED)
            {
                // SDL_GPU handles this internally.
            }
        }

        if (g_auto_rotate)
        {
            static Uint32 last_ticks = 0;
            Uint32 now = SDL_GetTicks();
            if (last_ticks == 0) last_ticks = now;
            float dt = (float)(now - last_ticks) / 1000.0f;
            last_ticks = now;
            g_star_angle += dt * 0.5f;
            if (g_star_angle > 2.0f * (float)M_PI) g_star_angle -= 2.0f * (float)M_PI;
            g_preview_dirty = true;
        }

        // Start asynchronous map extraction when properties change.
        if (g_maps_dirty && g_input.rgba && !g_worker_running.load())
        {
            start_worker();
        }

        // Apply the worker's result when it finishes.
        finish_worker();

        // Recreate preview textures whenever maps change.
        if (g_maps.diffuse.rgba != last_maps)
        {
            // Wait for the GPU to finish using the previous frame's textures before releasing them.
            SDL_WaitForGPUIdle(device);

            // release old textures
            if (tex_diffuse) { SDL_ReleaseGPUTexture(device, tex_diffuse); tex_diffuse = NULL; g_maps.diffuse.internal = NULL; }
            if (tex_normal) { SDL_ReleaseGPUTexture(device, tex_normal); tex_normal = NULL; g_maps.normal.internal = NULL; }
            if (tex_depth) { SDL_ReleaseGPUTexture(device, tex_depth); tex_depth = NULL; g_maps.depth.internal = NULL; }
            if (tex_position) { SDL_ReleaseGPUTexture(device, tex_position); tex_position = NULL; g_maps.position.internal = NULL; }
            if (g_preview_sampler_tex) { SDL_ReleaseGPUTexture(device, g_preview_sampler_tex); g_preview_sampler_tex = NULL; }
            g_preview_dirty = true;
            last_maps = g_maps.diffuse.rgba;
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Acquire the command buffer for the frame.
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        if (g_maps.diffuse.rgba)
        {
            if (!tex_diffuse) tex_diffuse = create_texture(device, &g_maps.diffuse, cmd);
            if (!tex_normal) tex_normal = create_texture(device, &g_maps.normal, cmd);
            if (!tex_depth) tex_depth = create_texture(device, &g_maps.depth, cmd);
            if (!tex_position) tex_position = create_texture(device, &g_maps.position, cmd);

            if (g_preview_ctx.initialized && g_preview_dirty)
            {
                // Submit the map-upload command buffer so the four maps are ready on the GPU.
                SDL_SubmitGPUCommandBuffer(cmd);

                // Use the same synchronous path as the headless screenshot mode.
                image_t screenshot = preview_render_to_image(&g_preview_ctx, &g_maps, g_star_angle,
                                                            (int)g_preview_size, (int)g_preview_size);
                if (screenshot.rgba)
                {
                    if (g_preview_sampler_tex) { SDL_ReleaseGPUTexture(device, g_preview_sampler_tex); g_preview_sampler_tex = NULL; }
                    cmd = SDL_AcquireGPUCommandBuffer(device);
                    g_preview_sampler_tex = create_texture(device, &screenshot, cmd);
                    image_free(&screenshot);
                }
                g_preview_dirty = false;
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("4-Map Extractor", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open PNG...", "Ctrl+O"))
                {
                    char tmp[1024] = {};
                    imgui_open_file_dialog(tmp, sizeof(tmp));
                    if (tmp[0])
                    {
                        strncpy(g_input_path, tmp, sizeof(g_input_path) - 1);
                        image_free(&g_input);
                        g_input = image_load(g_input_path);
                        if (g_input.rgba)
                        {
                            g_ship_size_w = g_input.w;
                            g_ship_size_h = g_input.h;
                        }
                        g_maps_dirty = TRUE;
                    }
                }
                if (ImGui::MenuItem("Export", "Ctrl+E", false, g_maps.diffuse.rgba != NULL))
                {
                    char tmp[1024] = {};
                    imgui_save_folder_dialog(tmp, sizeof(tmp));
                    if (tmp[0])
                    {
                        strncpy(g_output_dir, tmp, sizeof(g_output_dir) - 1);
                        if (export_all(g_output_dir, g_ship_name))
                        {
                            SDL_Log("Exported to %s/%s", g_output_dir, g_ship_name);
                        }
                        else
                        {
                            SDL_Log("Export failed");
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))
                {
                    running = false;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        ImGui::Columns(2, "main_cols", false);
        ImGui::SetColumnWidth(0, 320);

        ImGui::BeginChild("Controls", ImVec2(0, 0), true);
        ImGui::Text("Input");
        ImGui::InputText("Path", g_input_path, sizeof(g_input_path), ImGuiInputTextFlags_ReadOnly);
        ImGui::InputText("Output dir", g_output_dir, sizeof(g_output_dir));
        ImGui::InputText("Ship name", g_ship_name, sizeof(g_ship_name));

        ImGui::Separator();
        ImGui::Text("Parameters");
        bool any_slider_active = false;
        if (ImGui::SliderFloat("Alpha threshold", &g_alpha_threshold, 0.0f, 1.0f)) g_slider_active = true;
        any_slider_active |= ImGui::IsItemActive();
        if (ImGui::SliderFloat("Normal strength", &g_normal_strength, 0.0f, 4.0f)) g_slider_active = true;
        any_slider_active |= ImGui::IsItemActive();
        if (ImGui::SliderFloat("Depth contrast", &g_depth_contrast, 0.1f, 4.0f)) g_slider_active = true;
        any_slider_active |= ImGui::IsItemActive();

        const char* normal_names[] = { "Chamfer SDF", "True EDT SDF", "Alpha gradient", "Gaussian height" };
        int algo = (int)g_normal_algo;
        if (ImGui::Combo("Normal algorithm", &algo, normal_names, NORMAL_ALGORITHM_COUNT))
        {
            g_normal_algo = (normal_algorithm_t)algo;
            g_slider_active = true;
        }
        any_slider_active |= ImGui::IsItemActive();

        if (g_slider_active && !any_slider_active)
        {
            g_slider_active = false;
            g_maps_dirty = true;
        }

        ImGui::Separator();
        ImGui::Text("Ship definition");
        ImGui::InputFloat("World scale", &g_world_scale, 0.01f, 0.1f);
        ImGui::InputInt("Size W", &g_ship_size_w);
        ImGui::InputInt("Size H", &g_ship_size_h);

        ImGui::Separator();
        if (ImGui::Button("Export now", ImVec2(-1, 0)) && g_maps.diffuse.rgba)
        {
            if (export_all(g_output_dir, g_ship_name))
            {
                SDL_Log("Exported to %s/%s", g_output_dir, g_ship_name);
            }
            else
            {
                SDL_Log("Export failed");
            }
        }

        const char* status = "no image loaded";
        if (g_worker_running.load()) status = "computing maps...";
        else if (g_maps.diffuse.rgba) status = "maps ready";
        ImGui::Text("Status: %s", status);
        ImGui::EndChild();

        ImGui::NextColumn();

        ImGui::BeginChild("Preview", ImVec2(0, 0), true);
        if (g_maps.diffuse.rgba)
        {
            ImGui::Text("Resolution: %dx%d", g_maps.diffuse.w, g_maps.diffuse.h);
            if (ImGui::BeginTabBar("PreviewTabs", ImGuiTabBarFlags_None))
            {
                if (ImGui::BeginTabItem("Thumbnails"))
                {
                    float avail = ImGui::GetContentRegionAvail().x;
                    float base = avail * 0.5f - 8;
                    float thumb = base * g_thumb_zoom;
                    float thumb_h = thumb * ((float)g_maps.diffuse.h / (float)g_maps.diffuse.w);

                    ImGui::Text("Zoom");
                    ImGui::SameLine();
                    if (ImGui::Button("-")) g_thumb_zoom = thumb_zoom_step(g_thumb_zoom, FALSE);
                    ImGui::SameLine();
                    ImGui::Text("%d%%", (int)(g_thumb_zoom * 100.0f + 0.5f));
                    ImGui::SameLine();
                    if (ImGui::Button("+")) g_thumb_zoom = thumb_zoom_step(g_thumb_zoom, TRUE);

                    ImGui::Text("Diffuse");
                    ImGui::Image((ImTextureID)tex_diffuse, ImVec2(thumb, thumb_h));
                    ImGui::Text("Normal");
                    ImGui::Image((ImTextureID)tex_normal, ImVec2(thumb, thumb_h));
                    ImGui::Text("Depth");
                    ImGui::Image((ImTextureID)tex_depth, ImVec2(thumb, thumb_h));
                    ImGui::Text("Position");
                    ImGui::Image((ImTextureID)tex_position, ImVec2(thumb, thumb_h));
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Lit Preview"))
                {
                    if (g_preview_ctx.initialized)
                    {
                        float avail = ImGui::GetContentRegionAvail().x;
                        float sz = avail - 8;
                        if (sz > g_preview_size) sz = g_preview_size;
                        float aspect = (g_maps.diffuse.w > 0 && g_maps.diffuse.h > 0)
                            ? (float)g_maps.diffuse.w / (float)g_maps.diffuse.h : 1.0f;
                        float disp_w = sz, disp_h = sz;
                        if (aspect > 1.0f) disp_h = sz / aspect;
                        else disp_w = sz * aspect;
                        if (g_preview_sampler_tex)
                        {
                            ImGui::Image((ImTextureID)g_preview_sampler_tex, ImVec2(disp_w, disp_h));
                        }
                        else
                        {
                            ImGui::Dummy(ImVec2(sz, sz));
                            ImGui::Text("Rendering preview...");
                        }

                        float star_deg = g_star_angle * 180.0f / (float)M_PI;
                        if (ImGui::SliderFloat("Star angle (deg)", &star_deg, 0.0f, 360.0f))
                        {
                            g_star_angle = star_deg * (float)M_PI / 180.0f;
                            g_preview_dirty = true;
                        }
                        ImGui::Checkbox("Auto-rotate", &g_auto_rotate);
                        if (ImGui::Button("Reset angle")) { g_star_angle = 0.785398f; g_auto_rotate = false; g_preview_dirty = true; }
                    }
                    else
                    {
                        ImGui::Text("Lit preview unavailable (pipeline init failed).");
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        else if (g_input_path[0])
        {
            ImGui::Text("Failed to load: %s", g_input_path);
        }
        else
        {
            ImGui::Text("Open a PNG file to begin.");
        }
        ImGui::EndChild();

        ImGui::End();

        // Render
        ImGui::Render();

        SDL_GPUTexture* swapchain = NULL;
        if (!SDL_AcquireGPUSwapchainTexture(cmd, window, &swapchain, NULL, NULL))
        {
            SDL_Log("AcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        }

        if (swapchain)
        {
            ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

            SDL_GPUColorTargetInfo target = {};
            target.texture = swapchain;
            target.clear_color = (SDL_FColor){ 0.1f, 0.1f, 0.1f, 1.0f };
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
            ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    if (g_worker_thread.joinable()) g_worker_thread.join();

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (g_preview_sampler_tex) SDL_ReleaseGPUTexture(device, g_preview_sampler_tex);
    if (tex_diffuse) SDL_ReleaseGPUTexture(device, tex_diffuse);
    if (tex_normal) SDL_ReleaseGPUTexture(device, tex_normal);
    if (tex_depth) SDL_ReleaseGPUTexture(device, tex_depth);
    if (tex_position) SDL_ReleaseGPUTexture(device, tex_position);
    preview_shutdown(&g_preview_ctx);

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    image_free(&g_input);
    extracted_maps_free(&g_maps);

    return 0;
}
