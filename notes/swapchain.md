                                                                                                                 
  Current architecture WORKS:                                                                                                    
  - Guest app → Venus driver → virtio-gpu → virglrenderer → MoltenVK → Metal                                               
  - Venus proxies Vulkan commands to the host                                                                              
  - Swapchains are inherently tied to a display/window system                                                              
  - The guest has no window system (headless)                                                                              
  - Current solution: Guest renders to blob memory → GBM → DRM scanout → QEMU displays                                     
                                                                                                                             
  vkcube issue: It requires VK_KHR_swapchain which Venus doesn't expose (swapchains are host-side, not guest-side in this  

  Could we expose VK_KHR_swapchain?                                                                                        
                                                                                                                           
  Yes, but it would need to be a "virtual" swapchain implementation:                                                       
                                                                                                                           
  Option A: Proxy swapchain to host                                                                                        
  - Guest calls vkCreateSwapchain                                                                                          
  - Venus/virglrenderer intercepts and creates a real swapchain on the host (connected to QEMU's display)                  
  - Guest vkAcquireNextImage gets a blob-backed VkImage                                                                    
  - Guest renders to it                                                                                                    
  - Guest vkQueuePresent triggers host to present to the real swapchain                                                    
  - This is complex but would make standard Vulkan apps (like vkcube) work                                                 
                                                                                                                           
  Option B: Fake swapchain with blob backing                                                                               
  - Venus exposes VK_KHR_swapchain                                                                                         
  - vkCreateSwapchain creates blob-backed images                                                                           
  - vkQueuePresent sets the blob as DRM scanout                                                                            
  - This is simpler but requires the guest to use DRM display                                                              
                                                                                                                           
  Option C: WSI layer in Mesa                                                                                              
  - Mesa's Venus driver could implement a WSI layer that uses blobs                                                        
  - This would be client-side (in the guest Mesa)                                                                          
  - vkCreateSwapchain would create blob images                                                                             
                                                                                                                           
  - vkQueuePresent would do the DRM scanout                                                                                
                                                                                                                                                                   
                                     