state of
vulkaninfo
...
BLOB_HEAD_READ[455]: head=600 test@0x30=0xcafe01e9 data=0x12befc000 (changed=1)
ERROR: [Loader Message] Code 0 : setup_loader_term_phys_devs:  Failed to detect any valid GPUs in the current config
BLOB_TAIL_WRITE: val=0x270 data=0x12befc000 ptr=0x12befc040 before=0x258 test@0x30=0xcafe0221
BLOB_TAIL_VERIFY: after=0x270 (expected 0x270)
VKR_DEBUG: vkr_renderer_submit_cmd: ctx_id=2 size=24
VKR_DISPATCH: cmd_type=190 (vkNotifyRingMESA) flags=0x0
VKR_DEBUG: vkr_renderer_submit_cmd: result=1
VKR_DISPATCH: cmd_type=1 (vkDestroyInstance) flags=0x0
VKR_HEAD_STORE: addr=0x101054000 before=600 storing=624 after=624
BLOB_HEAD_READ[458]: head=624 test@0x30=0xcafe0234 data=0x12befc000 (changed=1)
VKR_DEBUG: proxy_context_submit_cmd: success
VKR_DEBUG: vkr_renderer_submit_cmd: ctx_id=2 size=16
VKR_DISPATCH: cmd_type=189 (vkDestroyRingMESA) flags=0x0
VKR_DEBUG: vkr_renderer_submit_cmd: result=1
ERROR at /home/buildozer/aports/main/vulkan-tools/src/Vulkan-Tools-vulkan-sdk-1.4.321.0/vulkaninfo/./vulkaninfo.h:247:vkEnumeratePhysicalDevices failed with ERROR_INITIALIZATION_FAILED
                                             
  Looking at the summary, I need to continue debugging why vkEnumeratePhysicalDevices returns -3                             
  (VK_ERROR_INITIALIZATION_FAILED) on the guest despite:                                                                     
  1. Host successfully processing the command and returning count=1                                                          
  2. Reply data being correctly written to SHM                                                                               
  3. Guest correctly reading the reply data                                                                                  
                                                                                                                             
  The next steps suggested were:                                                                                             
  - Enable more verbose mesa debug output                                                                                    
  - Check protocol version mismatch                                                                                          
  - Examine reply encoding format                                                                                            
  - Check VK_KHR_portability_subset extension handling                                                                       
  - Look at mesa source for vn_EnumeratePhysicalDevices                                                                      
