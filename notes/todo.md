Add screen capture feature to QEMU?

Allow different color formats. 
      85 -    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888,                                            
      85 +    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,