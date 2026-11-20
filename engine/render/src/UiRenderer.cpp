#include "sr/render/UiRenderer.h"

#include <algorithm>
#include <cstring>

#include "kUiPixelShader.spv.h"
#include "kUiVertexShader.spv.h"
#include "sr/ui/Font.h"

namespace sr::render {

namespace {
struct PushData {
    float scale[2];
    float translate[2];
    std::uint32_t textureIndex;
    std::uint32_t flags;
};
} // namespace

bool UiRenderer::Init(rhi::Device& device, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kUiVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kUiPixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(ui::Vertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(ui::Vertex, x))},
        {1, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(ui::Vertex, u))},
        {2, rhi::VertexFormat::RGBA8Unorm, static_cast<std::uint32_t>(offsetof(ui::Vertex, color))},
    };
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.debugName = "game ui";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_) {
        error = "could not create the UI pipeline";
        return false;
    }
    return true;
}

void UiRenderer::Shutdown() {
    if (device_ == nullptr) return;
    for (FontTexture& f : fonts_) device_->DestroyTexture(f.texture);
    fonts_.clear();
    if (pipeline_) device_->DestroyPipeline(pipeline_);
    pipeline_ = {};
    device_ = nullptr;
}

void UiRenderer::SyncFont(ui::Font& font) {
    if (device_ == nullptr) return;
    auto it = std::find_if(fonts_.begin(), fonts_.end(), [&](const FontTexture& f) { return f.font == &font; });
    const auto pixels = std::as_bytes(std::span(font.atlasPixels()));
    const auto width = static_cast<std::uint32_t>(font.atlasWidth()), height = static_cast<std::uint32_t>(font.atlasHeight());
    if (it == fonts_.end()) {
        const std::span<const std::byte> mip0 = pixels;
        const rhi::TextureInitData init{std::span(&mip0, 1)};
        const rhi::TextureHandle texture =
            device_->CreateTexture({width, height, 1, rhi::Format::RGBA8Unorm, "ui font atlas"}, &init);
        fonts_.push_back({&font, texture});
        font.MarkUploaded({device_->BindlessIndex(texture), false});
        return;
    }
    if (!font.dirty()) return;
    // Only the rows that changed (new glyphs).
    const int top = std::max(0, font.dirtyTop()), bottom = std::min(font.atlasHeight(), font.dirtyBottom());
    if (bottom > top) {
        const std::size_t rowBytes = std::size_t{width} * 4;
        device_->UpdateTexture(it->texture, {0, top, width, static_cast<std::uint32_t>(bottom - top)},
                               pixels.subspan(static_cast<std::size_t>(top) * rowBytes,
                                              static_cast<std::size_t>(bottom - top) * rowBytes),
                               width * 4);
    }
    font.MarkUploaded({device_->BindlessIndex(it->texture), false});
}

void UiRenderer::Render(rhi::CommandList& cmd, const std::vector<const ui::DrawList*>& lists, float width, float height) {
    if (device_ == nullptr || width <= 0 || height <= 0) return;
    std::size_t vertexCount = 0, indexCount = 0;
    for (const ui::DrawList* list : lists) {
        vertexCount += list->vertices().size();
        indexCount += list->indices().size();
    }
    if (indexCount == 0) return;
    const std::size_t vertexBytes = vertexCount * sizeof(ui::Vertex);
    const std::size_t indexStart = (vertexBytes + 15) & ~std::size_t{15};
    const rhi::TransientAllocation alloc = device_->AllocateTransient(indexStart + indexCount * sizeof(std::uint32_t), 16);
    if (alloc.cpu == nullptr) return;
    auto* vertices = static_cast<std::byte*>(alloc.cpu);
    auto* indices = static_cast<std::byte*>(alloc.cpu) + indexStart;
    for (const ui::DrawList* list : lists) {
        std::memcpy(vertices, list->vertices().data(), list->vertices().size() * sizeof(ui::Vertex));
        std::memcpy(indices, list->indices().data(), list->indices().size() * sizeof(std::uint32_t));
        vertices += list->vertices().size() * sizeof(ui::Vertex);
        indices += list->indices().size() * sizeof(std::uint32_t);
    }
    cmd.BindPipeline(pipeline_);
    cmd.BindVertexBuffer(alloc.buffer, alloc.offset);
    cmd.BindIndexBuffer(alloc.buffer, alloc.offset + indexStart, true);
    cmd.SetViewport({0, 0, width, height});

    PushData push{};
    push.scale[0] = 2.0f / width;
    push.scale[1] = 2.0f / height;
    push.translate[0] = -1.0f;
    push.translate[1] = -1.0f;
    std::uint32_t indexBase = 0;
    std::int32_t vertexBase = 0;
    for (const ui::DrawList* list : lists) {
        for (const ui::DrawCommand& dc : list->commands()) {
            const float x0 = std::max(dc.clip.x, 0.0f), y0 = std::max(dc.clip.y, 0.0f);
            const float x1 = std::min(dc.clip.Right(), width), y1 = std::min(dc.clip.Bottom(), height);
            if (x1 <= x0 || y1 <= y0 || dc.indexCount == 0) continue;
            cmd.SetScissor({static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::uint32_t>(x1 - x0),
                            static_cast<std::uint32_t>(y1 - y0)});
            push.textureIndex = dc.texture.index;
            push.flags = static_cast<std::uint32_t>(rhi::SamplerIndex::LinearClamp) | (dc.texture.premultipliedAlpha ? 0x10000u : 0u) |
                         (dc.texture.opaque ? 0x20000u : 0u);
            cmd.PushConstants(&push, sizeof(push));
            cmd.DrawIndexed(dc.indexCount, indexBase + dc.indexOffset, vertexBase);
        }
        indexBase += static_cast<std::uint32_t>(list->indices().size());
        vertexBase += static_cast<std::int32_t>(list->vertices().size());
    }
}

} // namespace sr::render
