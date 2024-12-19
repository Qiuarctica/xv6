#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char          *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char          *rx_bufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock         e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void e1000_init(uint32 *xregs) {
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0;  // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0;  // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_bufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64)tx_ring;
  if (sizeof(tx_ring) % 128 != 0) panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_bufs[i] = kalloc();
    if (!rx_bufs[i]) panic("e1000");
    rx_ring[i].addr = (uint64)rx_bufs[i];
  }
  regs[E1000_RDBAL] = (uint64)rx_ring;
  if (sizeof(rx_ring) % 128 != 0) panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA + 1] = 0x5634 | (1 << 31);
  // multicast table
  for (int i = 0; i < 4096 / 32; i++) regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |                  // enable
                     E1000_TCTL_PSP |                 // pad short packets
                     (0x10 << E1000_TCTL_CT_SHIFT) |  // collision stuff
                     (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8 << 10) | (6 << 20);      // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN |       // enable receiver
                     E1000_RCTL_BAM |      // enable broadcast
                     E1000_RCTL_SZ_2048 |  // 2048-byte rx buffers
                     E1000_RCTL_SECRC;     // strip CRC

  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0;  // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0;  // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7);  // RXDW -- Receiver Descriptor Write Back
}

int e1000_transmit(char *buf, int len) {
  //
  // Your code here.
  //
  // buf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after send completes.
  //

  // first read the tail of the ring
  acquire(&e1000_lock);
  uint32 tail = regs[E1000_TDT];
  tail = tail % TX_RING_SIZE;
  // check if the tail is overflowing
  if (tail < 0 || tail >= TX_RING_SIZE) panic("e1000_transmit");
  // check the status of the tail
  if (!(tx_ring[tail].status & E1000_TXD_STAT_DD)) {
    goto error;
  } else {
    // free the buffer
    if (tx_bufs[tail]) {
      kfree(tx_bufs[tail]);
    }
  }
  // fill the descriptor
  tx_bufs[tail] = buf;
  struct tx_desc *desc = &tx_ring[tail];
  desc->addr = (uint64)buf;
  desc->length = len;
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  desc->status = 0;

  // update the tail
  regs[E1000_TDT] = (tail + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;

error:
  if (holding(&e1000_lock)) release(&e1000_lock);
  return -1;
}

static void e1000_recv(void) {
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver a buf for each packet (using net_rx()).
  // get the tail of the ring
  acquire(&e1000_lock);
  int tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  while (rx_ring[tail].status & E1000_RXD_STAT_DD) {
    // get the buffer
    char *buf = rx_bufs[tail];
    // get the length
    int len = rx_ring[tail].length;
    // deliver the packet
    release(&e1000_lock);
    net_rx(buf, len);
    acquire(&e1000_lock);
    // allocate a new buffer for the descriptor
    rx_bufs[tail] = kalloc();
    if (!rx_bufs[tail]) panic("e1000");
    rx_ring[tail].addr = (uint64)rx_bufs[tail];
    // reset the descriptor
    rx_ring[tail].status = 0;
    // update the tail
    regs[E1000_RDT] = tail;
    tail = (tail + 1) % RX_RING_SIZE;
  }
  release(&e1000_lock);
}

void e1000_intr(void) {
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
