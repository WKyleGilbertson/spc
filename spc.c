#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>

#include "spc.h"

#define DRIVER_NAME            "spc"
#define DEVICE_BASE            0x41600000
#define DEVICE_SIZE            0x1000
#define CLEAR_INTERRUPT_OFFSET 0x80C
#define CLEAR_INTERRUPT_VALUE  0x80000000

// Channel register stride in spc_ctrlmux (relative to 0x41600000)
#define CH_STRIDE              0x80 // paddr[9:7] = 128 bytes per channel
#define OFF_IN                 0x08
#define OFF_QN                 0x0C
#define OFF_IE                 0x10
#define OFF_QE                 0x14
#define OFF_IP                 0x18
#define OFF_QP                 0x1C
#define OFF_IL                 0x20
#define OFF_QL                 0x24
#define OFF_CARR_PHASE         0x28
#define OFF_EPOCH_INFO         0x30 // [15:0] = epoch_idx, [17:16] = epoch_cnt, [31:24] = data_ready

#define FIFO_FRAME_COUNT       64

struct spc_dev {
    void __iomem *base;
    int irq;
    struct miscdevice misc;
    DECLARE_KFIFO(fifo, struct spc_epoch_data, FIFO_FRAME_COUNT);
    spinlock_t fifo_lock;
    wait_queue_head_t read_queue;
    uint32_t epoch_counter;
};

static struct spc_dev *g_spc;

// Top-Half Interrupt Handler (IRQ 176)
static irqreturn_t spc_isr(int irq, void *dev_id)
{
    struct spc_dev *dev = dev_id;
    struct spc_epoch_data frame;
    int ch;

    frame.system_epoch = dev->epoch_counter++;

    // Unrolled harvest across APB bus
    for (ch = 0; ch < SPC_NUM_CHANNELS; ch++) {
        void __iomem *ch_base = dev->base + (ch * CH_STRIDE);
        frame.ch[ch].in            = (int32_t)ioread32(ch_base + OFF_IN);
        frame.ch[ch].qn            = (int32_t)ioread32(ch_base + OFF_QN);
        frame.ch[ch].ie            = (int32_t)ioread32(ch_base + OFF_IE);
        frame.ch[ch].qe            = (int32_t)ioread32(ch_base + OFF_QE);
        frame.ch[ch].ip            = (int32_t)ioread32(ch_base + OFF_IP);
        frame.ch[ch].qp            = (int32_t)ioread32(ch_base + OFF_QP);
        frame.ch[ch].il            = (int32_t)ioread32(ch_base + OFF_IL);
        frame.ch[ch].ql            = (int32_t)ioread32(ch_base + OFF_QL);

        uint32_t epoch_info        = ioread32(ch_base + OFF_EPOCH_INFO);
        frame.ch[ch].epoch_idx     = (uint16_t)(epoch_info & 0xFFFF);
        frame.ch[ch].epoch_cnt     = (uint8_t)((epoch_info >> 16) & 0x03);
        frame.ch[ch].carrier_phase = ioread32(ch_base + OFF_CARR_PHASE);
        frame.ch[ch].data_ready    = 1;
    }

    // Acknowledge interrupt in FPGA fabric
    iowrite32(CLEAR_INTERRUPT_VALUE, dev->base + CLEAR_INTERRUPT_OFFSET);

    // Push into lock-free kfifo
    kfifo_put(&dev->fifo, frame);

    // Wake user-space daemon
    wake_up_interruptible(&dev->read_queue);

    return IRQ_HANDLED;
}

// File Operations
static ssize_t spc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct spc_dev *dev = g_spc;
    struct spc_epoch_data frame;
    int ret;

    if (count < sizeof(struct spc_epoch_data))
        return -EINVAL;

    ret = wait_event_interruptible(dev->read_queue, !kfifo_is_empty(&dev->fifo));
    if (ret)
        return ret;

    ret = kfifo_get(&dev->fifo, &frame);
    if (!ret)
        return -EAGAIN;

    if (copy_to_user(buf, &frame, sizeof(frame)))
        return -EFAULT;

    return sizeof(frame);
}

static __poll_t spc_poll(struct file *file, poll_table *wait)
{
    struct spc_dev *dev = g_spc;
    __poll_t mask = 0;

    poll_wait(file, &dev->read_queue, wait);

    if (!kfifo_is_empty(&dev->fifo))
        mask |= EPOLLIN | EPOLLRDNORM;

    return mask;
}

static int spc_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long pfn = DEVICE_BASE >> PAGE_SHIFT;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > DEVICE_SIZE)
        return -EINVAL;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot))
        return -EAGAIN;

    return 0;
}

static const struct file_operations spc_fops = {
    .owner   = THIS_MODULE,
    .read    = spc_read,
    .poll    = spc_poll,
    .mmap    = spc_mmap,
    .llseek  = noop_llseek,
};

static int spc_probe(struct platform_device *pdev)
{
    struct spc_dev *dev;
    int ret;

    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->base = devm_ioremap(&pdev->dev, DEVICE_BASE, DEVICE_SIZE);
    if (!dev->base)
        return -ENOMEM;

    dev->irq = platform_get_irq(pdev, 0);
    if (dev->irq < 0)
        return dev->irq;

    INIT_KFIFO(dev->fifo);
    init_waitqueue_head(&dev->read_queue);
    spin_lock_init(&dev->fifo_lock);
    dev->epoch_counter = 0;

    dev->misc.minor = MISC_DYNAMIC_MINOR;
    dev->misc.name  = DRIVER_NAME;
    dev->misc.fops  = &spc_fops;

    ret = misc_register(&dev->misc);
    if (ret)
        return ret;

    ret = devm_request_irq(&pdev->dev, dev->irq, spc_isr, 0, DRIVER_NAME, dev);
    if (ret) {
        misc_deregister(&dev->misc);
        return ret;
    }

    g_spc = dev;
    dev_info(&pdev->dev, "SPC harvester bound to IRQ %d, /dev/%s ready\n", dev->irq, DRIVER_NAME);
    return 0;
}

// Void return type for modern platform drivers
static void spc_remove(struct platform_device *pdev)
{
    struct spc_dev *dev = g_spc;
    if (dev)
        misc_deregister(&dev->misc);
}

static const struct of_device_id spc_of_match[] = {
    { .compatible = "microchip,spc" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spc_of_match);

static struct platform_driver spc_driver = {
    .probe  = spc_probe,
    .remove = spc_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = spc_of_match,
    },
};

module_platform_driver(spc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("W. Kyle Gilbertson");
MODULE_DESCRIPTION("SPC 1ms Synchronous Correlator Top-Half Harvester");