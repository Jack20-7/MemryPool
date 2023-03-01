#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>


#define MP_ALIGNMENT                 32
#define MP_PAGE_SIZE                 4096
#define MP_MAX_ALLOC_FROM_POOL       (MP_PAGE_SIZE - 1)

//对齐的操作
#define mp_align(n,alingment)        (((n) + (alingment - 1)) & ~(alingment - 1))
#define mp_align_ptr(p,alingment)    (void*)((((size_t)p) + (alingment - 1)) & ~(alingment - 1))

//该内存池采用的模式是不单独归还内存，而是整个内存池一起归还(分配的大内存可以单独归还)

//大块内存
struct mp_large_s{
    struct mp_large_s* next;
    void* alloc;
};

//小块内存
struct mp_node_s{
    unsigned char* last;    //指向已经使用了的内存的尾地址
    unsigned char* end;     //指向分配内存的尾地址

    struct mp_node_s* next;
    size_t failed;          //分配失败的次数，用来处理分配的每一块page最后的那一小块没有被分配出去的内存块
};


//内存池
struct mp_pool_s{
    size_t max;                   //表示大块内存和小块内存的分界线

    struct mp_large_s* large;     //指向分配的大块内存对应的链表
    struct mp_node_s*  current;   //当前处于哪一个大内存块的起始node

    struct mp_node_s   head[0];     //小块内存的头指针.零长数组
};

//内存池提供的API
struct mp_pool_s*   mp_create_pool(size_t size);
void                mp_destroy_pool(struct mp_pool_s* pool);
void*               mp_alloc(struct mp_pool_s* pool,size_t size);
void*               mp_nalloc(struct mp_pool_s* pool,size_t size);
void*               mp_calloc(struct mp_pool_s* pool,size_t size);
void                mp_free(struct mp_pool_s* pool);


//分配出来的结构是这样的
/*
       | ------mp_pool_s-----|--------mp_node_s------|------------size---------------|
                             head                     last                           end
                             current 
                             
*/
struct mp_pool_s*   mp_create_pool(size_t size){
    struct mp_pool_s* pool;
    //这里使用的是posix_memalign来分配堆区内存，而不是malloc.因为涉及到4k及以上的堆区内存分配我们一般使用posix_memalign来进行分配
    int ret = posix_memalign((void**)&pool,MP_ALIGNMENT,sizeof(struct mp_pool_s) + size + sizeof(struct mp_node_s));
    if(ret){
      printf("posix_memalig mp_create_pool failed\n");
      return NULL;
    }

    pool->max = size < MP_MAX_ALLOC_FROM_POOL ? size : MP_MAX_ALLOC_FROM_POOL;
    pool->current = pool->head;  //不是很明白，这个pool->head都没有初始化过，为什么就直接可以这样使用
    pool->large = NULL;

    pool->head->last = (unsigned char*)pool + sizeof(struct mp_pool_s) + sizeof(struct mp_node_s);
    pool->head->end = pool->head->last + size;

    pool->head->failed = 0;
}

void mp_destroy_pool(struct mp_pool_s* pool){
    struct mp_node_s * h ,*n;
    struct mp_large_s* l;


    //释放大内存链表
    for(l = pool->large;l;l = l->next){
        if(l->alloc){
            free(l->alloc);
        }
    }

    h = pool->head->next;
    while(h){
        n = h->next;
        free(h);
        h = n;
    }
    free(pool);
}

//重置内存池
void mp_reset_pool(struct mp_pool_s* pool){
    struct mp_node_s* h ;
    struct mp_large_s* l;
    //large内存全部释放掉
    for(l = pool->large;l;l = l->next){
        if(l->alloc){
            free(l->alloc);
        }
    }
    pool->large = NULL;

    //小内存每一块的管理node last重置到起始地址
    for(h = pool->head;h;h = h->next){
        h->failed = 0;
        h->last = (unsigned char*)h + sizeof(struct mp_node_s);
    }
}

