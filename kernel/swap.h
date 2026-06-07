// we will use the disk area as a swap area from the disk whose block number is 2000  
#define SWAPBASE 2000
// the number of disk blocks for swapping
#define SWAPMAX (FSSIZE - SWAPBASE)
// how many disk blocks are needed for a page?
#define BLOCKPERPAGE (PGSIZE / BSIZE)
// the number of total swap slots(how many pages can we save?)
#define NSWAPSLOT (SWAPMAX / BLOCKPERPAGE)
// swap will be saved in root disk device
#define SWAPDEV ROOTDEV
