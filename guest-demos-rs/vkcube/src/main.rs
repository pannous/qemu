//! Vulkan animated cube demo - renders spinning rainbow cube to DRM display

use ash::vk;
use std::time::Instant;
use vk_common::{
    vulkan::{load_spirv, DepthBuffer, RenderTarget, VkContext},
    DrmDisplay,
};

// Rainbow cube vertices: position (x,y,z) + color (r,g,b)
#[rustfmt::skip]
const CUBE_VERTS: &[[f32; 6]] = &[
    // Front face: red corner to yellow corner
    [-1.0,-1.0, 1.0,  1.0,0.0,0.0], [ 1.0,-1.0, 1.0,  1.0,1.0,0.0], [ 1.0, 1.0, 1.0,  0.0,1.0,0.0],
    [-1.0,-1.0, 1.0,  1.0,0.0,0.0], [ 1.0, 1.0, 1.0,  0.0,1.0,0.0], [-1.0, 1.0, 1.0,  1.0,0.0,1.0],
    // Back face: cyan to orange
    [ 1.0,-1.0,-1.0,  0.0,1.0,1.0], [-1.0,-1.0,-1.0,  1.0,0.5,0.0], [-1.0, 1.0,-1.0,  1.0,0.0,0.0],
    [ 1.0,-1.0,-1.0,  0.0,1.0,1.0], [-1.0, 1.0,-1.0,  1.0,0.0,0.0], [ 1.0, 1.0,-1.0,  0.0,0.0,1.0],
    // Top face: blue to yellow
    [-1.0, 1.0, 1.0,  0.0,0.0,1.0], [ 1.0, 1.0, 1.0,  1.0,1.0,0.0], [ 1.0, 1.0,-1.0,  1.0,0.0,1.0],
    [-1.0, 1.0, 1.0,  0.0,0.0,1.0], [ 1.0, 1.0,-1.0,  1.0,0.0,1.0], [-1.0, 1.0,-1.0,  0.0,1.0,0.0],
    // Bottom face: magenta to cyan
    [-1.0,-1.0,-1.0,  1.0,0.0,1.0], [ 1.0,-1.0,-1.0,  0.0,1.0,1.0], [ 1.0,-1.0, 1.0,  1.0,1.0,0.0],
    [-1.0,-1.0,-1.0,  1.0,0.0,1.0], [ 1.0,-1.0, 1.0,  1.0,1.0,0.0], [-1.0,-1.0, 1.0,  0.0,1.0,0.0],
    // Right face: green to red
    [ 1.0,-1.0, 1.0,  0.0,1.0,0.0], [ 1.0,-1.0,-1.0,  1.0,0.0,0.0], [ 1.0, 1.0,-1.0,  0.0,0.0,1.0],
    [ 1.0,-1.0, 1.0,  0.0,1.0,0.0], [ 1.0, 1.0,-1.0,  0.0,0.0,1.0], [ 1.0, 1.0, 1.0,  1.0,1.0,0.0],
    // Left face: white to black with colors
    [-1.0,-1.0,-1.0,  0.0,0.0,0.0], [-1.0,-1.0, 1.0,  1.0,0.0,0.0], [-1.0, 1.0, 1.0,  1.0,1.0,1.0],
    [-1.0,-1.0,-1.0,  0.0,0.0,0.0], [-1.0, 1.0, 1.0,  1.0,1.0,1.0], [-1.0, 1.0,-1.0,  0.0,0.0,1.0],
];

type Mat4 = [f32; 16];

fn mat4_identity() -> Mat4 {
    let mut m = [0.0; 16];
    m[0] = 1.0; m[5] = 1.0; m[10] = 1.0; m[15] = 1.0;
    m
}

fn mat4_mul(a: &Mat4, b: &Mat4) -> Mat4 {
    let mut out = [0.0; 16];
    for c in 0..4 {
        for r in 0..4 {
            out[c * 4 + r] = a[r] * b[c * 4]
                + a[4 + r] * b[c * 4 + 1]
                + a[8 + r] * b[c * 4 + 2]
                + a[12 + r] * b[c * 4 + 3];
        }
    }
    out
}

