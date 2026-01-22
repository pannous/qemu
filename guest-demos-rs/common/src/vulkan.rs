//! Vulkan context and utilities for offscreen rendering

use ash::vk;
use std::ffi::CStr;

pub struct VkContext {
    pub entry: ash::Entry,
    pub instance: ash::Instance,
    pub physical_device: vk::PhysicalDevice,
    pub device: ash::Device,
    pub queue: vk::Queue,
    pub mem_props: vk::PhysicalDeviceMemoryProperties,
}

impl VkContext {
    pub fn new() -> Result<Self, String> {
        let entry = unsafe { ash::Entry::load() }.map_err(|e| format!("Failed to load Vulkan: {}", e))?;

        let app_info = vk::ApplicationInfo::default()
            .api_version(vk::make_api_version(0, 1, 0, 0));

        let create_info = vk::InstanceCreateInfo::default()
            .application_info(&app_info);

        let instance = unsafe { entry.create_instance(&create_info, None) }
            .map_err(|e| format!("Failed to create instance: {:?}", e))?;

        let physical_devices = unsafe { instance.enumerate_physical_devices() }
            .map_err(|e| format!("Failed to enumerate GPUs: {:?}", e))?;

        let physical_device = physical_devices
            .into_iter()
            .next()
            .ok_or("No GPU found")?;

        let props = unsafe { instance.get_physical_device_properties(physical_device) };
        let name = unsafe { CStr::from_ptr(props.device_name.as_ptr()) };
        println!("GPU: {}", name.to_string_lossy());

        let mem_props = unsafe { instance.get_physical_device_memory_properties(physical_device) };

        let queue_priority = 1.0f32;
        let queue_info = vk::DeviceQueueCreateInfo::default()
            .queue_family_index(0)
            .queue_priorities(std::slice::from_ref(&queue_priority));

        let device_info = vk::DeviceCreateInfo::default()
            .queue_create_infos(std::slice::from_ref(&queue_info));

        let device = unsafe { instance.create_device(physical_device, &device_info, None) }
            .map_err(|e| format!("Failed to create device: {:?}", e))?;

        let queue = unsafe { device.get_device_queue(0, 0) };

        Ok(Self {
            entry,
            instance,
            physical_device,
            device,
            queue,
            mem_props,
        })
    }

    pub fn find_memory_type(&self, type_bits: u32, properties: vk::MemoryPropertyFlags) -> Option<u32> {
        for i in 0..self.mem_props.memory_type_count {
            if (type_bits & (1 << i)) != 0 {
                let mem_type = self.mem_props.memory_types[i as usize];
                if mem_type.property_flags.contains(properties) {
                    return Some(i);
                }
            }
        }
        None
    }

    pub fn create_shader_module(&self, code: &[u8]) -> Result<vk::ShaderModule, vk::Result> {
        let code_aligned: Vec<u32> = code
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();

        let create_info = vk::ShaderModuleCreateInfo::default()
            .code(&code_aligned);

        unsafe { self.device.create_shader_module(&create_info, None) }
    }
}

impl Drop for VkContext {
    fn drop(&mut self) {
        unsafe {
            self.device.destroy_device(None);
            self.instance.destroy_instance(None);
        }
    }
}

/// Render target with CPU-visible memory for readback
pub struct RenderTarget {
    pub image: vk::Image,
    pub memory: vk::DeviceMemory,
    pub view: vk::ImageView,
    pub width: u32,
    pub height: u32,
    pub mapped_ptr: *mut u8,
    pub row_pitch: u64,
}

