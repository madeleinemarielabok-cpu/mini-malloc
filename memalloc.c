#include <unistd.h>
#include <string.h>
#include <pthread.h>
// Used by print_mem_list for diagnostic output.
#include <stdio.h>

// The placeholder makes the header at least 16 bytes; union alignment is
// determined by its members.
typedef char ALIGN[16];
// Metadata stored immediately before each user-visible allocation.
union header {
    struct{
        size_t size;
        unsigned is_free;
        union header *next;
    } s;
    // The union's size and alignment keep the following payload aligned.
    ALIGN stub;
};
typedef union header header_t;

// All blocks obtained from sbrk are tracked in one list in allocation order.
// head and tail make it possible to search the list and append new blocks.
header_t *head = NULL, *tail = NULL;
// Serializes allocator operations that access the shared block list.
pthread_mutex_t global_malloc_lock;

// Find the first reusable free block large enough for the requested payload.
header_t *get_free_block(size_t size)
{
    header_t *curr = head;
    while(curr){
        // Reuse the first free block whose recorded payload is large enough.
        if(curr->s.is_free && curr->s.size >= size) return curr;
        curr = curr->s.next;
    }
    return NULL;
}

void free(void *block)
{
    header_t *header, *tmp;
    // The program break marks the current end of the process's data segment.
    void *programbreak;

    if(!block) return;
    pthread_mutex_lock(&global_malloc_lock);
    // The header is stored directly before the pointer returned by malloc.
    header = (header_t*)block - 1;
    // sbrk(0) reports the current program break without changing it.
    programbreak = sbrk(0);
    /*
    When the payload end equals the current program break, this path removes
    the block from the list and attempts to return its storage to the OS.
    Otherwise, keep it in the list so a later malloc can reuse it.
    */
   if((char*)block + header->s.size == programbreak)
   {
    // Remove the terminal block from the list before shrinking the heap.
    if(head == tail)
    {
        head = tail = NULL;
    }
    else
    {
        tmp = head;
        while(tmp)
        {
            if(tmp->s.next == tail)
            {
                tmp->s.next = NULL;
                tail = tmp;
            }
            tmp = tmp->s.next;
        }
    }
    /*
    A negative sbrk increment moves the program break backward, releasing
    this block's payload and header storage to the OS.
    */
   sbrk(0 - header->s.size - sizeof(header_t));
   /*
   The lock only coordinates calls through this allocator. An unrelated
   caller of sbrk could change the program break between the check above
   and this decrement, so this does not make sbrk use globally thread-safe.
   */
   pthread_mutex_unlock(&global_malloc_lock);
   return;
   }
   // Non-terminal blocks remain in the list for reuse by a future malloc.
   header->s.is_free = 1;
   pthread_mutex_unlock(&global_malloc_lock);
}

void *malloc(size_t size)
{
    size_t total_size;
    void *block;
    header_t *header;
    if(!size) return NULL;
    pthread_mutex_lock(&global_malloc_lock);
    // Reuse a suitable block before asking the OS to extend the heap.
    header = get_free_block(size);
    if(header)
    {
        // Mark the reused block allocated before returning its payload.
        header->s.is_free = 0;
        pthread_mutex_unlock(&global_malloc_lock);
        return (void*)(header + 1);
    }
    // Request enough space for both allocator metadata and the payload.
    total_size = sizeof(header_t) + size;
    block = sbrk(total_size);
    if(block==(void*) -1)
    {
        pthread_mutex_unlock(&global_malloc_lock);
        return NULL;
    }
    // Initialize metadata, then append this new block to the allocation list.
    header = block;
    header->s.size = size;
    header->s.is_free = 0;
    header->s.next = NULL;
    if(!head) head = header;
    if(tail) tail->s.next = header;
    tail = header;
    pthread_mutex_unlock(&global_malloc_lock);
    return (void*)(header + 1);
}

void *calloc(size_t num, size_t nsize)
{
    size_t size;
    void *block;
    if(!num || !nsize) return NULL;
    // Check multiplication before using the combined size for allocation.
    size = num * nsize;
    if(nsize != size / num) return NULL;
    // malloc supplies the storage; calloc additionally zeroes every byte.
    block = malloc(size);
    if(!block) return NULL;
    memset(block,0,size);
    return block;
}

void *realloc(void *block,size_t size)
{
    header_t *header;
    void *ret;
    if(!block || !size) return malloc(size);
    // The existing allocation already fits, so its address can be retained.
    header = (header_t*)block - 1;
    if(header->s.size >= size) return block;
    ret = malloc(size);
    if(ret)
    {
        // Copy the old payload before releasing its allocation.
        memcpy(ret,block,header->s.size);
        free(block);
    }
    return ret;
}

// Print the allocator's block list and metadata for debugging.
void print_mem_list()
{
	header_t *curr = head;
	printf("head = %p, tail = %p \n", (void*)head, (void*)tail);
	while(curr) {
		printf("addr = %p, size = %zu, is_free=%u, next=%p\n",
			(void*)curr, curr->s.size, curr->s.is_free, (void*)curr->s.next);
		curr = curr->s.next;
	}
}