/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the exact NTFS FILE attribute decoder compiled into the hosted
 * and freestanding Common V2 readers. The existing real-media V2 suite
 * continues to qualify complete NTFS namespace and file reads.
 */
#include "../native/ntfs_ro_v2.c"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){ \
    fprintf(stderr,"NTFS V2 attribute regression line %u: %s\n", \
            (unsigned)__LINE__,#x);return 1; \
}} while(0)
static void store16(uint8_t *p,uint16_t v)
{
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);
}
static void store32(uint8_t *p,uint32_t v)
{
    for(unsigned i=0u;i<4u;++i)p[i]=(uint8_t)(v>>(8u*i));
}
static uint8_t record[512];
static void make_record(void)
{
    memset(record,0,sizeof(record));
    memcpy(record,"FILE",4u);
    store16(record+20u,56u); /* first attribute begins at an aligned offset */
    store32(record+24u,124u);
    store32(record+56u,NTFS_ATTR_DATA);
    store32(record+60u,32u);
    store32(record+88u,NTFS_ATTR_END);
}
int main(void)
{
    const uint8_t *matched=NULL;
    uint32_t n=0u;
    make_record();
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_OK);
    CHECK(matched==record+56u && n==32u);

    /* The duplicate is a syntactically aligned attribute with the same
     * unnamed $DATA key. It must invalidate the complete FILE chain. */
    make_record();
    store32(record+88u,NTFS_ATTR_DATA);
    store32(record+92u,32u);
    store32(record+120u,NTFS_ATTR_END);
    matched=(const uint8_t *)(uintptr_t)1u;n=123u;
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_CORRUPT);
    CHECK(matched==(const uint8_t *)(uintptr_t)1u && n==123u);

    /* Unrelated attributes may appear after $DATA; they must not be
     * mistaken for a duplicate, provided the chain is terminated. */
    store32(record+88u,0x10u);
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_OK);
    CHECK(matched==record+56u && n==32u);

    /* An attribute list, later corruption or a missing terminator must
     * never be concealed by the earlier valid data stream. */
    store32(record+88u,NTFS_ATTR_LIST);
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_UNSUPPORTED);
    store32(record+88u,0x10u);store32(record+92u,0u);
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_CORRUPT);
    make_record();
    store32(record+88u,0x10u);
    store32(record+92u,32u);
    store32(record+24u,120u); /* no NTFS_ATTR_END in used bytes */
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_CORRUPT);
    CHECK(attribute(NULL,sizeof(record),NTFS_ATTR_DATA,
                    NULL,&matched,&n)==IFS_FS_INVALID);
    CHECK(attribute(record,sizeof(record),NTFS_ATTR_DATA,
                    NULL,NULL,&n)==IFS_FS_INVALID);
    return 0;
}