impl RenderTarget {
    pub fn new(ctx: &VkContext, width: u32, height: u32) -> Result<Self, String> {
        let image_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk::Format::B8G8R8A8_UNORM)
            .extent(vk::Extent3D { width, height, depth: 1 })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::LINEAR)
            .usage(vk::ImageUsageFlags::COLOR_ATTACHMENT);

        let image = unsafe { ctx.device.create_image(&image_info, None) }
            .map_err(|e| format!("Failed to create image: {:?}", e))?;

        let mem_req = unsafe { ctx.device.get_image_memory_requirements(image) };

        let mem_type_idx = ctx
            .find_memory_type(
                mem_req.memory_type_bits,
                vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
            )
            .ok_or("No suitable memory type")?;

        let alloc_info = vk::MemoryAllocateInfo::default()
            .allocation_size(mem_req.size)
            .memory_type_index(mem_type_idx);

        let memory = unsafe { ctx.device.allocate_memory(&alloc_info, None) }
            .map_err(|e| format!("Failed to allocate memory: {:?}", e))?;

        unsafe { ctx.device.bind_image_memory(image, memory, 0) }
            .map_err(|e| format!("Failed to bind memory: {:?}", e))?;

        let subresource = vk::ImageSubresource {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            mip_level: 0,
            array_layer: 0,
        };
        let layout = unsafe { ctx.device.get_image_subresource_layout(image, subresource) };

        let mapped_ptr = unsafe {
            ctx.device.map_memory(memory, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty())
        }
        .map_err(|e| format!("Failed to map memory: {:?}", e))? as *mut u8;

        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(vk::Format::B8G8R8A8_UNORM)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: 0,
                layer_count: 1,
            });

        let view = unsafe { ctx.device.create_image_view(&view_info, None) }
            .map_err(|e| format!("Failed to create image view: {:?}", e))?;

        Ok(Self {
            image,
            memory,
            view,
            width,
            height,
            mapped_ptr: unsafe { mapped_ptr.add(layout.offset as usize) },
            row_pitch: layout.row_pitch,
        })
    }

    pub fn destroy(&self, device: &ash::Device) {
        unsafe {
            device.unmap_memory(self.memory);
            device.destroy_image_view(self.view, None);
            device.destroy_image(self.image, None);
            device.free_memory(self.memory, None);
        }
    }
}

/// Depth buffer
pub struct DepthBuffer {
    pub image: vk::Image,
    pub memory: vk::DeviceMemory,
    pub view: vk::ImageView,
}

impl DepthBuffer {
    pub fn new(ctx: &VkContext, width: u32, height: u32) -> Result<Self, String> {
        let image_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk::Format::D32_SFLOAT)
            .extent(vk::Extent3D { width, height, depth: 1 })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT);

        let image = unsafe { ctx.device.create_image(&image_info, None) }
            .map_err(|e| format!("Failed to create depth image: {:?}", e))?;

        let mem_req = unsafe { ctx.device.get_image_memory_requirements(image) };

        let mem_type_idx = ctx
            .find_memory_type(mem_req.memory_type_bits, vk::MemoryPropertyFlags::DEVICE_LOCAL)
            .ok_or("No suitable memory type for depth")?;

        let alloc_info = vk::MemoryAllocateInfo::default()
            .allocation_size(mem_req.size)
            .memory_type_index(mem_type_idx);

        let memory = unsafe { ctx.device.allocate_memory(&alloc_info, None) }
            .map_err(|e| format!("Failed to allocate depth memory: {:?}", e))?;

        unsafe { ctx.device.bind_image_memory(image, memory, 0) }
            .map_err(|e| format!("Failed to bind depth memory: {:?}", e))?;

        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(vk::Format::D32_SFLOAT)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::DEPTH,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: 0,
                layer_count: 1,
            });

        let view = unsafe { ctx.device.create_image_view(&view_info, None) }
            .map_err(|e| format!("Failed to create depth view: {:?}", e))?;

        Ok(Self { image, memory, view })
    }

    pub fn destroy(&self, device: &ash::Device) {
        unsafe {
            device.destroy_image_view(self.view, None);
            device.destroy_image(self.image, None);
            device.free_memory(self.memory, None);
        }
    }
}

/// Load SPIR-V shader from file
pub fn load_spirv(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("Failed to load {}: {}", path, e))
}
