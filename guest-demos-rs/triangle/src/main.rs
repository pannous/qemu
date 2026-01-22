//! Vulkan triangle demo - renders RGB triangle to DRM display

use ash::vk;
use vk_common::{vulkan::{load_spirv, RenderTarget, VkContext}, DrmDisplay};

fn main() -> Result<(), String> {
    let display = DrmDisplay::open("/dev/dri/card0")?;
    let ctx = VkContext::new()?;
    let (w, h) = (display.width, display.height);

    let rt = RenderTarget::new(&ctx, w, h)?;

    // Create render pass
    let attachments = [vk::AttachmentDescription::default()
        .format(vk::Format::B8G8R8A8_UNORM)
        .samples(vk::SampleCountFlags::TYPE_1)
        .load_op(vk::AttachmentLoadOp::CLEAR)
        .store_op(vk::AttachmentStoreOp::STORE)
        .initial_layout(vk::ImageLayout::UNDEFINED)
        .final_layout(vk::ImageLayout::GENERAL)];

    let color_ref = [vk::AttachmentReference {
        attachment: 0,
        layout: vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
    }];

    let subpasses = [vk::SubpassDescription::default()
        .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
        .color_attachments(&color_ref)];

    let rp_info = vk::RenderPassCreateInfo::default()
        .attachments(&attachments)
        .subpasses(&subpasses);

    let render_pass = unsafe { ctx.device.create_render_pass(&rp_info, None) }
        .map_err(|e| format!("Render pass: {:?}", e))?;

    // Create framebuffer
    let fb_attachments = [rt.view];
    let fb_info = vk::FramebufferCreateInfo::default()
        .render_pass(render_pass)
        .attachments(&fb_attachments)
        .width(w)
        .height(h)
        .layers(1);

    let framebuffer = unsafe { ctx.device.create_framebuffer(&fb_info, None) }
        .map_err(|e| format!("Framebuffer: {:?}", e))?;

    // Load shaders
    let vert_code = load_spirv("/root/tri.vert.spv")?;
    let frag_code = load_spirv("/root/tri.frag.spv")?;
    let vert_mod = ctx.create_shader_module(&vert_code).map_err(|e| format!("Vert shader: {:?}", e))?;
    let frag_mod = ctx.create_shader_module(&frag_code).map_err(|e| format!("Frag shader: {:?}", e))?;

    let entry_name = c"main";

    let stages = [
        vk::PipelineShaderStageCreateInfo::default()
            .stage(vk::ShaderStageFlags::VERTEX)
            .module(vert_mod)
            .name(entry_name),
        vk::PipelineShaderStageCreateInfo::default()
            .stage(vk::ShaderStageFlags::FRAGMENT)
            .module(frag_mod)
            .name(entry_name),
    ];

    // Pipeline layout (empty)
    let layout_info = vk::PipelineLayoutCreateInfo::default();
    let pipeline_layout = unsafe { ctx.device.create_pipeline_layout(&layout_info, None) }
        .map_err(|e| format!("Pipeline layout: {:?}", e))?;

    // Pipeline
    let vertex_input = vk::PipelineVertexInputStateCreateInfo::default();
    let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
        .topology(vk::PrimitiveTopology::TRIANGLE_LIST);

    let viewports = [vk::Viewport {
        x: 0.0,
        y: 0.0,
        width: w as f32,
        height: h as f32,
        min_depth: 0.0,
        max_depth: 1.0,
    }];
    let scissors = [vk::Rect2D {
        offset: vk::Offset2D { x: 0, y: 0 },
        extent: vk::Extent2D { width: w, height: h },
    }];
    let viewport_state = vk::PipelineViewportStateCreateInfo::default()
        .viewports(&viewports)
        .scissors(&scissors);

    let rasterizer = vk::PipelineRasterizationStateCreateInfo::default()
        .polygon_mode(vk::PolygonMode::FILL)
        .cull_mode(vk::CullModeFlags::NONE)
        .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
        .line_width(1.0);

    let multisampling = vk::PipelineMultisampleStateCreateInfo::default()
        .rasterization_samples(vk::SampleCountFlags::TYPE_1);

    let blend_attachments = [vk::PipelineColorBlendAttachmentState::default()
        .color_write_mask(vk::ColorComponentFlags::RGBA)];
    let color_blending = vk::PipelineColorBlendStateCreateInfo::default()
        .attachments(&blend_attachments);

    let pipeline_info = vk::GraphicsPipelineCreateInfo::default()
        .stages(&stages)
        .vertex_input_state(&vertex_input)
        .input_assembly_state(&input_assembly)
        .viewport_state(&viewport_state)
        .rasterization_state(&rasterizer)
        .multisample_state(&multisampling)
        .color_blend_state(&color_blending)
        .layout(pipeline_layout)
        .render_pass(render_pass)
        .subpass(0);

    let pipeline = unsafe {
        ctx.device.create_graphics_pipelines(vk::PipelineCache::null(), &[pipeline_info], None)
    }
    .map_err(|e| format!("Pipeline: {:?}", e.1))?[0];

    // Command pool and buffer
    let pool_info = vk::CommandPoolCreateInfo::default()
        .queue_family_index(0);
    let cmd_pool = unsafe { ctx.device.create_command_pool(&pool_info, None) }
        .map_err(|e| format!("Cmd pool: {:?}", e))?;

    let alloc_info = vk::CommandBufferAllocateInfo::default()
        .command_pool(cmd_pool)
        .level(vk::CommandBufferLevel::PRIMARY)
        .command_buffer_count(1);
    let cmd = unsafe { ctx.device.allocate_command_buffers(&alloc_info) }
        .map_err(|e| format!("Cmd buffer: {:?}", e))?[0];

    // Fence
    let fence_info = vk::FenceCreateInfo::default();
    let fence = unsafe { ctx.device.create_fence(&fence_info, None) }
        .map_err(|e| format!("Fence: {:?}", e))?;

    // Record commands
    let begin_info = vk::CommandBufferBeginInfo::default();
    unsafe { ctx.device.begin_command_buffer(cmd, &begin_info) }
        .map_err(|e| format!("Begin cmd: {:?}", e))?;

    let clear_values = [vk::ClearValue {
        color: vk::ClearColorValue { float32: [0.0, 0.0, 0.3, 1.0] },
    }];
    let rp_begin = vk::RenderPassBeginInfo::default()
        .render_pass(render_pass)
        .framebuffer(framebuffer)
        .render_area(vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent: vk::Extent2D { width: w, height: h },
        })
        .clear_values(&clear_values);

    unsafe {
        ctx.device.cmd_begin_render_pass(cmd, &rp_begin, vk::SubpassContents::INLINE);
        ctx.device.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipeline);
        ctx.device.cmd_draw(cmd, 3, 1, 0, 0);
        ctx.device.cmd_end_render_pass(cmd);
        ctx.device.end_command_buffer(cmd).map_err(|e| format!("End cmd: {:?}", e))?;
    }

    // Submit
    let submit_info = vk::SubmitInfo::default()
        .command_buffers(std::slice::from_ref(&cmd));
    unsafe {
        ctx.device.queue_submit(ctx.queue, &[submit_info], fence)
            .map_err(|e| format!("Submit: {:?}", e))?;
        ctx.device.wait_for_fences(&[fence], true, u64::MAX)
            .map_err(|e| format!("Wait fence: {:?}", e))?;
    }

    println!("Render done");

    // Copy to display
    display.present(rt.mapped_ptr, rt.row_pitch as u32);
    println!("Should show RGB triangle on blue for 5s");
    std::thread::sleep(std::time::Duration::from_secs(5));

    // Cleanup
    unsafe {
        ctx.device.destroy_fence(fence, None);
        ctx.device.destroy_command_pool(cmd_pool, None);
        ctx.device.destroy_pipeline(pipeline, None);
        ctx.device.destroy_pipeline_layout(pipeline_layout, None);
        ctx.device.destroy_shader_module(vert_mod, None);
        ctx.device.destroy_shader_module(frag_mod, None);
        ctx.device.destroy_framebuffer(framebuffer, None);
        ctx.device.destroy_render_pass(render_pass, None);
    }
    rt.destroy(&ctx.device);

    Ok(())
}
