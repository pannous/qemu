//! DRM/KMS display handling using dumb buffers

use std::fs::{File, OpenOptions};
use std::os::unix::io::{AsRawFd, RawFd};

#[repr(C)]
struct DrmModeRes {
    fb_id_ptr: u64,
    crtc_id_ptr: u64,
    connector_id_ptr: u64,
    encoder_id_ptr: u64,
    count_fbs: u32,
    count_crtcs: u32,
    count_connectors: u32,
    count_encoders: u32,
    min_width: u32,
    max_width: u32,
    min_height: u32,
    max_height: u32,
}

#[repr(C)]
struct DrmModeGetConnector {
    encoders_ptr: u64,
    modes_ptr: u64,
    props_ptr: u64,
    prop_values_ptr: u64,
    count_modes: u32,
    count_props: u32,
    count_encoders: u32,
    encoder_id: u32,
    connector_id: u32,
    connector_type: u32,
    connector_type_id: u32,
    connection: u32,
    mm_width: u32,
    mm_height: u32,
    subpixel: u32,
    _pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DrmModeModeInfo {
    pub clock: u32,
    pub hdisplay: u16,
    pub hsync_start: u16,
    pub hsync_end: u16,
    pub htotal: u16,
    pub hskew: u16,
    pub vdisplay: u16,
    pub vsync_start: u16,
    pub vsync_end: u16,
    pub vtotal: u16,
    pub vscan: u16,
    pub vrefresh: u32,
    pub flags: u32,
    pub type_: u32,
    pub name: [i8; 32],
}

#[repr(C)]
struct DrmModeGetEncoder {
    encoder_id: u32,
    encoder_type: u32,
    crtc_id: u32,
    possible_crtcs: u32,
    possible_clones: u32,
}

#[repr(C)]
struct DrmModeCreateDumb {
    height: u32,
    width: u32,
    bpp: u32,
    flags: u32,
    handle: u32,
    pitch: u32,
    size: u64,
}

#[repr(C)]
struct DrmModeMapDumb {
    handle: u32,
    _pad: u32,
    offset: u64,
}

#[repr(C)]
struct DrmModeFbCmd {
    fb_id: u32,
    width: u32,
    height: u32,
    pitch: u32,
    bpp: u32,
    depth: u32,
    handle: u32,
}

#[repr(C)]
struct DrmModeCrtc {
    set_connectors_ptr: u64,
    count_connectors: u32,
    crtc_id: u32,
    fb_id: u32,
    x: u32,
    y: u32,
    gamma_size: u32,
    mode_valid: u32,
    mode: DrmModeModeInfo,
}

const DRM_IOCTL_BASE: u64 = 0x64;
const fn drm_iowr(nr: u64, size: u64) -> u64 {
    0xC000_0000 | ((size & 0x1FFF) << 16) | (DRM_IOCTL_BASE << 8) | nr
}
const fn drm_iow(nr: u64, size: u64) -> u64 {
    0x4000_0000 | ((size & 0x1FFF) << 16) | (DRM_IOCTL_BASE << 8) | nr
}

const DRM_IOCTL_MODE_GETRESOURCES: u64 = drm_iowr(0xA0, std::mem::size_of::<DrmModeRes>() as u64);
const DRM_IOCTL_MODE_GETCONNECTOR: u64 = drm_iowr(0xA7, std::mem::size_of::<DrmModeGetConnector>() as u64);
const DRM_IOCTL_MODE_GETENCODER: u64 = drm_iowr(0xA6, std::mem::size_of::<DrmModeGetEncoder>() as u64);
const DRM_IOCTL_MODE_CREATE_DUMB: u64 = drm_iowr(0xB2, std::mem::size_of::<DrmModeCreateDumb>() as u64);
const DRM_IOCTL_MODE_MAP_DUMB: u64 = drm_iowr(0xB3, std::mem::size_of::<DrmModeMapDumb>() as u64);
const DRM_IOCTL_MODE_ADDFB: u64 = drm_iowr(0xAE, std::mem::size_of::<DrmModeFbCmd>() as u64);
const DRM_IOCTL_MODE_SETCRTC: u64 = drm_iow(0xA2, std::mem::size_of::<DrmModeCrtc>() as u64);

const DRM_MODE_CONNECTED: u32 = 1;

unsafe fn ioctl(fd: RawFd, request: u64, arg: *mut libc::c_void) -> i32 {
    unsafe { libc::ioctl(fd, request, arg) }
}

pub struct DrmDisplay {
    file: File,
    pub width: u32,
    pub height: u32,
    fb_id: u32,
    fb_ptr: *mut u8,
    fb_size: usize,
    pub pitch: u32,
    crtc_id: u32,
    connector_id: u32,
    mode: DrmModeModeInfo,
}

impl DrmDisplay {
    pub fn open(device: &str) -> Result<Self, String> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(device)
            .map_err(|e| format!("Failed to open {}: {}", device, e))?;

        let fd = file.as_raw_fd();

