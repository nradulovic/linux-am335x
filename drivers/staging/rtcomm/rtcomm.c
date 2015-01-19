/*
 * Real-time communication driver
 *
 * Author: Nenad Radulovic <nenad.b.radulovic@gmail.com>
 *
 * Licensed under GPL-v2
 */

#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

#define RTCOMM_NAME                     "RTCOMM"
#define g_log_level                     5

#define RTCOMM_ERR(msg, ...)                                           \
        do {                                                            \
                if (g_log_level > 0) {                                  \
                        printk(KERN_ERR RTCOMM_NAME " error: " msg,    \
                                ## __VA_ARGS__);                        \
                }                                                       \
        } while (0)

#define RTCOMM_INF(msg, ...)                                           \
        do {                                                            \
                if (g_log_level > 3) {                                  \
                        printk(KERN_INFO RTCOMM_NAME " info: " msg,    \
                                ## __VA_ARGS__);                        \
                }                                                       \
        } while (0)

#define RTCOMM_NOT(msg, ...)                                           \
        do {                                                            \
                if (g_log_level > 2) {                                  \
                        printk(KERN_NOTICE  RTCOMM_NAME ": " msg,      \
                                ## __VA_ARGS__);                        \
                }                                                       \
        } while (0)

#define RTCOMM_WRN(msg, ...)                                           \
        do {                                                            \
                if (g_log_level > 1) {                                  \
                        printk(KERN_WARNING RTCOMM_NAME " warning: " msg,\
                                ## __VA_ARGS__);                        \
                }                                                       \
        } while (0)

#define RTCOMM_DBG(msg, ...)                                           \
        do {                                                            \
                if (g_log_level > 4) {                                  \
                        printk(KERN_DEFAULT RTCOMM_NAME " debug: " msg,\
                                ## __VA_ARGS__);                        \
                }                                                       \
        } while (0)
        
struct rtcomm_state 
{
        struct spi_transfer     spi_transfer;
        struct spi_message      spi_message;
        bool                    is_spi_locked;
        bool                    is_isr_init;
        bool                    is_gpio_init;
        bool                    is_busy;
};

struct acqunity_sample
{
    uint32_t                    channel[3];
    uint32_t                    timestamp;
};

struct buff
{
    struct acqunity_sample *    storage;
}

struct ppbuff
{
    wait_queue_head_t           wait;
    struct buff                 buffer[2];
    struct buff *               consumer;
    struct buff *               producer;
    uint32_t                    size;
    uint32_t                    lock_count;
}


static int rtcomm_open(struct inode * inode, struct file * fd);
static int rtcomm_release(struct inode * inode, struct file * fd);
static long rtcomm_ioctl(struct file * fd, unsigned int , unsigned long);
static ssize_t rtcomm_read(struct file * fd, char __user *, size_t, loff_t *);

static int ppbuff_init(struct ppbuff * ppbuff, uint32_t size)
{
        RTCOMM_DBG("init PPBUFF: %p, size: %d\n", ppbuff, size);
        ppbuff->consumer = &ppbuff->buffer[0];
        ppbuff->producer = &ppbuff->buffer[1];
        ppbuff->size     = size;
        ppbuff->lock_count = 0;
        
        RTCOMM_DBG("init PPBUFF consumer: %p\n", ppbuff->consumer);
        RTCOMM_DBG("init PPBUFF producer: %p\n", ppbuff->producer);
        ppbuff->consumer->storage = kzalloc(
                sizeof(struct acqunity_sample) * size, GFP_KERNEL);
        
        RTCOMM_DBG("init PPBUFF consumer storage: %p\n", ppbuff->consumer->storage);
        
        if (ppbuff->consumer->storage == NULL) {
                return (-ENOMEM);
        }
        ppbuff->producer->storage = kzalloc(
                sizeof(struct acqunity_sample) * size, GFP_KERNEL);
        
        RTCOMM_DBG("init PPBUFF producer storage: %p\n", ppbuff->producer->storage);
        
        if (ppbuff->consumer->storage == NULL) {
                return (-ENOMEM);
        }
        init_waitqueue_head((&ppbuff->wait);
        
        return (0);
}

static void ppbuff_term(struct ppbuff * ppbuff)
{
        RTCOMM_DBG("term PPBUFF: %p\n", ppbuff);
        kfree(ppbuff->consumer->storage);
        kfree(ppbuff->producer->storage);
}

static void ppbuff_swap(struct ppbuff * ppbuff)
{
        struct buff *           tmp;
        
        RTCOMM_DBG("swap PPBUFF: %p\n", ppbuff);

        ppbuff->lock_count++;

        tmp = ppbuff->consumer;
        ppbuff->consumer = ppbuff->producer;
        ppbuff->producer = tmp;
        wake_up_interruptible(&ppbuff->wait);
}

static void * ppbuff_get_producer_storage(struct ppbuff * ppbuff)
{
        if (!ppbuff->lock_count) {
                return (ppbuff->producer->storage);
        } else {
                return (NULL);
        }
}

static void * ppbuff_get_consumer_storage(struct ppbuff * ppbuff)
{
        wait_event_interruptible(ppbuff->wait, ppbuff->lock_count != 0);
        ppbuff->lock_count--;
        
        return (ppbuff->consumer->storage);
}

static uint32_t ppbuff_size(struct ppbuff * ppbuff)
{
        return (sizeof(struct acqunity_sample) * ppbuff->size);
}



static void transfer_done(void * arg)
{
        ppbuff_swap(&g_ppbuff);
}

static irqreturn_t trigger_notify_handler(int irq, void * p)
{
        unsigned long           flags;
        unsigned int            chip_id_no;
        void *                  storage;

        disable_irq_nosync(irq);
        storage = ppbuff_get_producer_storage(&g_ppbuff);
        
        if (storage) {
                memset(&g_rtcomm_state.spi_transfer, 0, sizeof(g_rtcomm_state.spi_transfer));
                g_rtcomm_state.spi_transfer.rx_buf = storage;
                g_rtcomm_state.spi_transfer.len    = ppbuff_size(&g_ppbuff);
                
                spi_message_init(&g_rtcomm_state.spi_message);
                spi_message_add_tail(&g_rtcomm_state.spi_transfer, &g_rtcomm_state.spi_message);
                g_rtcomm_state.spi_message.complete = transfer_done;
                g_rtcomm_state.spi_message.context  = NULL;
                spi_async_locked(&g_spi, &g_rtcomm_state.spi_message);
        }
        enable_irq(irq);

        return (IRQ_HANDLED);
}

/*--  Module parameters  ----------------------------------------------------*/
static int g_bus_id = 0;
module_param(g_bus_id, int, S_IRUGO);
MODULE_PARM_DESC(g_bus_is, "SPI bus ID");

static int g_notify;
module_param(g_notify, int, S_IRUGO);
MODULE_PARM_DESC(g_notify, "real-time notification GPIO pin");

static int g_buff_size = 1024;
module_param(g_buff_size, int, S_IRUGO);
MODULE_PARM_DESC(g_notify, "real-time buffer size");


static const struct file_operations g_rtcomm_fops = {
        .owner          = THIS_MODULE,
        .open           = rtcomm_open,
        .release        = rtcomm_release,
        .read           = rtcomm_read,
};

static struct miscdevice        g_rtcomm_miscdev = {
        MISC_DYNAMIC_MINOR, 
        RTCOMM_NAME,
        &g_rtcomm_fops
};

static struct rtcomm_state      g_rtcomm_state;
static struct spi_device *      g_spi;
static struct ppbuff            g_ppbuff;

static int rtcomm_open(struct inode * inode, struct file * fd)
{
        char                    label[64];
        int                     ret;
        
        RTCOMM_NOT("open(): %d:%d\n", current->group_leader->pid, current->pid);

        if (g_rtcomm_state.is_busy) {
                return (-EBUSY);
        }
        g_rtcomm_state.is_busy = true;
        fd->private_data = &g_rtcomm_state;
        
        g_spi.bits_per_word = 8;
        g_spi.mode          = SPI_MODE_1;
        g_spi.max_speed_hz  = 10000000ul;
        ret = spi_setup(g_spi);
            
        if (ret) {
                RTCOMM_ERR("spi_setup() request failed: %d\n", ret);
                
                return (ret);
        }
        spi_bus_lock(g_spi->master);
        g_ads1256_state.is_spi_locked = true;
        ret = ppbuff_init(&g_ppbuff, g_buff_size);
        
        if (ret) {
                RTCOMM_ERR("ppbuff_init() init failed: %d\n", ret);
                spi_bus_unlock(g_spi->master);
                
                return (ret);
        }
        ret = gpio_to_irq(g_notify);
        
        if (ret < 0) {
                RTCOMM_ERR("NOTIFY gpio %d interrupt request mapping failed\n", g_notify);
                
                goto fail_gpio_isr_map_request;
        }
        sprintf(label, RTCOMM_NAME "-notify");
        ret = request_irq(
                        ret, 
                        &trigger_notify_handler, 
                        IRQF_TRIGGER_RISING, 
                        label, 
                        NULL);
                        
        if (ret) {
                RTCOMM_ERR("NOTIFY gpio %d interrupt request failed\n", g_notify);
                
                goto fail_gpio_isr_request;
        }
        g_rtcomm_state.is_isr_init = true;

        return (0);
fail_gpio_isr_request:        
fail_gpio_isr_map_request:
        ppbuff_term(&g_ppbuff);
        g_rtcomm_state.is_busy = false;
        spi_bus_unlock(g_spi->master);
        
        return (ret);
}



static int rtcomm_release(struct inode * inode, struct file * fd)
{
        RTCOMM_NOT("close(): %d:%d\n", current->group_leader->pid, 
                        current->pid);

        if (!g_rtcomm_state.is_busy) {
                return (-EINVAL);
        }
        disable_irq_nosync(gpio_to_irq(g_notify));
        free_irq(gpio_to_irq(g_notify), NULL);
        spi_bus_unlock(g_spi->master);
        ppbuff_term(&g_ppbuff);
        g_ads1256_state.is_spi_locked = false;
        g_rtcomm_state.is_isr_init    = false;
        g_rtcomm_state.is_busy        = false;

        return (0);
}



static ssize_t rtcomm_read(struct file * fd, char __user * buff, size_t count, 
                loff_t * off)
{
        struct rtcomm_state *   state = fd->private_data;
        ssize_t                 ret;
        void *                  storage;

        if (count > ppbuff_size(&g_ppbuff)) {
                count = ppbuff_size(&g_ppbuff);
        }
        storage = ppbuff_get_consumer_storage(g_ppbuff);
        
        if (!storage) {
            return (-ENOMEM);
        }
        
        if (copy_to_user(buff, storage, sizeof(struct acqunity_sample) * count)) {
            return (-EFAULT);
        }
        *off += count;
        
        return (count);
}



static int __init ads1256_init(void)
{
        char                    label[64];
        int                     ret;
        struct spi_master *     master;
        struct spi_board_info   rtcomm_device_info = {
                .modalias     = RTCOMM_NAME,
                .max_speed_hz = 10000000ul,
                .bus_num      = g_bus_id,
        };
        RTCOMM_NOT("registering RTCOMM device driver\n");
        RTCOMM_NOT("sizeof(struct acqunity_sample) = %d\n", 
                sizeof(struct acqunity_sample));

        master = spi_busnum_to_master(g_bus_id);

        if (!master) {
                RTCOMM_ERR("invalid SPI bus id: %d\n", g_bus_id);

                return (-ENODEV);
        }
        g_spi = spi_new_device(master, &rtcomm_device_info);

        if (!g_spi) {
                RTCOMM_ERR("could not create SPI device\n");

                return (-ENODEV);
        }
        sprintf(label, RTCOMM_NAME "-notify");
        ret = gpio_request_one(g_notify, GPIOF_DIR_IN, label);
        
        if (ret) {
                RTCOMM_ERR("NOTIFY gpio %d request failed\n", g_notify);

                goto fail_gpio_request;
        }
        g_rtcomm_state.is_isr_init   = false;
        g_rtcomm_state.is_spi_locked = false;
        g_rtcomm_state.is_gpio_init  = true;
        g_rtcomm_state.is_busy       = false;
        
        ret = misc_register(&g_rtcomm_miscdev);
        
        return (ret);
fail_gpio_request:
        spi_unregister_device(g_spi);

        return (ret);
}



static void __exit ads1256_exit(void)
{
        if (g_rtcomm_state.is_isr_init) {
                disable_irq_nosync(gpio_to_irq(g_notify));
                free_irq(gpio_to_irq(g_notify), NULL);
        }
        
        if (g_rtcomm_state.is_gpio_init) {
                gpio_free(g_notify);
        }
        
        if (g_spi) {
        
                if (g_ads1256_state.is_spi_locked) {
                        spi_bus_unlock(g_spi->master);
                }
                spi_unregister_device(g_spi);
        }
        misc_deregister(&g_rtcomm_miscdev);
}

module_init(ads1256_init);
module_exit(ads1256_exit);
MODULE_AUTHOR("Nenad Radulovic <nenad.b.radulovic@gmail.com>");
MODULE_DESCRIPTION("Real-time communication driver");
MODULE_LICENSE("GPL v2");

