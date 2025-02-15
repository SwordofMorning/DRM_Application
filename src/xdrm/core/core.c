#include "core.h"

struct modeset_dev *modeset_list = NULL;

/* ======================================================================================================================== */
/* ================================================== Section 0 : Pattern ================================================== */
/* ======================================================================================================================== */

static int frame_count_test_pattern = 0;

// 直接生成ARGB测试图案
static void generate_test_pattern(uint32_t* argb_data, int width, int height, int frame_count) 
{
    // 简化的时间因子
    int offset = frame_count * 4;  // 增加速度
    
    for(int y = 0; y < height; y++) 
    {
        for(int x = 0; x < width; x++) 
        {
            // 生成RGB分量
            uint8_t r = (x + offset) & 0xFF;
            uint8_t g = (y + offset) & 0xFF;
            uint8_t b = (x + y + offset) & 0xFF;
            
            // 直接组装ARGB
            argb_data[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

// 彩条图案
static void generate_color_bars(uint32_t* argb_data, int width, int height, int frame_count)
{
    int bar_width = width / 8;
    int offset = frame_count * 8;  // 控制移动速度
    
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            int bar = ((x + offset) / bar_width) % 8;
            uint32_t color;
            
            switch(bar)
            {
                case 0: color = 0xFFFFFFFF; break; // 白
                case 1: color = 0xFFFFFF00; break; // 黄
                case 2: color = 0xFF00FFFF; break; // 青
                case 3: color = 0xFF00FF00; break; // 绿
                case 4: color = 0xFFFF00FF; break; // 品红
                case 5: color = 0xFFFF0000; break; // 红
                case 6: color = 0xFF0000FF; break; // 蓝
                case 7: color = 0xFF000000; break; // 黑
            }
            
            argb_data[y * width + x] = color;
        }
    }
}

// 棋盘图案
static void generate_checkerboard(uint32_t* argb_data, int width, int height, int frame_count)
{
    int square_size = 64;  // 更大的方格
    int offset = frame_count * 4;  // 移动速度
    
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            int px = (x + offset) / square_size;
            int py = (y + offset) / square_size;
            
            uint32_t color = ((px + py) & 1) ? 0xFFFFFFFF : 0xFF000000;
            argb_data[y * width + x] = color;
        }
    }
}

static void pattern(const uint8_t* nv12_data, uint32_t* argb_data, 
    int width, int height)
{
    // 每60帧切换一次模式
    switch((frame_count_test_pattern / 60) % 3) {
        case 0:
            generate_test_pattern(argb_data, width, height, frame_count_test_pattern);
            break;
        case 1:
            generate_color_bars(argb_data, width, height, frame_count_test_pattern);
            break;
        case 2:
            generate_checkerboard(argb_data, width, height, frame_count_test_pattern);
            break;
    }
    frame_count_test_pattern++;
}

/* ======================================================================================================================= */
/* ================================================== Section 1 : Basic ================================================== */
/* ======================================================================================================================= */

