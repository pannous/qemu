// Keyboard input handling via Linux input events
#![cfg(target_os = "linux")]

use input_linux::{EventKind, InputEvent, Key, GenericEvent};
use std::fs::{File, OpenOptions};
use std::io;
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;

pub enum KeyEvent {
    Left,
    Right,
    Fullscreen,
    Quit,
}

pub struct KeyboardInput {
    device: Option<File>,
}

impl KeyboardInput {
    pub fn new() -> Result<Self, Box<dyn std::error::Error>> {
        // Try to find a keyboard device
        eprintln!("Scanning for keyboard input devices...");
        for i in 0..10 {
            let path = format!("/dev/input/event{}", i);
            if let Ok(file) = OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_NONBLOCK)
                .open(&path)
            {
                // Try to get device name to verify it's a keyboard
                let name = get_device_name(file.as_raw_fd());
                eprintln!("  {}: {}", path, name);
                if name.to_lowercase().contains("keyboard") || name.to_lowercase().contains("input") {
                    println!("Using input: {} ({})", path, name);
                    return Ok(Self { device: Some(file) });
                }
            }
        }

        println!("Warning: No keyboard input found, arrow key navigation disabled");
        Ok(Self { device: None })
    }

    pub fn poll_event(&mut self) -> Option<KeyEvent> {
        let device = self.device.as_mut()?;

        // Read events in non-blocking mode
        loop {
            let mut event = InputEvent::zeroed();
            match read_input_event(device, &mut event) {
                Ok(true) => {
                    // Check for key press events (value == 1 means press, not release)
                    if event.kind == EventKind::Key && event.value() == 1 {
                        // Get key code from event
                        if let Ok(key) = Key::from_code(event.code) {
                            match key {
                                Key::Left => return Some(KeyEvent::Left),
                                Key::Right => return Some(KeyEvent::Right),
                                Key::F => return Some(KeyEvent::Fullscreen),
                                Key::Esc | Key::Q => return Some(KeyEvent::Quit),
                                _ => {}
                            }
                        }
                    }
                }
                Ok(false) => return None, // No more events
                Err(_) => return None,
            }
        }
    }
}

fn get_device_name(fd: i32) -> String {
    // EVIOCGNAME ioctl number for getting device name
    // On aarch64 Linux, use proper ioctl request
    const EVIOCGNAME_256: libc::c_int = 0x4506;
    let mut name = vec![0u8; 256];
    unsafe {
        if libc::ioctl(fd, EVIOCGNAME_256 as libc::c_ulong as libc::c_int, name.as_mut_ptr()) >= 0 {
            let len = name.iter().position(|&c| c == 0).unwrap_or(name.len());
            String::from_utf8_lossy(&name[..len]).to_string()
        } else {
            "Unknown".to_string()
        }
    }
}

fn read_input_event(file: &mut File, event: &mut InputEvent) -> io::Result<bool> {
    use std::io::Read;

    let event_bytes = unsafe {
        std::slice::from_raw_parts_mut(
            event as *mut _ as *mut u8,
            std::mem::size_of::<InputEvent>(),
        )
    };

    match file.read_exact(event_bytes) {
        Ok(_) => Ok(true),
        Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(false),
        Err(e) => Err(e),
    }
}
