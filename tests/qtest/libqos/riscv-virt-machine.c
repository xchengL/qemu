#include "qemu/osdep.h"
#include "libqos-malloc.h"
#include "virtio-mmio.h"
#include "generic-pcihost.h"

#define RISCV_PAGE_SIZE             4096
#define VIRTIO_MMIO_ADDR            0x10008000
#define VIRTIO_MMIO_SIZE            0x1000
#define RISCV_VIRT_RAM_ADDR         0x80000000
#define RISCV_VIRT_RAM_SIZE         0x8000000

typedef struct QVirtMachine QVirtMachine;

struct QVirtMachine {
    QOSGraphObject obj;
    QGuestAllocator alloc;
    QVirtioMMIODevice virtio_mmio;
    QGenericPCIHost bridge;
};

static void virt_destructor(QOSGraphObject *obj)
{
    QVirtMachine *machine = (QVirtMachine *) obj;
    alloc_destroy(&machine->alloc);
}

static void *virt_get_driver(void *object, const char *interface)
{
    QVirtMachine *machine = object;
    if (!g_strcmp0(interface, "memory")) {
        return &machine->alloc;
    }

    fprintf(stderr, "%s not present in riscv/virtio\n", interface);
    g_assert_not_reached();
}

static QOSGraphObject *virt_get_device(void *obj, const char *device)
{
    QVirtMachine *machine = obj;
    if (!g_strcmp0(device, "generic-pcihost")) {
        return &machine->bridge.obj;
    } else if (!g_strcmp0(device, "virtio-mmio")) {
        return &machine->virtio_mmio.obj;
    }

    fprintf(stderr, "%s not present in riscv/virt\n", device);
    g_assert_not_reached();
}

static void *qos_create_machine_riscv_virt(QTestState *qts)
{
    QVirtMachine *machine = g_new0(QVirtMachine, 1);

    alloc_init(&machine->alloc, 0,
               RISCV_VIRT_RAM_ADDR,
               RISCV_VIRT_RAM_ADDR + RISCV_VIRT_RAM_SIZE,
               RISCV_PAGE_SIZE);
    qvirtio_mmio_init_device(&machine->virtio_mmio, qts, VIRTIO_MMIO_ADDR,
                             VIRTIO_MMIO_SIZE);

    qos_create_generic_pcihost(&machine->bridge, qts, &machine->alloc);

    machine->obj.get_device = virt_get_device;
    machine->obj.get_driver = virt_get_driver;
    machine->obj.destructor = virt_destructor;
    return machine;
}

static void virt_machine_register_nodes(void)
{
    qos_node_create_machine("riscv/virt", qos_create_machine_riscv_virt);
    qos_node_contains("riscv/virt", "virtio-mmio", NULL);

    qos_node_create_machine_args("riscv64/virt", qos_create_machine_riscv_virt,
                                 NULL);
    qos_node_contains("riscv64/virt", "virtio-mmio", NULL);
}

libqos_init(virt_machine_register_nodes);
