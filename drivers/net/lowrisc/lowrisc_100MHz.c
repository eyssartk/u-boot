/*
 * Lowrisc Ether100MHz Linux driver for the Lowrisc Ethernet 100MHz device.
 *
 * This is a u-boot port of the lowrisc Linux driver by Jonathan Richard
 * Robert Kimmitt <jonathan@kimmitt.uk>
 *
 * Copyright 2023 Thales SA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <common.h>
#include <log.h>
#include <net.h>
#include <config.h>
#include <dm.h>
#include <console.h>
#include <malloc.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <phy.h>
#include <miiphy.h>
#include <fdtdec.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <asm/io.h>
#include "lowrisc_100MHz.h"

#define DRIVER_AUTHOR	"Kevin Eyssartier <kevin.eyssartier@thalesgroup.com>"
#define DRIVER_DESC	"ethernet driver for low_risc IP"
#define DRIVER_NAME     "lowrisc-eth"

/* General Ethernet Definitions */
#define XEL_ARP_PACKET_SIZE		28	/* Max ARP packet size */
#define XEL_HEADER_IP_LENGTH_OFFSET	16	/* IP Length Offset */

#define TX_TIMEOUT		(60*HZ)		/* Tx timeout is 60 seconds. */

/**
 * struct net_local - Our private per device data
 * @ndev:		instance of the network device
 * @reset_lock:		lock used for synchronization
 * @phy_dev:		pointer to the PHY device
 * @phy_node:		pointer to the PHY device node
 * @mii_bus:		pointer to the MII bus
 * @last_link:		last link status
 */
struct net_local {
  void __iomem *ioaddr;
  u32 msg_enable;
  
  int phyaddr;
  struct phy_device *phy_dev;
  struct mii_dev *mii_bus;
  int last_duplex;
  int last_carrier;
  
  /* Spinlock */
  uint32_t last_mdio_gpio;
  int irq, num_tests, phy_addr;

};

static void inline eth_write(struct net_local *priv, size_t addr, int data)
{
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  eth_base[addr >> 3] = data;
}

static void inline eth_copyout(struct net_local *priv, uint8_t *data, int len)
{
  int i, rnd = ((len-1)|7)+1;
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (!(((size_t)data) & 7))
    {
      uint64_t *ptr = (uint64_t *)data;
      for (i = 0; i < rnd/8; i++)
        eth_base[TXBUFF_OFFSET/8 + i] = ptr[i];
    }
  else // We can't unfortunately rely on the skb being word aligned
    {
      uint64_t notptr;
      for (i = 0; i < rnd/8; i++)
        {
          memcpy(&notptr, data+(i<<3), sizeof(uint64_t));
          eth_base[TXBUFF_OFFSET/8 + i] = notptr;
        }
    }
}

static volatile inline int eth_read(struct net_local *priv, size_t addr)
{
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  return eth_base[addr >> 3];
}

static inline void eth_copyin(struct net_local *priv, uint8_t *data, int len, int start)
{
  int i, rnd = ((len-1)|7)+1;
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (!(((size_t)data) & 7))
    {
      uint64_t *ptr = (uint64_t *)data;
      for (i = 0; i < rnd/8; i++)
        ptr[i] = eth_base[start + i];
    }
  else // We can't unfortunately rely on the skb being word aligned
    {
      for (i = 0; i < rnd/8; i++)
        {
          uint64_t notptr = eth_base[start + i];
          memcpy(data+(i<<3), &notptr, sizeof(uint64_t));
        }
    }
}

static int lowrisc_eth_miiphy_read(struct mii_dev *bus, int addr,
				int devad, int reg)
{
	u32 ret;
	u16 val = 0;

	val = phy_read(bus->phymap[addr], devad, reg);
	debug("lowrisc_eth: Read MII 0x%x, 0x%x, 0x%x\n", addr, reg, val);
	return val;
}

static int lowrisc_eth_miiphy_write(struct mii_dev *bus, int addr, int devad,
				 int reg, u16 value)
{
	debug("lowrisc_eth: Write MII 0x%x, 0x%x, 0x%x\n", addr, reg, value);
	return phy_write(bus->phymap[addr], devad, reg, value);
}