fn mat4_perspective(fovy: f32, aspect: f32, near: f32, far: f32) -> Mat4 {
    let mut m = [0.0; 16];
    let t = 1.0 / (fovy / 2.0).tan();
    m[0] = t / aspect;
    m[5] = -t;
    m[10] = far / (near - far);
    m[11] = -1.0;
    m[14] = near * far / (near - far);
    m
}

fn mat4_lookat(eye: [f32; 3], center: [f32; 3], up: [f32; 3]) -> Mat4 {
    let [ex, ey, ez] = eye;
    let [cx, cy, cz] = center;
    let [ux, uy, uz] = up;

    let fx = cx - ex; let fy = cy - ey; let fz = cz - ez;
    let fl = (fx * fx + fy * fy + fz * fz).sqrt();
    let (fx, fy, fz) = (fx / fl, fy / fl, fz / fl);

    let sx = fy * uz - fz * uy;
    let sy = fz * ux - fx * uz;
    let sz = fx * uy - fy * ux;
    let sl = (sx * sx + sy * sy + sz * sz).sqrt();
    let (sx, sy, sz) = (sx / sl, sy / sl, sz / sl);

    let uxn = sy * fz - sz * fy;
    let uyn = sz * fx - sx * fz;
    let uzn = sx * fy - sy * fx;

    let mut m = mat4_identity();
    m[0] = sx; m[4] = sy; m[8] = sz;
    m[1] = uxn; m[5] = uyn; m[9] = uzn;
    m[2] = -fx; m[6] = -fy; m[10] = -fz;
    m[12] = -(sx * ex + sy * ey + sz * ez);
    m[13] = -(uxn * ex + uyn * ey + uzn * ez);
    m[14] = fx * ex + fy * ey + fz * ez;
    m
}

fn mat4_rotate_y(angle: f32) -> Mat4 {
    let mut m = mat4_identity();
    let (s, c) = (angle.sin(), angle.cos());
    m[0] = c; m[8] = s; m[2] = -s; m[10] = c;
    m
}

fn mat4_rotate_x(angle: f32) -> Mat4 {
    let mut m = mat4_identity();
    let (s, c) = (angle.sin(), angle.cos());
    m[5] = c; m[9] = -s; m[6] = s; m[10] = c;
    m
}

struct Buffer {
    buffer: vk::Buffer,
    memory: vk::DeviceMemory,
    mapped: *mut u8,
}

impl Buffer {
    fn new(ctx: &VkContext, size: u64, usage: vk::BufferUsageFlags) -> Result<Self, String> {
        let info = vk::BufferCreateInfo::default()
            .size(size)
            .usage(usage);

        let buffer = unsafe { ctx.device.create_buffer(&info, None) }
            .map_err(|e| format!("Create buffer: {:?}", e))?;

        let req = unsafe { ctx.device.get_buffer_memory_requirements(buffer) };
        let mem_type = ctx
            .find_memory_type(
                req.memory_type_bits,
                vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
            )
            .ok_or("No host visible memory")?;

        let alloc = vk::MemoryAllocateInfo::default()
            .allocation_size(req.size)
            .memory_type_index(mem_type);

        let memory = unsafe { ctx.device.allocate_memory(&alloc, None) }
            .map_err(|e| format!("Alloc buffer: {:?}", e))?;

        unsafe { ctx.device.bind_buffer_memory(buffer, memory, 0) }
            .map_err(|e| format!("Bind buffer: {:?}", e))?;

        let mapped = unsafe {
            ctx.device.map_memory(memory, 0, size, vk::MemoryMapFlags::empty())
        }
        .map_err(|e| format!("Map buffer: {:?}", e))? as *mut u8;

        Ok(Self { buffer, memory, mapped })
    }

    fn write<T: Copy>(&self, data: &[T]) {
        unsafe {
            std::ptr::copy_nonoverlapping(
                data.as_ptr() as *const u8,
                self.mapped,
                std::mem::size_of_val(data),
            );
        }
    }