static uint32_t get_property_id(int fd, drmModeObjectProperties *props, const char *name)
{
    drmModePropertyPtr property;
    uint32_t i, prop_id = 0;

    for (i = 0; i < props->count_props; i++)
    {
        property = drmModeGetProperty(fd, props->props[i]);
        if (!property)
            continue;

        if (!strcmp(property->name, name))
            prop_id = property->prop_id;

        drmModeFreeProperty(property);
        if (prop_id)
            break;
    }

    return prop_id;
}

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj, const char *name, uint64_t value)
{
    uint32_t prop_id = 0;
    int i;

    for (i = 0; i < obj->props->count_props; i++)
    {
        if (!strcmp(obj->props_info[i]->name, name))
        {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id == 0)
    {
        fprintf(stderr, "Could not find property %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

static void modeset_get_object_properties(int fd, struct drm_object *obj, uint32_t type)
{
    obj->props = drmModeObjectGetProperties(fd, obj->id, type);
    if (!obj->props)
    {
        fprintf(stderr, "Cannot get object properties\n");
        return;
    }

    obj->props_info = (drmModePropertyRes **)calloc(obj->props->count_props, sizeof(*obj->props_info));
    for (int i = 0; i < obj->props->count_props; i++)
    {
        obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
    }
}

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    int ret;

    // create ARGB buffer
    memset(&creq, 0, sizeof(creq));
    creq.width = buf->width;
    creq.height = buf->height;
    creq.bpp = 32;
    creq.flags = 0;
    
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0)
    {
        fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
        return -errno;
    }

    buf->stride = creq.pitch;
    buf->size = creq.size;
    buf->handle = creq.handle;

    // crate framebuffer
    uint32_t handles[4] = {buf->handle};
    uint32_t pitches[4] = {buf->stride};
    uint32_t offsets[4] = {0};
    
    ret = drmModeAddFB2(fd, buf->width, buf->height,
                        DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                        &buf->fb, DRM_MODE_FB_MODIFIERS);
    if (ret)
    {
        fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
        ret = -errno;
        goto err_destroy;
    }

    // memory map
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = buf->handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    buf->map = (uint8_t *)mmap(0, buf->size,
                    PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
        ret = -errno;
        goto err_fb;
    }

    return 0;

err_fb:
    drmModeRmFB(fd, buf->fb);
err_destroy:
    struct drm_mode_destroy_dumb dreq;
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return ret;
}

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
    struct drm_mode_destroy_dumb dreq;

    // unmap
    munmap(buf->map, buf->size);

    // remove fb
    drmModeRmFB(fd, buf->fb);

    // destroy buffer
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = buf->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static int check_plane_capabilities(int fd, struct modeset_dev *dev)
{
    drmModePlane *plane = drmModeGetPlane(fd, dev->plane.id);
    if (!plane)
    {
        fprintf(stderr, "Cannot get plane %u\n", dev->plane.id);
        return -EINVAL;
    }

#if __ENABLE_DEBUG_LOG__
    printf("Plane info: id=%u, possible_crtcs=0x%x, formats_count=%u\n",
           plane->plane_id, plane->possible_crtcs, plane->count_formats);
#endif

    // get resource
    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources)
    {
        fprintf(stderr, "Cannot get DRM resources\n");
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    // checks whether the plane supports the specified CRTC
    uint32_t crtc_bit = 0;
    bool crtc_found = false;

    // traverse all encoders to find possible_crtcs related to CRTC
    for (int i = 0; i < resources->count_encoders; i++)
    {
        drmModeEncoder *encoder = drmModeGetEncoder(fd, resources->encoders[i]);
        if (!encoder)
            continue;

        drmModeCrtc *crtc = drmModeGetCrtc(fd, dev->crtc.id);
        if (crtc)
        {
            // if this encoder is currently connected to our CRTC
            if (encoder->crtc_id == dev->crtc.id)
            {
                crtc_bit = encoder->possible_crtcs;
                crtc_found = true;
                drmModeFreeCrtc(crtc);
                drmModeFreeEncoder(encoder);
                break;
            }
            drmModeFreeCrtc(crtc);
        }
        drmModeFreeEncoder(encoder);
    }

    drmModeFreeResources(resources);

    if (!crtc_found)
    {
        fprintf(stderr, "Could not find encoder for CRTC %u\n", dev->crtc.id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    if (!(plane->possible_crtcs & crtc_bit))
    {
        fprintf(stderr, "Plane %u cannot be used with CRTC %u\n",
                plane->plane_id, dev->crtc.id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    // checkout format
    bool format_supported = false;
    for (uint32_t i = 0; i < plane->count_formats; i++)
    {
        if (plane->formats[i] == DRM_FORMAT_ARGB8888)
        {
            format_supported = true;
            break;
        }
    }

    if (!format_supported)
    {
        fprintf(stderr, "Plane %u does not support ARGB8888 format\n",
                plane->plane_id);
        drmModeFreePlane(plane);
        return -EINVAL;
    }

    drmModeFreePlane(plane);
    return 0;
}

/* ======================================================================================================================== */
/* ================================================== Section 2 : Atomic ================================================== */
/* ======================================================================================================================== */

static int modeset_atomic_prepare_commit(int fd, struct modeset_dev *dev, drmModeAtomicReq *req, 
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];
    int ret;

    // only set necessary plane properties
    ret = set_drm_object_property(req, &dev->plane, "FB_ID", buf->fb);
    if (ret < 0) return ret;

    ret = set_drm_object_property(req, &dev->plane, "CRTC_ID", dev->crtc.id);
    if (ret < 0) return ret;

    // set source property
    ret = set_drm_object_property(req, &dev->plane, "SRC_X", 0);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_Y", 0);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_W", source_width << 16);
    ret |= set_drm_object_property(req, &dev->plane, "SRC_H", source_height << 16);
    if (ret < 0) return ret;

    // set display property
    ret = set_drm_object_property(req, &dev->plane, "CRTC_X", x_offset);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_Y", y_offset);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_W", source_width);
    ret |= set_drm_object_property(req, &dev->plane, "CRTC_H", source_height);

    // zpos
    ret = set_drm_object_property(req, &dev->plane, "zpos", 0);
    if (ret < 0) {
        fprintf(stderr, "Note: zpos property not supported\n");
    }

    return 0;
}

static int modeset_atomic_commit(int fd, struct modeset_dev *dev, uint32_t flags,
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    drmModeAtomicReq *req;
    int ret;

    req = drmModeAtomicAlloc();
    if (!req)
    {
        fprintf(stderr, "Failed to allocate atomic request\n");
        return -ENOMEM;
    }

    ret = modeset_atomic_prepare_commit(fd, dev, req, source_width, source_height, x_offset, y_offset);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to prepare atomic commit for plane %u\n", dev->plane.id);
        drmModeAtomicFree(req);
        return ret;
    }

    // @note without the NONBLOCK flag and use synchronous commit
    flags &= ~DRM_MODE_ATOMIC_NONBLOCK;

    ret = drmModeAtomicCommit(fd, req, flags, dev);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to commit atomic request for plane %u: %s\n",
                dev->plane.id, strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

int modeset_atomic_page_flip(int fd, struct modeset_dev *dev,
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    return modeset_atomic_commit(fd, dev, flags, source_width, source_height, x_offset, y_offset);
}

/* ====================================================================================================================== */
/* ================================================== Section 3 : APIs ================================================== */
/* ====================================================================================================================== */

int modeset_setup_dev(int fd, struct modeset_dev *dev, uint32_t conn_id, uint32_t crtc_id, uint32_t plane_id, 
    uint32_t source_width, uint32_t source_height)
{
    int ret;
    
    dev->connector.id = conn_id;
    dev->crtc.id = crtc_id;
    dev->plane.id = plane_id;

    // Step 1 : checkout plane
    ret = check_plane_capabilities(fd, dev);
    if (ret < 0) {
        fprintf(stderr, "Plane capability check failed\n");
        return ret;
    }

    // Step 2 : get connector information
    drmModeConnector *conn = drmModeGetConnector(fd, dev->connector.id);
    if (!conn)
    {
        fprintf(stderr, "cannot get connector %u: %m\n", dev->connector.id);
        return -errno;
    }

    // Step 3 : get the current display mode
    if (conn->count_modes <= 0)
    {
        fprintf(stderr, "no valid mode for connector %u\n", dev->connector.id);
        ret = -EFAULT;
        goto err_free;
    }
    
    memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));

    // Step 4 : set buffer display size, @note source!
    dev->bufs[0].width = source_width;
    dev->bufs[0].height = source_height;
    dev->bufs[1].width = source_width;
    dev->bufs[1].height = source_height;

    // Step 5 : set property blob
    ret = drmModeCreatePropertyBlob(fd, &dev->mode, sizeof(dev->mode),
                                   &dev->mode_blob_id);
    if (ret) {
        fprintf(stderr, "cannot create mode blob: %m\n");
        goto err_free;
    }

    // Step 6 : get properties
    modeset_get_object_properties(fd, &dev->connector, DRM_MODE_OBJECT_CONNECTOR);
    modeset_get_object_properties(fd, &dev->crtc, DRM_MODE_OBJECT_CRTC);
    modeset_get_object_properties(fd, &dev->plane, DRM_MODE_OBJECT_PLANE);

    // Step 7 : create frame buffer
    ret = modeset_create_fb(fd, &dev->bufs[0]);
    if (ret)
        goto err_blob;

    ret = modeset_create_fb(fd, &dev->bufs[1]);
    if (ret)
        goto err_fb0;

    drmModeFreeConnector(conn);
    return 0;

err_fb0:
    modeset_destroy_fb(fd, &dev->bufs[0]);
err_blob:
    drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);
err_free:
    drmModeFreeConnector(conn);
    return ret;
}