//分配小块内存
static void* mp_alloc_block(struct mp_pool_s* pool,size_t size){
    unsigned char* m;
    struct mp_node_s* h  = pool->head;
    size_t psize = (size_t)(h->end  - (unsigned char*)h);
    int ret = posix_memalign((void**)&m,MP_ALIGNMENT,psize);
    if(ret){
        return NULL;
    }
    struct mp_node_s* p,*new_node,*current;
    new_node = (struct mp_node_s*)m;
    new_node->end = m + psize;
    new_node->next = NULL;
    new_node->failed = 0;

    m += sizeof(struct mp_node_s);
    m = (unsigned char*)mp_align_ptr(m,MP_ALIGNMENT);
    new_node->last = m + size;

    //尝试4次
    current = pool->current;
    for(p = current;p->next;p = p->next){
        if(p->failed++ > 4){
            //如果在当前的内存块分配失败四次以上，就将current指向下一个内存块
            current = p->next;
        }
    }
    p->next = new_node;
    pool->current = current ? current : new_node;

    return m;
}
//分配大块内存
static void* mp_alloc_large(struct mp_pool_s* pool,size_t size){
    void* p = malloc(size);
    if(NULL == p){
        return NULL;
    }


    size_t n = 0;
    struct mp_large_s* large = NULL;
    for(large = pool->large;large;large = large->next){
        if(large->alloc == NULL){
            large->alloc = p;
            return p;
        }
        if(n++ > 3){
            break;
        }
    }


    //大内存分配的控制体是从小内存那里分配来的
    large = (struct mp_large_s*)mp_alloc(pool,sizeof(struct mp_large_s));
    if(NULL == large){
        free(p);
        return NULL;
    }


    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}


void* mp_memalign(struct mp_pool_s* pool,size_t size,size_t alignment){
    void* p = NULL;
    int ret = posix_memalign((void**)&p,alignment,size);
    if(ret){
        return NULL;
    }


    struct mp_large_s* large = (struct mp_large_s*)mp_alloc(pool,sizeof(struct mp_large_s));
    if(NULL == large){
        free(p);
        return NULL;
    }


    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}



void* mp_alloc(struct mp_pool_s* pool,size_t size){
    unsigned char* m;
    struct mp_node_s* p;

    if(size <= pool->max){
        //分配的是小块内存
        //寻找一下能够直接从内存池中获取
        p = pool->current;
        do{
            m = (unsigned char*)mp_align_ptr(p->last,MP_ALIGNMENT);
            if((size_t)(p->end - m) >= size){
                //如果能够满足的话
                p->last = m + size;
                return m;
            }
            p = p->next;
        }while(p);
        //如果当前内存池中内存满足不了的话，就需要重新分配一大块内存
        return mp_alloc_block(pool,size);
    }
    //分配大块内存
    return mp_alloc_large(pool,size);
}

void* mp_nalloc(struct mp_pool_s* pool,size_t size){
    unsigned char* m = NULL;
    struct mp_node_s* p = NULL;

    if(size <= pool->max){
        p = pool->current;
        do{
            m = p->last;
            if((size_t)(p->end - m) >= size){
                p->last = m + size;
                return m;
            }
            p = p->next;
        }while(p);
        return mp_alloc_block(pool,size);
    }
    return mp_alloc_large(pool,size);
}


void* mp_calloc(struct mp_pool_s* pool,size_t size){
    void* p = mp_alloc(pool,size);
    if(p){
        memset(p,0,size);
    }
    return p;
}


//只是单纯的对large内存进行释放
void mp_free(struct mp_pool_s* pool,void* p){
    struct mp_large_s* l;
    for(l = pool->large;l;l = l->next){
        if(p == l->alloc){
            free(l->alloc);
            l->alloc = NULL;
            return ;
        }
    }
}

int main(int argc,char** argv){
    int size = 1 << 12;
    struct mp_pool_s* pool = mp_create_pool(size);

    int i = 0;
    for(;i < 10;++i){
        void* mp = mp_alloc(pool,512);
    }

    int j = 0;
    for(i = 0;i < 5;++i){
        char* pp = (char*)mp_calloc(pool,32);
        for(;j < 32;++j){
            if(pp[j]){
                printf("calloc failed\n");
            }
            printf("calloc success\n");
        }
    }


    for(i = 0;i < 5;++i){
        //分配的是large
        void* l = mp_alloc(pool,8192);
        mp_free(pool,l);
    }

    mp_destroy_pool(pool);
    return 0;
}