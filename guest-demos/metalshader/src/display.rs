// DRM display management using DumbBuffer
#![cfg(target_os = "linux")]

use drm::control::{connector, crtc, framebuffer, Device as ControlDevice, dumbbuffer::DumbBuffer};
use drm::buffer::{Buffer, DrmFourcc};
use drm::Device;
use std::fs::{File, OpenOptions};
use std::os::unix::io::{AsFd, AsRawFd, BorrowedFd, RawFd};

/// Wrapper for DRM device that implements required traits
#[derive(Debug)]
struct DrmCard(File);

impl AsFd for DrmCard {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.0.as_fd()
    }
}

impl Device for DrmCard {}
impl ControlDevice for DrmCard {}

pub struct Display {
    drm_fd: RawFd,
    drm_card: DrmCard,
    dumb_buffer: DumbBuffer,
    fb_id: framebuffer::Handle,
    crtc_id: crtc::Handle,
    width: u32,
    height: u32,
}

impl Display {
    pub fn new() -> Result<Self, Box<dyn std::error::Error>> {
        // Open DRM device
        let drm_file = OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/dri/card0")
            .map_err(|e| format!("Failed to open /dev/dri/card0: {}", e))?;

        let drm_fd = drm_file.as_raw_fd();
        let drm_card = DrmCard(drm_file);

        // Get resources
        let res = drm_card.resource_handles()
            .map_err(|e| format!("Failed to get DRM resources: {}", e))?;

        // Find connected connector
        let connector_handle = res
            .connectors()
            .iter()
            .find_map(|&conn_handle| {
                let conn = drm_card.get_connector(conn_handle, true).ok()?;
                if conn.state() == connector::State::Connected {
                    Some(conn_handle)
                } else {
                    None
                }
            })
            .ok_or("No connected display found")?;

        let connector = drm_card.get_connector(connector_handle, true)?;

        // Get mode
        let mode = connector
            .modes()
            .get(0)
            .ok_or("No display mode available")?;

        let (width, height) = mode.size();

        // Get encoder and CRTC
        let crtc_id = connector
            .current_encoder()
            .and_then(|enc_handle| drm_card.get_encoder(enc_handle).ok())
            .and_then(|enc| enc.crtc())
            .or_else(|| res.crtcs().first().copied())
            .ok_or("No CRTC found")?;

        eprintln!("Creating dumb buffer: {}x{}", width, height);
        // Create DumbBuffer (CPU-accessible buffer for virtio-gpu)
        let dumb_buffer = drm_card.create_dumb_buffer(
            (width as u32, height as u32),
            DrmFourcc::Xrgb8888,
            32 // bpp
        ).map_err(|e| format!("Failed to create dumb buffer {}x{}: {}", width, height, e))?;

        eprintln!("Creating framebuffer");
        // Create framebuffer
        let fb_id = drm_card.add_framebuffer(&dumb_buffer, 24, 32)
            .map_err(|e| format!("Failed to add framebuffer: {}", e))?;

        eprintln!("Setting CRTC");
        // Set CRTC
        drm_card.set_crtc(
            crtc_id,
            Some(fb_id),
            (0, 0),
            &[connector_handle],
            Some(*mode),
        ).map_err(|e| format!("Failed to set CRTC: {}", e))?;

        Ok(Self {
            drm_fd,
            drm_card,
            dumb_buffer,
            fb_id,
            crtc_id,
            width: width as u32,
            height: height as u32,
        })
    }

    pub fn get_resolution(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn present(&mut self, frame_data: &[u8]) -> Result<(), Box<dyn std::error::Error>> {
        let bytes_per_pixel = 4;
        let row_size = self.width * bytes_per_pixel;
        let stride = self.dumb_buffer.pitch();  // Get pitch before mapping

        // Map DumbBuffer for CPU access
        let mut mapping = self.drm_card.map_dumb_buffer(&mut self.dumb_buffer)?;
        let buffer_slice = mapping.as_mut();

        for y in 0..self.height as usize {
            let dst_offset = y * stride as usize;
            let src_offset = y * row_size as usize;
            let copy_len = row_size.min((buffer_slice.len() - dst_offset) as u32) as usize;
            if dst_offset + copy_len <= buffer_slice.len() && src_offset + copy_len <= frame_data.len() {
                buffer_slice[dst_offset..dst_offset + copy_len]
                    .copy_from_slice(&frame_data[src_offset..src_offset + copy_len]);
            }
        }

        Ok(())
    }
}
