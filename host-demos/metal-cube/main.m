#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

typedef struct {
    simd_float3 position;
    simd_float3 color;
} Vertex;

typedef struct {
    simd_float4x4 modelViewProjection;
} Uniforms;

static const Vertex cubeVertices[] = {
    // Front
    {{-1, -1,  1}, {1, 0, 0}}, {{ 1, -1,  1}, {0, 1, 0}}, {{ 1,  1,  1}, {0, 0, 1}}, {{-1,  1,  1}, {1, 1, 0}},
    // Back
    {{ 1, -1, -1}, {0, 1, 1}}, {{-1, -1, -1}, {1, 0, 1}}, {{-1,  1, -1}, {1, 1, 1}}, {{ 1,  1, -1}, {0.5, 0.5, 0.5}},
    // Left
    {{-1, -1, -1}, {1, 0, 1}}, {{-1, -1,  1}, {1, 0, 0}}, {{-1,  1,  1}, {1, 1, 0}}, {{-1,  1, -1}, {1, 1, 1}},
    // Right
    {{ 1, -1,  1}, {0, 1, 0}}, {{ 1, -1, -1}, {0, 1, 1}}, {{ 1,  1, -1}, {0.5, 0.5, 0.5}}, {{ 1,  1,  1}, {0, 0, 1}},
    // Top
    {{-1,  1,  1}, {1, 1, 0}}, {{ 1,  1,  1}, {0, 0, 1}}, {{ 1,  1, -1}, {0.5, 0.5, 0.5}}, {{-1,  1, -1}, {1, 1, 1}},
    // Bottom
    {{-1, -1, -1}, {1, 0, 1}}, {{ 1, -1, -1}, {0, 1, 1}}, {{ 1, -1,  1}, {0, 1, 0}}, {{-1, -1,  1}, {1, 0, 0}},
};

static const uint16_t cubeIndices[] = {
    0,  1,  2,  2,  3,  0,   // Front
    4,  5,  6,  6,  7,  4,   // Back
    8,  9, 10, 10, 11,  8,   // Left
    12, 13, 14, 14, 15, 12,  // Right
    16, 17, 18, 18, 19, 16,  // Top
    20, 21, 22, 22, 23, 20,  // Bottom
};

@interface MetalRenderer : NSObject
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLDepthStencilState> depthStencilState;
@property (nonatomic, strong) id<MTLBuffer> vertexBuffer;
@property (nonatomic, strong) id<MTLBuffer> indexBuffer;
@property (nonatomic, strong) id<MTLBuffer> uniformBuffer;
@property (nonatomic, strong) id<MTLTexture> depthTexture;
@property (nonatomic, assign) float rotation;
@property (nonatomic, assign) CFTimeInterval lastFrameTime;
@property (nonatomic, assign) NSUInteger frameCount;
@property (nonatomic, assign) NSUInteger totalFrames;
@property (nonatomic, assign) CFTimeInterval startTime;
@end

@implementation MetalRenderer

- (instancetype)initWithDevice:(id<MTLDevice>)device {
    self = [super init];
    if (self) {
        _device = device;
        _rotation = 0.0f;
        _lastFrameTime = CACurrentMediaTime();
        _frameCount = 0;
        _totalFrames = 0;
        _startTime = CACurrentMediaTime();
        [self setupMetal];
    }
    return self;
}