    fn destroy(&self, device: &ash::Device) {
        unsafe {
            device.unmap_memory(self.memory);
            device.destroy_buffer(self.buffer, None);
            device.free_memory(self.memory, None);
        }
    }
}

fn main() -> Result<(), String> {
    let display = DrmDisplay::open("/dev/dri/card0")?;
    let ctx = VkContext::new()?;
    let (w, h) = (display.width, display.height);

    println!("Rainbow Cube - {}x{}", w, h);

    let rt = RenderTarget::new(&ctx, w, h)?;
    let depth = DepthBuffer::new(&ctx, w, h)?;

    // Render pass with color + depth
    let attachments = [
        vk::AttachmentDescription::default()
            .format(vk::Format::B8G8R8A8_UNORM)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::GENERAL),
        vk::AttachmentDescription::default()
            .format(vk::Format::D32_SFLOAT)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
    ];

    let color_ref = [vk::AttachmentReference {
        attachment: 0,
        layout: vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
    }];
    let depth_ref = vk::AttachmentReference {
        attachment: 1,
        layout: vk::ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    let subpasses = [vk::SubpassDescription::default()
        .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
        .color_attachments(&color_ref)
        .depth_stencil_attachment(&depth_ref)];

    let render_pass = unsafe {
        ctx.device.create_render_pass(
            &vk::RenderPassCreateInfo::default()
                .attachments(&attachments)
                .subpasses(&subpasses),
            None,
        )
    }
    .map_err(|e| format!("Render pass: {:?}", e))?;

    let fb_attachments = [rt.view, depth.view];
    let framebuffer = unsafe {
        ctx.device.create_framebuffer(
            &vk::FramebufferCreateInfo::default()
                .render_pass(render_pass)
                .attachments(&fb_attachments)
                .width(w)
                .height(h)
                .layers(1),
            None,
        )
    }
    .map_err(|e| format!("Framebuffer: {:?}", e))?;

    // Shaders
    let vert_code = load_spirv("/root/cube.vert.spv")?;
    let frag_code = load_spirv("/root/cube.frag.spv")?;
    let vert_mod = ctx.create_shader_module(&vert_code).map_err(|e| format!("Vert: {:?}", e))?;
    let frag_mod = ctx.create_shader_module(&frag_code).map_err(|e| format!("Frag: {:?}", e))?;

    // Descriptor set layout for UBO
    let binding = [vk::DescriptorSetLayoutBinding::default()
        .binding(0)
        .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
        .descriptor_count(1)
        .stage_flags(vk::ShaderStageFlags::VERTEX)];

    let desc_layout = unsafe {
        ctx.device.create_descriptor_set_layout(
            &vk::DescriptorSetLayoutCreateInfo::default().bindings(&binding),
            None,
        )
    }
    .map_err(|e| format!("Desc layout: {:?}", e))?;

    let layouts = [desc_layout];
    let pipeline_layout = unsafe {
        ctx.device.create_pipeline_layout(
            &vk::PipelineLayoutCreateInfo::default().set_layouts(&layouts),
            None,
        )
    }
    .map_err(|e| format!("Pipeline layout: {:?}", e))?;

    // Pipeline
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

    let binding_desc = [vk::VertexInputBindingDescription {
        binding: 0,
        stride: 24, // 6 floats
        input_rate: vk::VertexInputRate::VERTEX,
    }];
    let attr_desc = [
        vk::VertexInputAttributeDescription {
            location: 0,
            binding: 0,
            format: vk::Format::R32G32B32_SFLOAT,
            offset: 0,
        },
        vk::VertexInputAttributeDescription {
            location: 1,
            binding: 0,
            format: vk::Format::R32G32B32_SFLOAT,
            offset: 12,
        },
    ];

    let vertex_input = vk::PipelineVertexInputStateCreateInfo::default()
        .vertex_binding_descriptions(&binding_desc)
        .vertex_attribute_descriptions(&attr_desc);

    let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
        .topology(vk::PrimitiveTopology::TRIANGLE_LIST);

    let viewports = [vk::Viewport {
        x: 0.0, y: 0.0,
        width: w as f32, height: h as f32,
        min_depth: 0.0, max_depth: 1.0,
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
        .cull_mode(vk::CullModeFlags::BACK)
        .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
        .line_width(1.0);

    let multisampling = vk::PipelineMultisampleStateCreateInfo::default()
        .rasterization_samples(vk::SampleCountFlags::TYPE_1);

    let depth_stencil = vk::PipelineDepthStencilStateCreateInfo::default()
        .depth_test_enable(true)
        .depth_write_enable(true)
        .depth_compare_op(vk::CompareOp::LESS);

    let blend_attachments = [vk::PipelineColorBlendAttachmentState::default()
        .color_write_mask(vk::ColorComponentFlags::RGBA)];
    let color_blending = vk::PipelineColorBlendStateCreateInfo::default()
        .attachments(&blend_attachments);

    let pipeline = unsafe {
        ctx.device.create_graphics_pipelines(
            vk::PipelineCache::null(),
            &[vk::GraphicsPipelineCreateInfo::default()
                .stages(&stages)
                .vertex_input_state(&vertex_input)
                .input_assembly_state(&input_assembly)
                .viewport_state(&viewport_state)
                .rasterization_state(&rasterizer)
                .multisample_state(&multisampling)
                .depth_stencil_state(&depth_stencil)
                .color_blend_state(&color_blending)
                .layout(pipeline_layout)
                .render_pass(render_pass)
                .subpass(0)],
            None,
        )
    }
    .map_err(|e| format!("Pipeline: {:?}", e.1))?[0];

    // Vertex buffer
    let vert_buf = Buffer::new(
        &ctx,
        std::mem::size_of_val(CUBE_VERTS) as u64,
        vk::BufferUsageFlags::VERTEX_BUFFER,
    )?;
    vert_buf.write(CUBE_VERTS);

    // Uniform buffer
    let ubo_buf = Buffer::new(&ctx, 64, vk::BufferUsageFlags::UNIFORM_BUFFER)?;

    // Descriptor pool and set
    let pool_sizes = [vk::DescriptorPoolSize {
        ty: vk::DescriptorType::UNIFORM_BUFFER,
        descriptor_count: 1,
    }];
    let desc_pool = unsafe {
        ctx.device.create_descriptor_pool(
            &vk::DescriptorPoolCreateInfo::default()
                .max_sets(1)
                .pool_sizes(&pool_sizes),
            None,
        )
    }
    .map_err(|e| format!("Desc pool: {:?}", e))?;

    let desc_set = unsafe {
        ctx.device.allocate_descriptor_sets(
            &vk::DescriptorSetAllocateInfo::default()
                .descriptor_pool(desc_pool)
                .set_layouts(&layouts),
        )
    }
    .map_err(|e| format!("Desc set: {:?}", e))?[0];

    let buffer_info = [vk::DescriptorBufferInfo {
        buffer: ubo_buf.buffer,
        offset: 0,
        range: 64,
    }];
    unsafe {
        ctx.device.update_descriptor_sets(
            &[vk::WriteDescriptorSet::default()
                .dst_set(desc_set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                .buffer_info(&buffer_info)],
            &[],
        );
    }

    // Command pool/buffer
    let cmd_pool = unsafe {
        ctx.device.create_command_pool(
            &vk::CommandPoolCreateInfo::default()
                .queue_family_index(0)
                .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER),
            None,
        )
    }
    .map_err(|e| format!("Cmd pool: {:?}", e))?;

    let cmd = unsafe {
        ctx.device.allocate_command_buffers(
            &vk::CommandBufferAllocateInfo::default()
                .command_pool(cmd_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1),
        )
    }
    .map_err(|e| format!("Cmd buffer: {:?}", e))?[0];

    let fence = unsafe { ctx.device.create_fence(&vk::FenceCreateInfo::default(), None) }
        .map_err(|e| format!("Fence: {:?}", e))?;

    // Matrices
    let proj = mat4_perspective(std::f32::consts::PI / 4.0, w as f32 / h as f32, 0.1, 100.0);
    let view = mat4_lookat([0.0, 2.0, 5.0], [0.0, 0.0, 0.0], [0.0, 1.0, 0.0]);

    println!("Spinning for 10s...");
    let start = Instant::now();
    let mut frames = 0;

    loop {
        let t = start.elapsed().as_secs_f32();
        if t > 10.0 {
            break;
        }

        // Update MVP
        let rot_y = mat4_rotate_y(t);
        let rot_x = mat4_rotate_x(t * 0.5);
        let model = mat4_mul(&rot_y, &rot_x);
        let mv = mat4_mul(&view, &model);
        let mvp = mat4_mul(&proj, &mv);
        ubo_buf.write(&mvp);

        // Record
        unsafe {
            ctx.device.begin_command_buffer(
                cmd,
                &vk::CommandBufferBeginInfo::default()
                    .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT),
            )
        }
        .map_err(|e| format!("Begin: {:?}", e))?;

        let clear_values = [
            vk::ClearValue { color: vk::ClearColorValue { float32: [0.02, 0.02, 0.05, 1.0] } },
            vk::ClearValue { depth_stencil: vk::ClearDepthStencilValue { depth: 1.0, stencil: 0 } },
        ];

        unsafe {
            ctx.device.cmd_begin_render_pass(
                cmd,
                &vk::RenderPassBeginInfo::default()
                    .render_pass(render_pass)
                    .framebuffer(framebuffer)
                    .render_area(vk::Rect2D {
                        offset: vk::Offset2D { x: 0, y: 0 },
                        extent: vk::Extent2D { width: w, height: h },
                    })
                    .clear_values(&clear_values),
                vk::SubpassContents::INLINE,
            );
            ctx.device.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipeline);
            ctx.device.cmd_bind_descriptor_sets(
                cmd,
                vk::PipelineBindPoint::GRAPHICS,
                pipeline_layout,
                0,
                &[desc_set],
                &[],
            );
            ctx.device.cmd_bind_vertex_buffers(cmd, 0, &[vert_buf.buffer], &[0]);
            ctx.device.cmd_draw(cmd, 36, 1, 0, 0);
            ctx.device.cmd_end_render_pass(cmd);
            ctx.device.end_command_buffer(cmd)
        }
        .map_err(|e| format!("End: {:?}", e))?;

        // Submit
        let submit = vk::SubmitInfo::default().command_buffers(std::slice::from_ref(&cmd));
        unsafe {
            ctx.device.queue_submit(ctx.queue, &[submit], fence)
                .map_err(|e| format!("Submit: {:?}", e))?;
            ctx.device.wait_for_fences(&[fence], true, u64::MAX)
                .map_err(|e| format!("Wait: {:?}", e))?;
            ctx.device.reset_fences(&[fence])
                .map_err(|e| format!("Reset fence: {:?}", e))?;
        }

        display.present(rt.mapped_ptr, rt.row_pitch as u32);
        frames += 1;
    }

    println!("Done! {} frames ({:.1} fps)", frames, frames as f32 / 10.0);

    // Cleanup
    unsafe {
        ctx.device.device_wait_idle().ok();
        ctx.device.destroy_fence(fence, None);
        ctx.device.destroy_command_pool(cmd_pool, None);
        ctx.device.destroy_descriptor_pool(desc_pool, None);
    }
    ubo_buf.destroy(&ctx.device);
    vert_buf.destroy(&ctx.device);
    unsafe {
        ctx.device.destroy_pipeline(pipeline, None);
        ctx.device.destroy_pipeline_layout(pipeline_layout, None);
        ctx.device.destroy_descriptor_set_layout(desc_layout, None);
        ctx.device.destroy_shader_module(vert_mod, None);
        ctx.device.destroy_shader_module(frag_mod, None);
        ctx.device.destroy_framebuffer(framebuffer, None);
        ctx.device.destroy_render_pass(render_pass, None);
    }
    depth.destroy(&ctx.device);
    rt.destroy(&ctx.device);

    Ok(())
}
