#include "sr/devui/ImGuiLayer.h"

#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "kImGuiPixelShader.spv.h"
#include "kImGuiVertexShader.spv.h"

namespace sr::devui {

namespace {

struct PushData {
    float scale[2];
    float translate[2];
    std::uint32_t textureIndex;
    std::uint32_t flags;
};

// ImTextureID layout: bits 0-31 bindless index, 32-35 sampler, 36-39 channel,
// 40-47 mip+1, 48 premultiplied. The shader's `flags` is bits 32-48 shifted down.
constexpr int kFlagsShift = 32;

rhi::TextureHandle UnpackHandle(void* userData) {
    const auto packed = reinterpret_cast<std::uintptr_t>(userData);
    return {static_cast<std::uint32_t>(packed & 0xFFFFFFFFu), static_cast<std::uint32_t>(packed >> 32)};
}

void* PackHandle(rhi::TextureHandle h) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(h.index) | static_cast<std::uintptr_t>(h.generation) << 32);
}

// ImGui textures are RGBA32 or Alpha8; the GPU copy is always RGBA8.
std::vector<std::byte> ToRgba(const ImTextureData* tex, int x, int y, int w, int h) {
    std::vector<std::byte> out(static_cast<std::size_t>(w) * h * 4);
    for (int row = 0; row < h; ++row) {
        const auto* src = static_cast<const std::uint8_t*>(const_cast<ImTextureData*>(tex)->GetPixelsAt(x, y + row));
        std::byte* dst = out.data() + static_cast<std::size_t>(row) * w * 4;
        if (tex->Format == ImTextureFormat_RGBA32) {
            std::memcpy(dst, src, static_cast<std::size_t>(w) * 4);
        } else {
            for (int i = 0; i < w; ++i) {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = std::byte{255};
                dst[i * 4 + 3] = std::byte{src[i]};
            }
        }
    }
    return out;
}

} // namespace

ImTextureID MakeTextureId(const TextureView& view) {
    const std::uint64_t mip = view.mip < 0 ? 0 : static_cast<std::uint64_t>(std::min(view.mip + 1, 255));
    return static_cast<ImTextureID>(view.bindlessIndex) | static_cast<std::uint64_t>(view.sampler) << 32 |
           static_cast<std::uint64_t>(view.channel) << 36 | mip << 40 |
           static_cast<std::uint64_t>(view.premultipliedAlpha ? 1 : 0) << 48;
}

bool ImGuiLayer::Init(rhi::Device& device, platform::Window& window, std::string& error) {
    device_ = &device;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.BackendRendererName = "sr_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = "sr_devtools.ini";

    ImGui::StyleColorsDark();
    // Scale the UI with the display, so it stays readable on high-DPI screens.
    const float scale = window.DisplayScale();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    if (!ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(window.SdlWindow()))) {
        error = "ImGui_ImplSDL3_InitForVulkan failed";
        return false;
    }

    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kImGuiVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kImGuiPixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(ImDrawVert);
    desc.attributes = {
        {0, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(ImDrawVert, pos))},
        {1, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(ImDrawVert, uv))},
        {2, rhi::VertexFormat::RGBA8Unorm, static_cast<std::uint32_t>(offsetof(ImDrawVert, col))},
    };
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.debugName = "imgui";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_) {
        error = "could not create the ImGui pipeline";
        return false;
    }
    initialized_ = true;
    return true;
}

void ImGuiLayer::Shutdown() {
    if (!initialized_) return;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount == 1 && tex->BackendUserData != nullptr) {
            device_->DestroyTexture(UnpackHandle(tex->BackendUserData));
            tex->BackendUserData = nullptr;
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    device_->DestroyPipeline(pipeline_);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiLayer::ProcessEvent(const void* sdlEvent) { ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(sdlEvent)); }

