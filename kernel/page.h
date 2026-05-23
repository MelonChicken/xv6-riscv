struct page {
    //struct page *next;
    //struct page *prev;
    pagetable_t pagetable;
    char *vaddr;
    uchar age;
    int used;
};