- (void)setupMetal {
    self.commandQueue = [self.device newCommandQueue];
    NSLog(@"Setting up Metal...");

    NSError *error = nil;
    NSString *shaderSource = @
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "\n"
        "struct Vertex {\n"
        "    float3 position [[attribute(0)]];\n"
        "    float3 color [[attribute(1)]];\n"
        "};\n"
        "\n"
        "struct VertexOut {\n"
        "    float4 position [[position]];\n"
        "    float3 color;\n"
        "};\n"
        "\n"
        "struct Uniforms {\n"
        "    float4x4 modelViewProjection;\n"
        "};\n"
        "\n"
        "vertex VertexOut vertex_main(Vertex in [[stage_in]],\n"
        "                             constant Uniforms& uniforms [[buffer(1)]]) {\n"
        "    VertexOut out;\n"
        "    out.position = uniforms.modelViewProjection * float4(in.position, 1.0);\n"
        "    out.color = in.color;\n"
        "    return out;\n"
        "}\n"
        "\n"
        "fragment float4 fragment_main(VertexOut in [[stage_in]]) {\n"
        "    return float4(in.color, 1.0);\n"
        "}\n";

    id<MTLLibrary> library = [self.device newLibraryWithSource:shaderSource options:nil error:&error];
    if (!library) {
        NSLog(@"Failed to create shader library: %@", error);
        exit(1);
    }

    id<MTLFunction> vertexFunc = [library newFunctionWithName:@"vertex_main"];
    if (!vertexFunc) {
        NSLog(@"Failed to load vertex function");
        exit(1);
    }

    id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"fragment_main"];
    if (!fragmentFunc) {
        NSLog(@"Failed to load fragment function");
        exit(1);
    }

    MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDesc.vertexFunction = vertexFunc;
    pipelineDesc.fragmentFunction = fragmentFunc;
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    MTLVertexDescriptor *vertexDesc = [[MTLVertexDescriptor alloc] init];
    vertexDesc.attributes[0].format = MTLVertexFormatFloat3;
    vertexDesc.attributes[0].offset = 0;
    vertexDesc.attributes[0].bufferIndex = 0;
    vertexDesc.attributes[1].format = MTLVertexFormatFloat3;
    vertexDesc.attributes[1].offset = sizeof(simd_float3);
    vertexDesc.attributes[1].bufferIndex = 0;
    vertexDesc.layouts[0].stride = sizeof(Vertex);
    vertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    pipelineDesc.vertexDescriptor = vertexDesc;

    pipelineDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    self.pipelineState = [self.device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    if (!self.pipelineState) {
        NSLog(@"Failed to create pipeline state: %@", error);
        exit(1);
    }

    self.vertexBuffer = [self.device newBufferWithBytes:cubeVertices
                                                 length:sizeof(cubeVertices)
                                                options:MTLResourceStorageModeShared];

    self.indexBuffer = [self.device newBufferWithBytes:cubeIndices
                                                length:sizeof(cubeIndices)
                                               options:MTLResourceStorageModeShared];

    self.uniformBuffer = [self.device newBufferWithLength:sizeof(Uniforms)
                                                  options:MTLResourceStorageModeShared];

    MTLDepthStencilDescriptor *depthDesc = [[MTLDepthStencilDescriptor alloc] init];
    depthDesc.depthCompareFunction = MTLCompareFunctionLess;
    depthDesc.depthWriteEnabled = YES;
    self.depthStencilState = [self.device newDepthStencilStateWithDescriptor:depthDesc];

    // Create depth texture
    MTLTextureDescriptor *depthTexDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                            width:800
                                                                                           height:600
                                                                                        mipmapped:NO];
    depthTexDesc.usage = MTLTextureUsageRenderTarget;
    depthTexDesc.storageMode = MTLStorageModePrivate;
    self.depthTexture = [self.device newTextureWithDescriptor:depthTexDesc];
}

- (simd_float4x4)perspectiveWithFovy:(float)fovy aspect:(float)aspect near:(float)near far:(float)far {
    float yScale = 1.0f / tanf(fovy * 0.5f);
    float xScale = yScale / aspect;
    float zRange = far - near;
    float zScale = -(far + near) / zRange;
    float wzScale = -2.0f * far * near / zRange;

    simd_float4 P = {xScale, 0, 0, 0};
    simd_float4 Q = {0, yScale, 0, 0};
    simd_float4 R = {0, 0, zScale, -1};
    simd_float4 S = {0, 0, wzScale, 0};

    return simd_matrix(P, Q, R, S);
}