void ImGuiLayer::NewFrame() {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::UpdateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        const std::vector<std::byte> pixels = ToRgba(tex, 0, 0, tex->Width, tex->Height);
        const std::span<const std::byte> mip0(pixels);
        const rhi::TextureInitData init{std::span(&mip0, 1)};
        const rhi::TextureHandle handle = device_->CreateTexture(
            {static_cast<std::uint32_t>(tex->Width), static_cast<std::uint32_t>(tex->Height), 1,
             rhi::Format::RGBA8Unorm, "imgui texture"},
            &init);
        tex->BackendUserData = PackHandle(handle);
        tex->SetTexID(MakeTextureId({device_->BindlessIndex(handle), rhi::SamplerIndex::LinearClamp}));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        const rhi::TextureHandle handle = UnpackHandle(tex->BackendUserData);
        for (const ImTextureRect& r : tex->Updates) {
            const std::vector<std::byte> pixels = ToRgba(tex, r.x, r.y, r.w, r.h);
            device_->UpdateTexture(handle, {r.x, r.y, r.w, r.h}, pixels, static_cast<std::uint32_t>(r.w) * 4);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 4) {
        // More frames than the game -> render -> GPU pipeline holds, so no copied frame uses it.
        // The RHI defers the actual release until in-flight frames are done.
        device_->DestroyTexture(UnpackHandle(tex->BackendUserData));
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void ImGuiLayer::SetupRenderState(rhi::CommandList& cmd, const ImGuiFrame& frame, std::size_t vertexOffset,
                                  std::size_t indexOffset, rhi::BufferHandle buffer) {
    cmd.BindPipeline(pipeline_);
    cmd.BindVertexBuffer(buffer, vertexOffset);
    cmd.BindIndexBuffer(buffer, indexOffset, sizeof(ImDrawIdx) == 4);
    const float fbWidth = frame.displaySize.x * frame.framebufferScale.x;
    const float fbHeight = frame.displaySize.y * frame.framebufferScale.y;
    cmd.SetViewport({0, 0, fbWidth, fbHeight});
}

void ImGuiLayer::Render(rhi::CommandList& cmd) {
    EndFrame(singleThreaded_);
    Draw(cmd, singleThreaded_);
}

void ImGuiLayer::EndFrame(ImGuiFrame& out) {
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    out.Clear();
    out.displayPos = drawData->DisplayPos;
    out.displaySize = drawData->DisplaySize;
    out.framebufferScale = drawData->FramebufferScale;

    if (drawData->Textures != nullptr) {
        for (ImTextureData* tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);
        }
    }

    out.vertices.reserve(static_cast<std::size_t>(drawData->TotalVtxCount));
    out.indices.reserve(static_cast<std::size_t>(drawData->TotalIdxCount));
    for (const ImDrawList* list : drawData->CmdLists) {
        const auto globalVtx = static_cast<std::int32_t>(out.vertices.size());
        const auto globalIdx = static_cast<std::uint32_t>(out.indices.size());
        out.vertices.insert(out.vertices.end(), list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
        out.indices.insert(out.indices.end(), list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
        for (const ImDrawCmd& dc : list->CmdBuffer) {
            ImGuiFrame::Command command;
            if (dc.UserCallback != nullptr) {
                // Only the reset callback survives the copy; others need ImGui's live state.
                if (dc.UserCallback != ImDrawCallback_ResetRenderState) continue;
                command.resetState = true;
            } else {
                command.clip = dc.ClipRect;
                command.texture = dc.GetTexID();
                command.elemCount = dc.ElemCount;
                command.firstIndex = dc.IdxOffset + globalIdx;
                command.vertexOffset = static_cast<std::int32_t>(dc.VtxOffset) + globalVtx;
            }
            out.commands.push_back(command);
        }
    }
}

void ImGuiLayer::Draw(rhi::CommandList& cmd, const ImGuiFrame& frame) {
    const float fbWidth = frame.displaySize.x * frame.framebufferScale.x;
    const float fbHeight = frame.displaySize.y * frame.framebufferScale.y;
    if (fbWidth <= 0 || fbHeight <= 0 || frame.vertices.empty()) return;

    // One transient allocation holds all vertices followed by all indices.
    const std::size_t vertexBytes = frame.vertices.size() * sizeof(ImDrawVert);
    const std::size_t indexBytes = frame.indices.size() * sizeof(ImDrawIdx);
    const std::size_t indexStart = (vertexBytes + 15) & ~std::size_t{15};
    const rhi::TransientAllocation alloc = device_->AllocateTransient(indexStart + indexBytes, 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, frame.vertices.data(), vertexBytes);
    std::memcpy(static_cast<std::byte*>(alloc.cpu) + indexStart, frame.indices.data(), indexBytes);

    const std::size_t vertexOffset = alloc.offset;
    const std::size_t indexOffset = alloc.offset + indexStart;
    SetupRenderState(cmd, frame, vertexOffset, indexOffset, alloc.buffer);

    PushData push{};
    push.scale[0] = 2.0f / frame.displaySize.x;
    push.scale[1] = 2.0f / frame.displaySize.y;
    push.translate[0] = -1.0f - frame.displayPos.x * push.scale[0];
    push.translate[1] = -1.0f - frame.displayPos.y * push.scale[1];

    const ImVec2 clipOff = frame.displayPos;
    const ImVec2 clipScale = frame.framebufferScale;
    for (const ImGuiFrame::Command& dc : frame.commands) {
        if (dc.resetState) {
            SetupRenderState(cmd, frame, vertexOffset, indexOffset, alloc.buffer);
            continue;
        }
        const float x0 = std::max((dc.clip.x - clipOff.x) * clipScale.x, 0.0f);
        const float y0 = std::max((dc.clip.y - clipOff.y) * clipScale.y, 0.0f);
        const float x1 = std::min((dc.clip.z - clipOff.x) * clipScale.x, fbWidth);
        const float y1 = std::min((dc.clip.w - clipOff.y) * clipScale.y, fbHeight);
        if (x1 <= x0 || y1 <= y0) continue;
        cmd.SetScissor({static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
                        static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)});
        push.textureIndex = static_cast<std::uint32_t>(dc.texture & 0xFFFFFFFFu);
        push.flags = static_cast<std::uint32_t>(dc.texture >> kFlagsShift);
        cmd.PushConstants(&push, sizeof(push));
        cmd.DrawIndexed(dc.elemCount, dc.firstIndex, dc.vertexOffset);
    }
}
} // namespace sr::devui
