#include <linux/netdevice.h>
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
#include <linux/timekeeping.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>

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
    atomic_t open_count;
    struct delayed_work tsu_disarm_work;
    void __iomem *gem0;
    u16 last_pps_cnt;
    u16 latched_pps_cnt;
    u16 processed_pps_cnt;
    u32 latched_tsu_sec;
    u32 latched_tsu_nsec;
    s32 anchor_nsec;
    s32 prev_err_ns;
    bool has_prev_err;
    bool gem0_50mhz_done;
    s64 freq_int_subns_q8;
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

    // Check if 1PPS cycle_cnt (0x80C[15:0]) incremented on this 1ms sclk boundary
    if (dev->gem0) {
        u16 pps_cnt = (u16)(ioread32(dev->base + CLEAR_INTERRUPT_OFFSET) & 0xFFFFU);
        if (pps_cnt != dev->last_pps_cnt) {
            dev->latched_tsu_sec  = ioread32(dev->gem0 + 0x1D0);
            dev->latched_tsu_nsec = ioread32(dev->gem0 + 0x1D4);
            dev->last_pps_cnt     = pps_cnt;
            dev->latched_pps_cnt  = pps_cnt;
        }
    }

    // Acknowledge interrupt in FPGA fabric
    iowrite32(CLEAR_INTERRUPT_VALUE, dev->base + CLEAR_INTERRUPT_OFFSET);

    // Push into kfifo with spinlock protection (overwrite oldest if full)
    spin_lock_irqsave(&dev->fifo_lock, flags);
    if (kfifo_is_full(&dev->fifo))
    {
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

static void spc_tsu_disarm_fn(struct work_struct *work)
{
    struct spc_dev *dev = container_of(to_delayed_work(work), struct spc_dev, tsu_disarm_work);
    if (dev && dev->base) {
        iowrite32(0x2, dev->base + 0x818);
        dev_info(dev->misc.this_device, "GEM0 TSU hardware 1PPS jam-sync disarmed (free-run holdover)\n");
    }
}

#define NOMINAL_50_256MHZ_SUBNS (0LL) /* Keep TN slightly below 1e9 so only HW 1PPS pulse increments TSL */

#define HW_SYNC_SUBNS (193500U)
#define HW_SYNC_TISUBN (((HW_SYNC_SUBNS >> 8) & 0xFFFFU) | ((HW_SYNC_SUBNS & 0xFFU) << 24))

static void spc_program_gem0_incr(void __iomem *gem0, s64 total_subns)
{
    iowrite32(HW_SYNC_TISUBN, gem0 + 0x1BC);
    iowrite32(20, gem0 + 0x1DC);
}

static void spc_ensure_gem0_50mhz(struct spc_dev *dev)
{
    struct net_device *ndev;

    if (dev->gem0_50mhz_done)
        return;

    ndev = dev_get_by_name(&init_net, "eth0");
    if (!ndev)
        return;

    {
        u32 *priv = (u32 *)netdev_priv(ndev);
        int i;
        for (i = 0; i < 4096; i++) {
            if (priv[i] == 150000000U || priv[i] == 50000000U)
                priv[i] = 50000000U;
            if ((priv[i] == 11184810U && priv[i + 1] == 6U) || (priv[i] == 0U && priv[i + 1] == 20U)) {
                priv[i]     = 0U;
                priv[i + 1] = 20U;
                break;
            }
        }
        dev_put(ndev);
    }
    if (dev->gem0)
        spc_program_gem0_incr(dev->gem0, 20LL << 24);
    dev->gem0_50mhz_done = true;
}

static void spc_step_tsu_servo(struct spc_dev *dev)
{
    u16 pps_cnt;
    u32 sec, nsec;
    time64_t tai_sec;

    if (!dev || !dev->gem0)
        return;

    // Enforce 20 ns increment and keep FPGA hardware 1PPS sync (0x818 bit 1) armed
    iowrite32(HW_SYNC_TISUBN, dev->gem0 + 0x1BC);
    iowrite32(20, dev->gem0 + 0x1DC);
    iowrite32(0x2, dev->base + 0x818);

    pps_cnt = dev->latched_pps_cnt;
    if (pps_cnt == dev->processed_pps_cnt)
        return;
    dev->processed_pps_cnt = pps_cnt;

    sec  = dev->latched_tsu_sec;
    nsec = dev->latched_tsu_nsec;
    tai_sec = (time64_t)((ktime_get_real_ns() + 500000000ULL) / 1000000000ULL) + 37;

    /*
    if (sec != (u32)tai_sec) {
        iowrite32((u32)tai_sec, dev->gem0 + 0x1D0);
        sec = (u32)tai_sec;
    } */

    if ((sec % 60U) == 0U)
        dev_info(dev->misc.this_device, "[PPS-TSU] HW-SYNC locked: sec=%u lat=%u ns\n", sec, nsec);
}

static long spc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct spc_dev *dev = g_spc;
    channel_config_t cfg;
    channel_nco_update_t upd;
    pps_config_t pps;
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

     case SPC_IOC_STEER_PPS:
            if (copy_from_user(&pps, (void __user *)arg, sizeof(pps)))
                return -EFAULT;
            // 1. Steer 1ms epoch threshold in spc_timebase (0x800) if in 1ms range
            if (pps.period_ticks >= 16360 && pps.period_ticks <= 16376) {
                iowrite32(pps.period_ticks, dev->base + 0x800);
            }
            // 2. Steer 24-bit 1PPS generator in spc.v (0x810..0x81C) if in 1s range
            if (pps.period_ticks >= 16360000 && pps.period_ticks <= 16376000) {
                iowrite32(pps.period_ticks & 0xFFFFFF, dev->base + 0x810);
                if (pps.pulse_width > 0) {
                    iowrite32(pps.pulse_width & 0xFFFFFF, dev->base + 0x81C);
                }
                spc_ensure_gem0_50mhz(dev);
                if (pps.trigger_jam) {
                    iowrite32(pps.jam_target_val & 0xFFFFFF, dev->base + 0x814);
                    iowrite32(0x3, dev->base + 0x818); // Bit 0: jam 1PPS, Bit 1: arm 1-shot GEM0 TSU clear
                    mod_delayed_work(system_wq, &dev->tsu_disarm_work, msecs_to_jiffies(1100));
                    dev->has_prev_err = false;
                } else {
                    spc_step_tsu_servo(dev);
                }
            } else if (pps.trigger_jam) {
                dev->epoch_counter = pps.jam_target_val;
            }
            return 0;

     case SPC_IOC_RESET_TIMEBASE:
            dev->epoch_counter = 0;
            return 0;

     case SPC_IOC_SET_SYS_TIME: {
            spc_sys_time_t st;
            struct timespec64 ts64;
            if (copy_from_user(&st, (void __user *)arg, sizeof(st)))
                return -EFAULT;
            ts64.tv_sec  = st.tv_sec;
            ts64.tv_nsec = st.tv_nsec;
            return do_settimeofday64(&ts64);
     }   

    default:
        return -ENOTTY;
    }
}