int modeset_atomic_modeset(int fd, struct modeset_dev *dev, 
    uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    int ret;
    uint32_t flags;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        return -ENOMEM;
    }

    ret = modeset_atomic_prepare_commit(fd, dev, req, source_width, source_height, x_offset, y_offset);
    if (ret < 0) {
        drmModeAtomicFree(req);
        return ret;
    }

    // use the least privilege flag
    flags = DRM_MODE_ATOMIC_NONBLOCK;
    ret = drmModeAtomicCommit(fd, req, flags, dev);
    if (ret < 0) {
        printf("Atomic modeset failed: %s\n", strerror(errno));
    }

    drmModeAtomicFree(req);
    return ret;
}

int modeset_atomic_init(int fd)
{
    uint64_t cap;
    int ret;

    ret = drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap);
    if (ret || !cap)
    {
        fprintf(stderr, "Device does not support atomic modesetting\n");
        return -ENOTSUP;
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret)
    {
        fprintf(stderr, "Failed to set universal planes cap\n");
        return ret;
    }

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret)
    {
        fprintf(stderr, "Failed to set atomic cap\n");
        return ret;
    }

    return 0;
}

void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
    // uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    struct modeset_dev *dev = (struct modeset_dev *)data;
    struct shared_memory *shm = (struct shared_memory *)dev->user_data;
    struct sembuf sem_op;
    
    dev->pflip_pending = false;

    if (!dev->cleanup)
    {
        /*
        // 等待信号量
        sem_op.sem_num = 0;
        sem_op.sem_op = -1;
        sem_op.sem_flg = 0;
        if (semop(shm->semid, &sem_op, 1) >= 0)
        {
            // 获取当前缓冲区
            struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];
            
            // 转换并复制图像数据
            nv12_to_argb((uint8_t*)shm->addr, (uint32_t*)buf->map, 640, 512);

            // 释放信号量
            sem_op.sem_op = 1;
            semop(shm->semid, &sem_op, 1);

            // 提交新帧
            int ret = modeset_atomic_page_flip(fd, dev);
            if (ret >= 0) {
                dev->front_buf ^= 1;
                dev->pflip_pending = true;
                
                // 增加帧计数
                frame_count_test_pattern++;
                
                // 可选：添加延时控制
                usleep(16666); // 约60fps
            }
        }
        */

        // get current buffer
        struct modeset_buf *buf = &dev->bufs[dev->front_buf ^ 1];

        // push data
        pattern(NULL, (uint32_t*)buf->map, 640, 512);

        // commit
        int ret = modeset_atomic_page_flip(fd, dev, 640, 512, 0, 0);
        if (ret >= 0)
        {
            dev->front_buf ^= 1;
            dev->pflip_pending = true;

            frame_count_test_pattern++;

            usleep(16666);
        }
    }
}