/**
 * lowrisc_update_address - Update the MAC address in the device
 * @drvdata:	Pointer to the Ether100MHz device private data
 * @address_ptr:Pointer to the MAC address (MAC address is a 48-bit value)
 *
 * Tx must be idle and Rx should be idle for deterministic results.
 * It is recommended that this function should be called after the
 * initialization and before transmission of any packets from the device.
 * The MAC address can be programmed using any of the two transmit
 * buffers (if configured).
 */

static void lowrisc_update_address(struct net_local *priv, unsigned char *address_ptr)
{
  uint32_t macaddr_lo, macaddr_hi;
  memcpy (&macaddr_lo, address_ptr+2, sizeof(uint32_t));
  memcpy (&macaddr_hi, address_ptr+0, sizeof(uint16_t));
  eth_write(priv, MACLO_OFFSET, htonl(macaddr_lo));
  eth_write(priv, MACHI_OFFSET, htons(macaddr_hi));
  eth_write(priv, RFCS_OFFSET, 8); /* use 8 buffers */
}

/**
 * lowrisc_read_mac_address - Read the MAC address in the device
 * @drvdata:	Pointer to the Ether100MHz device private data
 * @address_ptr:Pointer to the 6-byte buffer to receive the MAC address (MAC address is a 48-bit value)
 *
 * In lowrisc the starting value is programmed by the boot loader according to DIP switch [15:12]
 */

static void lowrisc_read_mac_address(struct net_local *priv, u8 *address_ptr)
{
  uint32_t macaddr_hi, macaddr_lo;
  macaddr_hi = ntohs(eth_read(priv, MACHI_OFFSET)&MACHI_MACADDR_MASK);
  macaddr_lo = ntohl(eth_read(priv, MACLO_OFFSET));
  memcpy (address_ptr+2, &macaddr_lo, sizeof(uint32_t));
  memcpy (address_ptr+0, &macaddr_hi, sizeof(uint16_t));
}


/**
 * lowrisc_close - Close the network device
 * @dev:	Pointer to the network device
 *
 * This function stops the Tx queue, disables interrupts and frees the IRQ for
 * the Ether100MHz device.
 * It also disconnects the phy device associated with the Ether100MHz device.
 */
static void lowrisc_close(struct udevice *ndev)
{
	struct net_local *priv = dev_get_priv(ndev);
}




/**
 * lowrisc_open - Open the network device
 * @dev:	Pointer to the network device
 *
 * This function sets the MAC address, requests an IRQ and enables interrupts
 * for the Ether100MHz device and starts the Tx queue.
 * It also connects to the phy device, if MDIO is included in Ether100MHz device.
 */

static int lowrisc_open(struct udevice *ndev)
{
  int retval = 0;
  struct net_local *priv = dev_get_priv(ndev);
  // ndev->ethtool_ops = &lowrisc_ethtool_ops;

  /* Set the MAC address each time opened */
  lowrisc_update_address(priv, net_ethaddr);
  
  if (priv->phy_dev) {
    // linkmode_copy(priv->phy_dev->advertising, priv->phy_dev->supported);
    // call phy driver
    phy_startup(priv->phy_dev);
  }
  
  lowrisc_update_address(priv, net_ethaddr);

  return 0;
}

/**
 * lowrisc_send - Transmit a frame
 * @orig_skb:	Pointer to the socket buffer to be transmitted
 * @dev:	Pointer to the network device
 *
 * This function checks if the Tx buffer of the Ether100MHz device is free to send
 * data. If so, it fills the Tx buffer with data from socket buffer data,
 * updates the stats and frees the socket buffer.
 * Return:	0, always.
 */
static int lowrisc_send(struct udevice *ndev, void *ptr, int len)
{
    struct net_local *priv = dev_get_priv(ndev);
    int rslt;
    rslt = eth_read(priv, TPLR_OFFSET);
    if (rslt & TPLR_BUSY_MASK)
        return -1; // NETDEV_TX_BUSY
    eth_copyout(priv, (uint8_t *)(ptr), len);
    eth_write(priv, TPLR_OFFSET, len);

	return 0; // NETDEV_TX_OK
}