static int spc_open(struct inode *inode, struct file *file)
{
    struct spc_dev *dev = g_spc;
    unsigned long flags;

    // Only reset the kfifo when the first reader (l1iftrkd) opens /dev/spc
    if (atomic_inc_return(&dev->open_count) == 1) {
        spin_lock_irqsave(&dev->fifo_lock, flags);
        kfifo_reset(&dev->fifo);
        spin_unlock_irqrestore(&dev->fifo_lock, flags);
    }

    return 0;
}

static int spc_release(struct inode *inode, struct file *file)
{
    struct spc_dev *dev = g_spc;
    atomic_dec(&dev->open_count);
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
    .release = spc_release,
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

    //dev_info(dev, "spc driver loaded (version: %s, built: %s)\n", GIT_REV, BUILD_TIME);
    {
        struct device_node *ov_np = of_find_node_by_path("/chosen/overlays");
        const char *gw_ver = "unknown";
        if (ov_np) {
            if (of_property_read_string(ov_np, "L1IF-CAPE-GATEWARE", &gw_ver) < 0) {
                if (of_property_read_string(ov_np, "DEFAULT-CAPE-GATEWARE", &gw_ver) < 0)
                    of_property_read_string(ov_np, "DEFAULT-BASE", &gw_ver);
            }
            of_node_put(ov_np);
        }
        dev_info(&pdev->dev, "spc driver loaded (drv_git: %s, gw_git: %s, built: %s)\n",
                 GIT_CI, gw_ver, BUILD_TIMESTAMP);
    }

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
    atomic_set(&dev->open_count, 0);
    INIT_DELAYED_WORK(&dev->tsu_disarm_work, spc_tsu_disarm_fn);

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

    // Set GEM0 TSU increment & macb->tsu_incr for 50 MHz FIC_3_PCLK (20 ns/tick)
    {
        struct net_device *ndev = dev_get_by_name(&init_net, "eth0");
        if (ndev) {
            u32 *priv = (u32 *)netdev_priv(ndev);
            int i;
            for (i = 0; i < 4096; i++) {
                if (priv[i] == 150000000U) {
                    priv[i] = 50000000U;
                    dev_info(&pdev->dev, "Updated eth0 macb->tsu_rate (offset +%d) to 50 MHz\n", i * 4);
                }
                // Match struct tsu_incr { u32 sub_ns = 11184810; u32 ns = 6; }
                if (priv[i] == 11184810U && priv[i + 1] == 6U) {
                    priv[i]     = 0U;  // sub_ns = 0
                    priv[i + 1] = 20U; // ns = 20 (50 MHz)
                    dev_info(&pdev->dev, "Updated eth0 macb->tsu_incr (offset +%d) to 20 ns\n", i * 4);
                    break;
                }
            }
            dev_put(ndev);
        }
        void __iomem *gem0 = ioremap(0x20110000, 0x1000);
        if (gem0) {
            iowrite32(0, gem0 + 0x1BC);  // TSU_TIMER_INCR_SUB_NSEC = 0
            iowrite32(20, gem0 + 0x1DC); // TSU_TIMER_INCR = 20 ns (50 MHz)
            iounmap(gem0);
        }
    }
    dev->gem0 = ioremap(0x20110000, 0x1000);
    g_spc = dev;
    dev_info(&pdev->dev, "SPC harvester bound to IRQ %d, /dev/%s ready\n", dev->irq, DRIVER_NAME);
    return 0;
}

static void spc_remove(struct platform_device *pdev)
{
    struct spc_dev *dev = g_spc;
    if (dev) {
        cancel_delayed_work_sync(&dev->tsu_disarm_work);
        if (dev->gem0)
            iounmap(dev->gem0);
        misc_deregister(&dev->misc);
    }
}

static const struct of_device_id spc_of_match[] = {
    {.compatible = "microchip,spc"},
    {/* sentinel */}};
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


#ifndef GIT_CI
#define GIT_CI "unknown"
#endif
#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP "unknown"
#endif

MODULE_VERSION(GIT_CI);
MODULE_INFO(git_rev, GIT_CI);
MODULE_INFO(build_time, BUILD_TIMESTAMP);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("W. Kyle Gilbertson");
MODULE_DESCRIPTION("SPC 1ms Synchronous Correlator Top-Half Harvester");