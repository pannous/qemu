                                                                                           
  Current path (what ../guest-demos-copyback/ uses):                                                                                     
  Vulkan render → HOST_VISIBLE image → [COPY] → GBM blob → scanout                                                         
                                                                                                                           
  True zero-copy path (what we want):                                                                                     
  GBM blob ←import→ Vulkan image (same memory!) → render → scanout                                                         
                                                                                                                           
  The key is importing the GBM buffer's DMA-BUF fd into Vulkan via VK_KHR_external_memory_fd.