uint8_t __attribute__ ((aligned (8))) framebuffer[ETH_FRAME_LEN + ETH_FCS_LEN];
static int lowrisc_recv(struct udevice *dev, int flags, uchar **packetp)
{
    int rsr, len=0, buf;
    struct net_local *priv = dev_get_priv(dev);
    rsr = eth_read(priv, RSR_OFFSET);
    buf = rsr & RSR_RECV_FIRST_MASK;
    /* Check if there is Rx Data available */
    if (rsr & RSR_RECV_DONE_MASK)
    {
        len = eth_read(priv, RPLR_OFFSET+((buf&7)<<3)) - 4; /* discard FCS bytes ?? */
        if ((len >= 60) && (len <= ETH_FRAME_LEN + ETH_FCS_LEN))
        {
            int start = RXBUFF_OFFSET/8 + ((buf&7)<<8);
            eth_copyin(priv, framebuffer, len, start);
        }

        /* acknowledge, even if an error occurs */
        eth_write(priv, RSR_OFFSET, ++buf);
        *packetp = framebuffer;
    }

    return len;
}

static const struct eth_ops lowrisc_eth_ops = {
	.start = lowrisc_open,
	.send = lowrisc_send,
	.recv = lowrisc_recv,
	.stop = lowrisc_close,
};

/**
 * lowrisc_of_probe - Probe method for the Ether100MHz device.
 * @ofdev:	Pointer to OF device structure
 * @match:	Pointer to the structure used for matching a device
 *
 * This function probes for the Ether100MHz device in the device tree.
 * It initializes the driver data structure and the hardware, sets the MAC
 * address and registers the network device.
 * It also registers a mii_bus for the Ether100MHz device, if MDIO is included
 * in the device.
 *
 * Return:	0, if the driver is bound to the Ether100MHz device, or
 *		a negative error if there is failure.
 */
static int lowrisc_100MHz_probe(struct udevice *dev)
{
	struct net_local *priv = dev_get_priv(dev);
	int rc = 0;

	priv->mii_bus = mdio_alloc();
	priv->mii_bus->read = lowrisc_eth_miiphy_read;
	priv->mii_bus->write = lowrisc_eth_miiphy_write;
	priv->mii_bus->priv = priv;

	rc = mdio_register_seq(priv->mii_bus, dev_seq(dev));
	if (rc)
		return rc;

	return 0;
}

int lowrisc_100MHz_unregister(struct udevice *dev)
{
        struct net_local *priv = dev_get_priv(dev);

        if (priv->phy_dev) {
            // call phy driver
            phy_shutdown(priv->phy_dev);
        }

        free(priv->phy_dev);
        mdio_unregister(priv->mii_bus);
        mdio_free(priv->mii_bus);
        return 0;
}

static int lowrisc_eth_of_to_plat(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct net_local *emaclite = dev_get_priv(dev);
	int offset = 0;

	pdata->iobase = dev_read_addr(dev);
	emaclite->ioaddr = (void *)pdata->iobase;

	emaclite->phyaddr = -1;

	offset = fdtdec_lookup_phandle(gd->fdt_blob, dev_of_offset(dev),
				      "phy-handle");
	if (offset > 0)
		emaclite->phyaddr = fdtdec_get_int(gd->fdt_blob, offset,
						   "reg", -1);


	printf("LOWRISC-ETH: %lx, phyaddr %d\n", (ulong)emaclite->ioaddr,
	       emaclite->phyaddr);

	return 0;
}

static const struct udevice_id lowrisc_eth_ids[] = {
	{ .compatible = DRIVER_NAME },
	{ }
};

U_BOOT_DRIVER(lowrisc_eth) = {
	.name   = DRIVER_NAME,
	.id     = UCLASS_ETH,
	.of_match = lowrisc_eth_ids,
	.of_to_plat = lowrisc_eth_of_to_plat,
	.probe  = lowrisc_100MHz_probe,
	.remove = lowrisc_100MHz_unregister,
	.ops    = &lowrisc_eth_ops,
	.priv_auto	= sizeof(struct net_local),
	.plat_auto	= sizeof(struct eth_pdata),
};
