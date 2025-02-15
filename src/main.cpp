#include "xdrm/xdrm.h"
#include <iostream>

int main(int argc, char *argv[])
{
    int fd, ret;
    struct modeset_dev *dev;

    // 打开DRM设备
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open card: %s\n", strerror(errno));
        return 1;
    }

    std::cout << "Set Atomic" << std::endl;

    // 初始化原子模式设置
    ret = modeset_atomic_init(fd);
    if (ret) {
        close(fd);
        return ret;
    }

    std::cout << "Malloc device" << std::endl;

    // 分配设备结构
    dev = (struct modeset_dev *)malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));

    std::cout << "Set device" << std::endl;

    // 设置设备
    ret = modeset_setup_dev(fd, dev, CONN_ID, CRTC_ID, PLANE_ID, 640, 512);
    if (ret) {
        free(dev);
        close(fd);
        return ret;
    }

    std::cout << "Init" << std::endl;

    // 执行初始模式设置
    ret = modeset_atomic_modeset(fd, dev, 640, 512, 0, 0);
    if (ret) {
        modeset_cleanup(fd, dev);
        free(dev);
        close(fd);
        return ret;
    }

    std::cout << "Main loop" << std::endl;

    // 运行主绘制循环
    modeset_draw(fd, dev, 640, 512, 0, 0);

    std::cout << "Out loop" << std::endl;

    // 清理
    modeset_cleanup(fd, dev);
    free(dev);
    close(fd);

    return 0;
}