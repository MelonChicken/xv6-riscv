#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       2000  // size of file system in blocks
#define MAXPATH      128   // maximum file path name
#define USERSTACK    1     // user stack pages

// TODO: Project 03 Implement 3 syscalls 
#define PROT_READ    0x1 // read permission
#define PROT_WRITE   0x2 // write permission

#define MMAPBASE     0x40000000L // mapped region base address

#define MAP_ANONYMOUS 0x1 // anonymous mapping
#define MAP_POPULATE 0x2 // file-backed mapping

#define MAXMAP       64 // max # of mapping
