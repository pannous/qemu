//! Common utilities for Vulkan guest demos with DRM/KMS display

pub mod drm;
pub mod vulkan;

pub use drm::DrmDisplay;
pub use vulkan::VkContext;
