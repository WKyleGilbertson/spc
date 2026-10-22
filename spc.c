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

#define DRIVER_NAME "spc"
#define DEVICE_BASE 0x41600000
#define DEVICE_SIZE 0x1000
#define CLEAR_INTERRUPT_OFFSET 0x80C
#define CLEAR_INTERRUPT_VALUE 0x80000000

#define FIFO_FRAME_COUNT 64

struct spc_dev
{
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
    unsigned long flags;
    int ch;

    frame.system_epoch = dev->epoch_counter++;

    // Unrolled harvest across APB bus
    for (ch = 0; ch < SPC_NUM_CHANNELS; ch++)
    {
        void __iomem *ch_base = dev->base + (ch * CH_STRIDE);
        frame.ch[ch].in = (int32_t)ioread32(ch_base + OFF_IN);
        frame.ch[ch].qn = (int32_t)ioread32(ch_base + OFF_QN);
        frame.ch[ch].ie = (int32_t)ioread32(ch_base + OFF_IE);
        frame.ch[ch].qe = (int32_t)ioread32(ch_base + OFF_QE);
        frame.ch[ch].ip = (int32_t)ioread32(ch_base + OFF_IP);
        frame.ch[ch].qp = (int32_t)ioread32(ch_base + OFF_QP);
        frame.ch[ch].il = (int32_t)ioread32(ch_base + OFF_IL);
        frame.ch[ch].ql = (int32_t)ioread32(ch_base + OFF_QL);

uint32_t epoch_info = ioread32(ch_base + OFF_EPOCH_INFO);
        frame.ch[ch].epoch_idx = (uint16_t)(epoch_info & 0xFFFF);
        frame.ch[ch].epoch_cnt = (uint8_t)((epoch_info >> 16) & 0x03);
        frame.ch[ch].carrier_phase = ioread32(ch_base + OFF_CARR_PHASE);
        frame.ch[ch].data_ready = (uint8_t)((epoch_info >> 24) & 0xFF);
    }

    // Acknowledge interrupt in FPGA fabric
    iowrite32(CLEAR_INTERRUPT_VALUE, dev->base + CLEAR_INTERRUPT_OFFSET);

    // Push into kfifo with spinlock protection (overwrite oldest if full)
    spin_lock_irqsave(&dev->fifo_lock, flags);
    if (kfifo_is_full(&dev->fifo)) {
        struct spc_epoch_data dummy;
        int ret = kfifo_get(&dev->fifo, &dummy);
        (void)ret; // Silence warn_unused_result properly
    }
    kfifo_put(&dev->fifo, frame);
    spin_unlock_irqrestore(&dev->fifo_lock, flags);

    // Wake user-space daemon
    wake_up_interruptible(&dev->read_queue);

    return IRQ_HANDLED;
}

static long spc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct spc_dev *dev = g_spc;
    channel_config_t cfg;
    channel_nco_update_t upd;
    void __iomem *ch_base;

    switch (cmd)
    {
    case SPC_IOC_CONFIG_CH:
        if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
            return -EFAULT;
        ch_base = dev->base + (cfg.channel_idx * CH_STRIDE);

        iowrite32(((cfg.spacing & 0x3F) << 2) | (cfg.nms & 0x3), ch_base + 0x00);
        iowrite32(cfg.carr_freq, ch_base + 0x04);
        iowrite32(cfg.code_freq, ch_base + 0x08);
        iowrite32(((cfg.g2_seed & 0x3FF) << 20) |
                      ((cfg.g1_seed & 0x3FF) << 10) |
                      (cfg.code_phase & 0x3FF),
                  ch_base + 0x0C);
        return 0;

    case SPC_IOC_UPDATE_NCO:
        if (copy_from_user(&upd, (void __user *)arg, sizeof(upd)))
            return -EFAULT;
        ch_base = dev->base + (upd.channel_idx * CH_STRIDE);

        iowrite32(upd.carr_freq, ch_base + 0x04);
        iowrite32(upd.code_freq, ch_base + 0x08);
        return 0;

    default:
        return -ENOTTY;
    }
}

static int spc_open(struct inode *inode, struct file *file)
{
    struct spc_dev *dev = g_spc;
    unsigned long flags;

    spin_lock_irqsave(&dev->fifo_lock, flags);
    kfifo_reset(&dev->fifo);
    spin_unlock_irqrestore(&dev->fifo_lock, flags);

    return 0;
}

static ssize_t spc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct spc_dev *dev = g_spc;
    struct spc_epoch_data frame;
    unsigned long flags;
    int ret;

    if (count < sizeof(struct spc_epoch_data))
        return -EINVAL;

    ret = wait_event_interruptible(dev->read_queue, !kfifo_is_empty(&dev->fifo));
    if (ret)
        return ret;

    spin_lock_irqsave(&dev->fifo_lock, flags);
    ret = kfifo_get(&dev->fifo, &frame);
    spin_unlock_irqrestore(&dev->fifo_lock, flags);

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
    .owner = THIS_MODULE,
    .open = spc_open,
    .read = spc_read,
    .poll = spc_poll,
    .unlocked_ioctl = spc_ioctl,
    .mmap = spc_mmap,
    .llseek = noop_llseek,
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
    dev->misc.name = DRIVER_NAME;
    dev->misc.fops = &spc_fops;

    ret = misc_register(&dev->misc);
    if (ret)
        return ret;

    ret = devm_request_irq(&pdev->dev, dev->irq, spc_isr, 0, DRIVER_NAME, dev);
    if (ret)
    {
        misc_deregister(&dev->misc);
        return ret;
    }

    g_spc = dev;
    dev_info(&pdev->dev, "SPC harvester bound to IRQ %d, /dev/%s ready\n", dev->irq, DRIVER_NAME);
    return 0;
}

static void spc_remove(struct platform_device *pdev)
{
    struct spc_dev *dev = g_spc;
    if (dev)
        misc_deregister(&dev->misc);
}

static const struct of_device_id spc_of_match[] = {
    {.compatible = "microchip,spc"},
    {/* sentinel */}
};
MODULE_DEVICE_TABLE(of, spc_of_match);

static struct platform_driver spc_driver = {
    .probe = spc_probe,
    .remove = spc_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = spc_of_match,
    },
};

module_platform_driver(spc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("W. Kyle Gilbertson");
MODULE_DESCRIPTION("SPC 1ms Synchronous Correlator Top-Half Harvester");