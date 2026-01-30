// DRM/GBM display management
#![cfg(target_os = "linux")]

use drm::control::{connector, crtc, framebuffer, Mode, ResourceHandle};
use drm::Device as DrmDevice;
use gbm::{BufferObjectFlags, Device as GbmDevice, Format, AsRaw};
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

        // Get resources (DRM device)
        let res = drm_file.resource_handles()?;

        // Find connected connector
        let connector_handle = res
            .connectors()
            .iter()
            .find_map(|&conn_handle| {
                let conn = drm_file.get_connector(conn_handle).ok()?;
                if conn.state() == connector::State::Connected {
                    Some(conn_handle)
                } else {
                    None
                }
            })
            .ok_or("No connected display found")?;

        let connector = drm_file.get_connector(connector_handle)?;

        // Get mode
        let mode = connector
            .modes()
            .get(0)
            .ok_or("No display mode available")?;

        let (width, height) = mode.size();

        // Get encoder and CRTC
        let crtc_id = connector
            .current_encoder()
            .and_then(|enc_handle| drm_file.get_encoder(enc_handle).ok())
            .and_then(|enc| enc.crtc())
            .or_else(|| res.crtcs().first().copied())
            .ok_or("No CRTC found")?;

        // Create GBM device
        let gbm_device = GbmDevice::new(drm_file.try_clone()?)?;

        // Create buffer object
        let bo = gbm_device.create_buffer_object::<()>(
            width as u32,
            height as u32,
            Format::Xrgb8888,
            BufferObjectFlags::SCANOUT | BufferObjectFlags::RENDERING,
        )?;

        let stride = bo.stride()?;

        // Create framebuffer using GBM device (which implements DRM)
        let fb_id = gbm_device.add_framebuffer(&bo, 32, 32)?;

        // Set CRTC
        gbm_device.set_crtc(
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
        // Map GBM buffer for writing
        let mut buffer = self.bo.map_mut(
            0,
            0,
            self.width,
            self.height,
            gbm::BufferAccessFlags::WRITE,
        )?;

        // Copy frame data
        let bytes_per_pixel = 4;
        let row_size = self.width * bytes_per_pixel;
        let stride = buffer.stride() as u32;

        // Access the buffer data slice
        let buffer_slice = buffer.as_mut();

        for y in 0..self.height as usize {
            let dst_offset = y * stride as usize;
            let src_offset = y * row_size as usize;
            let dst = &mut buffer_slice[dst_offset..dst_offset + row_size as usize];
            let src = &frame_data[src_offset..src_offset + row_size as usize];
            dst.copy_from_slice(src);
        }

        // Buffer automatically unmapped on drop

        Ok(())
    }
}
