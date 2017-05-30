#pragma once

#include "abstract_renderable.hpp"
#include "texture_manager.hpp"
#include "renderer.hpp"

namespace Granite
{
struct SpriteRenderInfo : RenderInfo
{
	const Vulkan::ImageView *texture = nullptr;
	Vulkan::Program *program = nullptr;

	struct QuadData
	{
		int16_t pos_off_x, pos_off_y, pos_scale_x, pos_scale_y;
		int16_t tex_off_x, tex_off_y, tex_scale_x, tex_scale_y;
		uint8_t color[4];
		float layer;
	};
	QuadData *quads;
	unsigned quad_count;
};

namespace RenderFunctions
{
void sprite_render(Vulkan::CommandBuffer &cmd, const RenderInfo **render, unsigned instances);
}

struct Sprite : AbstractRenderable
{
	MeshDrawPipeline pipeline = MeshDrawPipeline::Opaque;
	Vulkan::Texture *texture = nullptr;

	ivec2 tex_offset;
	ivec2 size;
	vec4 color;

	void get_quad_render_info(const vec3 &position, RenderQueue &queue) const override;
	void get_render_info(const RenderContext &, const CachedSpatialTransformComponent *, RenderQueue &) const override
	{
	}

	MeshDrawPipeline get_mesh_draw_pipeline() const override
	{
		return pipeline;
	}
};
}