        // Get resources
        let mut res = unsafe { std::mem::zeroed::<DrmModeRes>() };
        if unsafe { ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &mut res as *mut _ as *mut _) } != 0 {
            return Err("Failed to get DRM resources".into());
        }

        let mut connectors = vec![0u32; res.count_connectors as usize];
        let mut crtcs = vec![0u32; res.count_crtcs as usize];
        res.connector_id_ptr = connectors.as_mut_ptr() as u64;
        res.crtc_id_ptr = crtcs.as_mut_ptr() as u64;

        if unsafe { ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &mut res as *mut _ as *mut _) } != 0 {
            return Err("Failed to get DRM resources (2nd call)".into());
        }

        // Find connected connector
        let mut connector_id = 0u32;
        let mut encoder_id = 0u32;
        let mut mode = unsafe { std::mem::zeroed::<DrmModeModeInfo>() };

        for &conn_id in &connectors {
            let mut conn = unsafe { std::mem::zeroed::<DrmModeGetConnector>() };
            conn.connector_id = conn_id;

            if unsafe { ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &mut conn as *mut _ as *mut _) } != 0 {
                continue;
            }

            if conn.connection != DRM_MODE_CONNECTED || conn.count_modes == 0 {
                continue;
            }

            let mut modes = vec![unsafe { std::mem::zeroed::<DrmModeModeInfo>() }; conn.count_modes as usize];
            conn.modes_ptr = modes.as_mut_ptr() as u64;

            if unsafe { ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &mut conn as *mut _ as *mut _) } != 0 {
                continue;
            }

            connector_id = conn_id;
            encoder_id = conn.encoder_id;
            mode = modes[0];
            break;
        }

        if connector_id == 0 {
            return Err("No connected display found".into());
        }

        // Get CRTC ID from encoder
        let mut crtc_id = crtcs.first().copied().unwrap_or(0);
        if encoder_id != 0 {
            let mut enc = unsafe { std::mem::zeroed::<DrmModeGetEncoder>() };
            enc.encoder_id = encoder_id;
            if unsafe { ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &mut enc as *mut _ as *mut _) } == 0 {
                if enc.crtc_id != 0 {
                    crtc_id = enc.crtc_id;
                }
            }
        }

        let width = mode.hdisplay as u32;
        let height = mode.vdisplay as u32;

        // Create dumb buffer
        let mut create = DrmModeCreateDumb {
            height,
            width,
            bpp: 32,
            flags: 0,
            handle: 0,
            pitch: 0,
            size: 0,
        };

        if unsafe { ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &mut create as *mut _ as *mut _) } != 0 {
            return Err("Failed to create dumb buffer".into());
        }

        // Add framebuffer
        let mut fb_cmd = DrmModeFbCmd {
            fb_id: 0,
            width,
            height,
            pitch: create.pitch,
            bpp: 32,
            depth: 24,
            handle: create.handle,
        };

        if unsafe { ioctl(fd, DRM_IOCTL_MODE_ADDFB, &mut fb_cmd as *mut _ as *mut _) } != 0 {
            return Err("Failed to add framebuffer".into());
        }

        // Map dumb buffer
        let mut map = DrmModeMapDumb {
            handle: create.handle,
            _pad: 0,
            offset: 0,
        };

        if unsafe { ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mut map as *mut _ as *mut _) } != 0 {
            return Err("Failed to map dumb buffer".into());
        }

        let fb_ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                create.size as usize,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                map.offset as i64,
            )
        };

        if fb_ptr == libc::MAP_FAILED {
            return Err("Failed to mmap framebuffer".into());
        }

        println!("Display: {}x{}", width, height);

        Ok(Self {
            file,
            width,
            height,
            fb_id: fb_cmd.fb_id,
            fb_ptr: fb_ptr as *mut u8,
            fb_size: create.size as usize,
            pitch: create.pitch,
            crtc_id,
            connector_id,
            mode,
        })
    }

    /// Copy rendered image to framebuffer and display
    pub fn present(&self, src: *const u8, src_pitch: u32) {
        unsafe {
            for y in 0..self.height {
                let dst_row = self.fb_ptr.add((y * self.pitch) as usize);
                let src_row = src.add((y * src_pitch) as usize);
                std::ptr::copy_nonoverlapping(src_row, dst_row, (self.width * 4) as usize);
            }
        }

        let mut crtc = DrmModeCrtc {
            set_connectors_ptr: &self.connector_id as *const u32 as u64,
            count_connectors: 1,
            crtc_id: self.crtc_id,
            fb_id: self.fb_id,
            x: 0,
            y: 0,
            gamma_size: 0,
            mode_valid: 1,
            mode: self.mode,
        };

        unsafe {
            ioctl(self.file.as_raw_fd(), DRM_IOCTL_MODE_SETCRTC, &mut crtc as *mut _ as *mut _);
        }
    }
}

impl Drop for DrmDisplay {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.fb_ptr as *mut _, self.fb_size);
        }
    }
}