- (void)renderFrame:(CAMetalLayer *)layer {
    static BOOL firstFrame = YES;
    static int callCount = 0;
    callCount++;

    if (firstFrame) {
        NSLog(@"First frame rendering...");
        self.startTime = CACurrentMediaTime();
        firstFrame = NO;
    }

    if (callCount < 5) {
        NSLog(@"renderFrame called %d", callCount);
    }

    self.frameCount++;
    self.totalFrames++;
    CFTimeInterval currentTime = CACurrentMediaTime();

    if (currentTime - self.lastFrameTime >= 1.0) {
        float instantFPS = self.frameCount / (currentTime - self.lastFrameTime);
        float avgFPS = self.totalFrames / (currentTime - self.startTime);
        fprintf(stderr, "FPS: %.1f (avg: %.1f over %llu frames)\n", instantFPS, avgFPS, (unsigned long long)self.totalFrames);
        fflush(stderr);
        self.frameCount = 0;
        self.lastFrameTime = currentTime;
    }

    self.rotation += 0.01f;

    // Rotation around Y axis, then around X axis
    float rotY = cosf(self.rotation);
    float rotYsin = sinf(self.rotation);
    float rotX = cosf(self.rotation * 0.7f);
    float rotXsin = sinf(self.rotation * 0.7f);

    simd_float4x4 modelMatrix = simd_matrix(
        (simd_float4){rotY, 0, rotYsin, 0},
        (simd_float4){rotYsin * rotXsin, rotX, -rotY * rotXsin, 0},
        (simd_float4){-rotYsin * rotX, rotXsin, rotY * rotX, 0},
        (simd_float4){0, 0, -5, 1}  // Closer to camera
    );

    simd_float4x4 viewMatrix = simd_matrix(
        (simd_float4){1, 0, 0, 0},
        (simd_float4){0, 1, 0, 0},
        (simd_float4){0, 0, 1, 0},
        (simd_float4){0, 0, 0, 1}
    );

    float aspect = (float)layer.drawableSize.width / (float)layer.drawableSize.height;
    simd_float4x4 projectionMatrix = [self perspectiveWithFovy:M_PI / 4.0f aspect:aspect near:0.1f far:100.0f];

    Uniforms *uniforms = (Uniforms *)[self.uniformBuffer contents];
    uniforms->modelViewProjection = simd_mul(projectionMatrix, simd_mul(viewMatrix, modelMatrix));

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor *renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPass.colorAttachments[0].texture = drawable.texture;
    renderPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    renderPass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    renderPass.colorAttachments[0].storeAction = MTLStoreActionStore;

    renderPass.depthAttachment.texture = self.depthTexture;
    renderPass.depthAttachment.loadAction = MTLLoadActionClear;
    renderPass.depthAttachment.storeAction = MTLStoreActionDontCare;
    renderPass.depthAttachment.clearDepth = 1.0;

    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
    [encoder setRenderPipelineState:self.pipelineState];
    [encoder setDepthStencilState:self.depthStencilState];
    [encoder setVertexBuffer:self.vertexBuffer offset:0 atIndex:0];
    [encoder setVertexBuffer:self.uniformBuffer offset:0 atIndex:1];
    [encoder setCullMode:MTLCullModeBack];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:sizeof(cubeIndices) / sizeof(uint16_t)
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:self.indexBuffer
                 indexBufferOffset:0];
    [encoder endEncoding];

    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
}

@end

// Custom NSView that creates CAMetalLayer with VSync disabled
@interface MetalView : NSView
@end

@implementation MetalView
- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = NO;  // Disable VSync at Metal layer level
    return layer;
}
- (BOOL)wantsUpdateLayer { return YES; }
@end

@interface AppDelegate : NSObject <NSApplicationDelegate> {
    dispatch_source_t _displaySource;
}
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) MetalView *metalView;
@property (nonatomic, strong) MetalRenderer *renderer;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSRect frame = NSMakeRect(0, 0, 800, 600);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable;

    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window setTitle:@"Metal Gradient Cube - Host Performance Baseline"];
    [self.window center];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        NSLog(@"Metal is not supported on this device");
        [NSApp terminate:nil];
        return;
    }

    self.metalView = [[MetalView alloc] initWithFrame:frame];
    [self.metalView setWantsLayer:YES];

    CAMetalLayer *metalLayer = (CAMetalLayer *)self.metalView.layer;
    metalLayer.device = device;
    metalLayer.drawableSize = CGSizeMake(800, 600);
    metalLayer.framebufferOnly = YES;

    // Check display refresh rate
    CGDirectDisplayID displayID = CGMainDisplayID();
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
    double refreshRate = CGDisplayModeGetRefreshRate(mode);
    if (refreshRate == 0) refreshRate = 60.0;
    CGDisplayModeRelease(mode);
    NSLog(@"Display refresh rate: %.1f Hz, VSync disabled: %d", refreshRate, !metalLayer.displaySyncEnabled);

    self.renderer = [[MetalRenderer alloc] initWithDevice:device];

    [self.window setContentView:self.metalView];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    NSLog(@"Metal Gradient Cube - Host Performance Baseline");
    NSLog(@"Press Cmd+Q to quit");

    // Start render loop using GCD for maximum performance
    NSLog(@"Creating dispatch source...");
    _displaySource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    NSLog(@"Setting timer...");
    dispatch_source_set_timer(_displaySource, DISPATCH_TIME_NOW, NSEC_PER_SEC / 10000, 0);  // 10000 Hz target, no leeway

    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(_displaySource, ^{
        @autoreleasepool {
            CAMetalLayer *layer = (CAMetalLayer *)weakSelf.metalView.layer;
            [weakSelf.renderer renderFrame:layer];
        }
    });

    dispatch_resume(_displaySource);
    NSLog(@"Render loop started");
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
