╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌
 Fix HVF WFI Spurious Wakeup Bug                                                                    
                                                                                                    
 Problem Summary                                                                                    
                                                                                                    
 HVF's WFI (Wait For Interrupt) returns spuriously on macOS, causing 300% CPU usage during idle.    
 Current code returns EXCP_HLT but never sets cpu->halted = true, so QEMU's halt mechanism never    
 triggers proper sleep via qemu_cond_wait(). Instead, the vCPU thread tight-spins in the main loop. 
                                                                                                    
 Root Cause                                                                                         
                                                                                                    
 1. hvf_wfi() (hvf.c:1726) returns EXCP_HLT when no work, but doesn't set cpu->halted = true        
 2. cpu_thread_is_idle() (cpus.c:95) checks if (!cpu->halted || cpu_has_work(cpu)) and returns      
 FALSE                                                                                              
 3. qemu_process_cpu_events() (cpus.c:467) skips qemu_cond_wait() because CPU is not idle           
 4. Thread immediately re-enters hvf_arch_vcpu_exec() causing tight spinning (300% CPU)             
                                                                                                    
 Solution: Implement Proper Halt Mechanism                                                          
                                                                                                    
 Set cpu->halted = true in hvf_wfi() and clear it in hvf_arch_vcpu_exec() when work is available.   
 This matches the pattern used by TCG, NVMM, and WHPX accelerators.                                 
                                                                                                    
 Code Changes                                                                                       
                                                                                                    
 File: target/arm/hvf/hvf.c                                                                         
                                                                                                    
 Change 1: Set halted flag in hvf_wfi (line 1726)                                                   
                                                                                                    
 static int hvf_wfi(CPUState *cpu)                                                                  
 {                                                                                                  
     if (cpu_has_work(cpu)) {                                                                       
         /*                                                                                         
          * Don't bother to go into our "low power state" if                                        
          * we would just wake up immediately.                                                      
          */                                                                                        
         return 0;                                                                                  
     }                                                                                              
                                                                                                    
     /*                                                                                             
      * No work available, halt the CPU. The main loop will sleep                                   
      * on cpu->halt_cond until an interrupt or event wakes us.                                     
      */                                                                                            
     cpu->halted = true;                                                                            
     return EXCP_HLT;                                                                               
 }                                                                                                  
                                                                                                    
 Change 2: Clear halted flag in hvf_arch_vcpu_exec (line 2037)                                      
                                                                                                    
 int hvf_arch_vcpu_exec(CPUState *cpu)                                                              
 {                                                                                                  
     int ret;                                                                                       
     hv_return_t r;                                                                                 
                                                                                                    
     if (cpu->halted) {                                                                             
         if (!cpu_has_work(cpu)) {                                                                  
             /* Still no work, remain halted */                                                     
             return EXCP_HLT;                                                                       
         }                                                                                          
         /* We have work now, clear halt state and proceed */                                       
         cpu->halted = false;                                                                       
     }                                                                                              
                                                                                                    
     flush_cpu_state(cpu);                                                                          
                                                                                                    
     // ... rest of function unchanged                                                              
 }                                                                                                  
                                                                                                    
 How It Works                                                                                       
                                                                                                    
 1. WFI triggers halt:                                                                              
   - Guest executes WFI instruction                                                                 
   - hvf_wfi() checks cpu_has_work(), returns 0 if work available                                   
   - Otherwise sets cpu->halted = true and returns EXCP_HLT                                         
 2. Main loop sleeps properly:                                                                      
   - hvf_cpu_thread_fn() calls qemu_process_cpu_events()                                            
   - cpu_thread_is_idle() checks !cpu->halted || cpu_has_work(cpu)                                  
   - Since cpu->halted = true AND !cpu_has_work(), returns TRUE (idle)                              
   - qemu_process_cpu_events() calls qemu_cond_wait(cpu->halt_cond, &bql)                           
   - Thread blocks with near-zero CPU usage ✓                                                       
 3. Interrupt wakes CPU:                                                                            
   - Interrupt arrives, cpu_set_interrupt() calls qemu_cpu_kick()                                   
   - qemu_cpu_kick() broadcasts to cpu->halt_cond, waking thread                                    
   - Thread re-checks cpu_thread_is_idle()                                                          
   - Now cpu_has_work() = true, so cpu_thread_is_idle() = false                                     
   - Proceeds to call hvf_arch_vcpu_exec()                                                          
 4. Execution resumes:                                                                              
   - hvf_arch_vcpu_exec() checks cpu->halted = true                                                 
   - Checks cpu_has_work(), which returns TRUE                                                      
   - Clears cpu->halted = false and continues to execution loop                                     
   - Injects interrupts and runs vCPU normally ✓                                                    
                                                                                                    
 Expected Outcomes                                                                                  
                                                                                                    
 - Idle CPU usage: ~0-5% (proper sleep via qemu_cond_wait)                                          
 - Boot performance: Unchanged or better (< 2 sec for alpine)                                       
 - Responsiveness: No degradation (interrupts wake immediately via qemu_cpu_kick)                   
                                                                                                    
 Why This Wasn't Working Before                                                                     
                                                                                                    
 The upstream commit b5f8f77271 simplified WFI to just return EXCP_HLT, expecting the main loop to  
 handle halt properly. However, without setting cpu->halted = true, the halt mechanism never        
 triggered. This works fine on systems without spurious wakeups, but on macOS where HVF returns     
 spuriously from WFI, the missing halt causes tight spinning.                                       
                                                                                                    
 References to Similar Implementations                                                              
                                                                                                    
 - TCG: cpu_handle_halt() in accel/tcg/cpu-exec.c:653 checks halt and clears via cpu->halted = 0    
 - NVMM: target/i386/nvmm/nvmm-all.c:662 sets cpu->halted = true, clears at line 710                
 - WHPX: target/i386/whpx/whpx-all.c:1446 sets cpu->halted = true, clears at line 1620              
                                                                                                    
 Critical Files                                                                                     
                                                                                                    
 - /opt/other/qemu/target/arm/hvf/hvf.c:1726 - hvf_wfi function                                     
 - /opt/other/qemu/target/arm/hvf/hvf.c:2037 - hvf_arch_vcpu_exec halt check                        
 - /opt/other/qemu/system/cpus.c:467 - qemu_process_cpu_events (halt loop)                          
 - /opt/other/qemu/system/cpus.c:87 - cpu_thread_is_idle (idle check)                               
                                                                              