void modeset_draw(int fd, struct modeset_dev *dev, uint32_t source_width, uint32_t source_height, int x_offset, int y_offset)
{
    struct pollfd fds[1];
    int ret;
    struct fps_stats fps_stats;
    
    // 初始化事件上下文
    drmEventContext ev = {};
    memset(&ev, 0, sizeof(ev));
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler2 = page_flip_handler;
    ev.vblank_handler = NULL;
    
    // 初始化FPS统计
    xDRM_Init_FPS_Stats(&fps_stats);
    
    // 设置DRM事件的文件描述符
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    
    // 执行初始页面翻转
re_flip:
    ret = modeset_atomic_page_flip(fd, dev, source_width, source_height, x_offset, y_offset);
    if (ret) {
        fprintf(stderr, "Initial page flip failed: %s\n", strerror(errno));
        goto re_flip;
    }
    
    dev->front_buf ^= 1;
    dev->pflip_pending = true;

    // 主循环
    while (1) {
        fds[0].revents = 0;

        ret = poll(fds, 1, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            printf("poll failed: %s\n", strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN) {
            ret = drmHandleEvent(fd, &ev);
            if (ret != 0) {
                printf("drmHandleEvent failed: %s\n", strerror(errno));
                break;
            }
            
            // 更新FPS统计
            xDRM_Update_FPS_Stats(&fps_stats);
        }
    }
}

void modeset_cleanup(int fd, struct modeset_dev *dev)
{
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler2 = page_flip_handler,
    };

    dev->cleanup = true;

    while (dev->pflip_pending) {
        drmHandleEvent(fd, &ev);
    }

    // 清理plane
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (req) {
        // 只清理plane，不触碰CRTC
        set_drm_object_property(req, &dev->plane, "FB_ID", 0);
        set_drm_object_property(req, &dev->plane, "CRTC_ID", 0);
        drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        drmModeAtomicFree(req);
    }

    // 清理资源
    modeset_destroy_fb(fd, &dev->bufs[0]);
    modeset_destroy_fb(fd, &dev->bufs[1]);
    drmModeDestroyPropertyBlob(fd, dev->mode_blob_id);

    // 清理属性资源
    for (int i = 0; i < dev->connector.props->count_props; i++)
        drmModeFreeProperty(dev->connector.props_info[i]);
    for (int i = 0; i < dev->crtc.props->count_props; i++)
        drmModeFreeProperty(dev->crtc.props_info[i]);
    for (int i = 0; i < dev->plane.props->count_props; i++)
        drmModeFreeProperty(dev->plane.props_info[i]);

    free(dev->connector.props_info);
    free(dev->crtc.props_info);
    free(dev->plane.props_info);

    drmModeFreeObjectProperties(dev->connector.props);
    drmModeFreeObjectProperties(dev->crtc.props);
    drmModeFreeObjectProperties(dev->plane.props);
}