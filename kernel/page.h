struct page {
    pagetable_t pagetable;
    uint64 vaddr;
    uchar age;
    int used;
};
