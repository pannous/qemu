This is a fork of qemu to bring Vulkan -> Metal on macOS to Redox via MoltenVK

Associated repositories:
/opt/other/MoltenVK/
/opt/other/virglrenderer/ with venus
/opt/other/mesa/ on host  /root/mesa-25.0.2 on guest!!
and later
/opt/other/redox

All custom forks to get Vulkan rendering working on the Mac. 

rebuild qemu via
/opt/other/qemu/rebuild-qemu.sh

Already upstream
	•	virtio-gpu Venus backend
	•	Vulkan command forwarding
	•	Feature filtering
	•	Sync handling

We add (macOS-specific)
	•	Ensure Vulkan loader finds MoltenVK ICD
	•	Fix any missing fence / memory edge cases
	•	Possibly disable unsupported Vulkan extensions
  • ⚠️ don't use any OpenGL fallbacks
  • ⚠️ use IOSurface instead of dmabuf  
  • ⚠️ swap chain Directly on host, so nothing needs to be copied back to the guest for direct rendering 
  • test directly with vkcube --wsi display

The common WSI on Linux needs X11 or Wayland. In the Alpine VM, there's no display server.
  The solution involves creating a headless swapchain that's backed by blob resources instead of relying on a display      
  server. The swapchain images would use virtio-gpu blobs, and when the guest presents, it sets the blob as scanout for    
  QEMU to render to the display.  
  Mesa's Venus driver already implements WSI using wsi_common, without a display, vkcube can't create a surface.   

we use custom /opt/other/virglrenderer and vanilla /opt/homebrew/Cellar/molten-vk/1.4.0/ or source /opt/other/MoltenVK/

debug via ./scripts/debug-venus.sh ( tmux wrapper around ./scripts/run-alpine.sh or via terminal or ssh 2222  )
Do not yet debug with the Redox operating system!

Do all Guest development in /root/ so that we can reuse it after a reboot!
Try to cross-compile on the host if possible for much faster compilation. 

Do NOT use TCG, use HVF acceleration on macOS!
HVF has 16KB page, FIXED alignment issues with 4KB blob allocations:
We have a custom kernel with 16k pages alpine-virt-16k.img

Do not kill other QEMU sessions!
