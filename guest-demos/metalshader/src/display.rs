// DRM/GBM display management
#![cfg(target_os = "linux")]

use drm::control::{connector, crtc, framebuffer, Mode, ResourceHandle};
use drm::Device;
use gbm::{BufferObjectFlags, Device as GbmDevice, Format};
use std::fs::{File, OpenOptions};
use std::os::unix::io::{AsRawFd, RawFd};

pub struct Display {
    drm_fd: RawFd,
    _drm_file: File,
    gbm_device: GbmDevice<File>,
    bo: gbm::BufferObject<()>,
    fb_id: framebuffer::Handle,
    crtc_id: crtc::Handle,
    width: u32,
    height: u32,
    stride: u32,
}

impl Display {
    pub fn new() -> Result<Self, Box<dyn std::error::Error>> {
        // Open DRM device
        let drm_file = OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/dri/card0")?;

        let drm_fd = drm_file.as_raw_fd();

        // Set master
        unsafe {
            drm::ffi::drm_set_master(drm_fd)?;
        }

        // Get resources
        let res = drm::control::Device::resource_handles(&drm_file)?;

        // Find connected connector
        let connector_handle = res
            .connectors()
            .iter()
            .find_map(|&conn_handle| {
                let conn = drm::control::Device::get_connector(&drm_file, conn_handle, false).ok()?;
                if conn.state() == connector::State::Connected {
                    Some(conn_handle)
                } else {
                    None
                }
            })
            .ok_or("No connected display found")?;

        let connector = drm::control::Device::get_connector(&drm_file, connector_handle, false)?;

        // Get mode
        let mode = connector
            .modes()
            .get(0)
            .ok_or("No display mode available")?;

        let (width, height) = mode.size();

        // Get encoder and CRTC
        let encoder_handle = connector
            .current_encoder()
            .or_else(|| connector.encoders().get(0).copied())
            .ok_or("No encoder found")?;

        let encoder = drm::control::Device::get_encoder(&drm_file, encoder_handle)?;
        let crtc_id = encoder.crtc().unwrap_or_else(|| *res.crtcs().first().unwrap());

        // Create GBM device
        let gbm_device = GbmDevice::new(drm_file.try_clone()?)?;

        // Create buffer object
        let bo = gbm_device.create_buffer_object::<()>(
            width,
            height,
            Format::Xrgb8888,
            BufferObjectFlags::SCANOUT | BufferObjectFlags::RENDERING,
        )?;

        let stride = bo.stride()?;
        let handle = bo.handle()?;

        // Create framebuffer
        let fb_id = drm::control::Device::add_framebuffer(
            &drm_file,
            &bo,
            32, // depth
            32, // bpp
        )?;

        // Set CRTC
        drm::control::Device::set_crtc(
            &drm_file,
            crtc_id,
            Some(fb_id),
            (0, 0),
            &[connector_handle],
            Some(*mode),
        )?;

        Ok(Self {
            drm_fd,
            _drm_file: drm_file,
            gbm_device,
            bo,
            fb_id,
            crtc_id,
            width: width as u32,
            height: height as u32,
            stride,
        })
    }

    pub fn get_resolution(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn present(&mut self, frame_data: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
        // Map GBM buffer
        let mut map_data = std::ptr::null_mut();
        let mut stride = 0u32;

        let ptr = unsafe {
            gbm::ffi::gbm_bo_map(
                self.bo.as_raw(),
                0,
                0,
                self.width,
                self.height,
                gbm::ffi::GBM_BO_TRANSFER_WRITE,
                &mut stride as *mut _,
                &mut map_data,
            )
        };

        if ptr.is_null() {
            return Err("Failed to map GBM buffer".into());
        }

        // Copy frame data
        let bytes_per_pixel = 4;
        let row_size = self.width * bytes_per_pixel;

        for y in 0..self.height as usize {
            unsafe {
                let dst = ptr.add(y * stride as usize);
                let src = frame_data.as_ptr().add(y * row_size as usize);
                std::ptr::copy_nonoverlapping(src, dst as *mut u8, row_size as usize);
            }
        }

        // Unmap
        unsafe {
            gbm::ffi::gbm_bo_unmap(self.bo.as_raw(), map_data);
        }

        // Mark dirty
        unsafe {
            drm::ffi::drm_mode_dirty_fb(self.drm_fd, self.fb_id.into(), std::ptr::null(), 0)?;
        }

        Ok(())
    }
}
