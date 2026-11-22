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

    /* This same native decoder now supplies the hosted Common V2
     * DEFRAG.MAP group. Validate the complete mapping-pairs stream,
     * partition bounds, sparse holes and pagination without a device. */
    {
        NtfsReadOnlyV2 v={0};
        v.geometry.cluster_bytes=4096u;
        v.geometry.cluster_count=100u;
        uint8_t attr[80]={0};
        IfsFsExtentV2 extents[3]={0};
        uint32_t written=0u;
        attr[8u]=1u;
        store16(attr+32u,64u);
        attr[64u]=0x11u;attr[65u]=3u;attr[66u]=2u;
        CHECK(map_runs_v2(&v,attr,sizeof(attr),12288u,0u,
                          extents,3u,&written)==IFS_FS_OK);
        CHECK(written==1u && extents[0].logical_offset==0u &&
              extents[0].physical_offset==8192u &&
              extents[0].length==12288u);
        written=0u;
        CHECK(map_runs_v2(&v,attr,sizeof(attr),12288u,1u,
                          extents,3u,&written)==IFS_FS_END &&
              written==0u);

        memset(attr+64u,0,sizeof(attr)-64u);
        attr[64u]=1u;attr[65u]=1u; /* sparse VCN 0 */
        attr[66u]=0x11u;attr[67u]=1u;attr[68u]=2u;
        CHECK(map_runs_v2(&v,attr,sizeof(attr),8192u,0u,
                          extents,3u,&written)==IFS_FS_OK);
        CHECK(written==1u && extents[0].logical_offset==4096u &&
              extents[0].physical_offset==8192u &&
              extents[0].length==4096u);

        memset(attr+64u,0,sizeof(attr)-64u);
        attr[64u]=0x11u;attr[65u]=1u;attr[66u]=200u;
        written=0u;
        CHECK(map_runs_v2(&v,attr,sizeof(attr),4096u,0u,
                          extents,3u,&written)==IFS_FS_CORRUPT &&
              written==0u);
        memset(attr+64u,0x11u,sizeof(attr)-64u);
        CHECK(map_runs_v2(&v,attr,sizeof(attr),4096u,0u,
                          extents,3u,&written)==IFS_FS_CORRUPT &&
              written==0u);
        attr[8u]=0u;
        CHECK(map_runs_v2(&v,attr,sizeof(attr),4096u,0u,
                          extents,3u,&written)==IFS_FS_END &&
              written==0u);
    }

    /* The authoritative $UpCase mapping is read from NTFS record 10,
     * not a host libc locale. Case equality must include Latin-1 UTF-8
     * filenames (CAFÉ vs Café) and reject overlong encodings. */
    {
        static uint8_t upcase[NTFS_UPCASE_TABLE_BYTES];
        NtfsReadOnlyV2 v={0};
        for(uint32_t i=0u;i<UINT32_C(65536);++i)
            store16(upcase+2u*i,(uint16_t)i);
        for(uint32_t i='a';i<='z';++i)
            store16(upcase+2u*i,(uint16_t)(i-32u));
        store16(upcase+2u*UINT32_C(0x00e9),UINT16_C(0x00c9));
        v.upcase=upcase;
        {
            const char lower[]="Caf\xc3\xa9.bin";
            const char upper[]="CAF\xc3\x89.BIN";
            const char invalid[]="CA\xc0\x81";
            CHECK(equal_name(&v,lower,sizeof(lower)-1u,upper,
                             sizeof(upper)-1u)==1);
            CHECK(equal_name(&v,lower,sizeof(lower)-1u,"CAFET.BIN",
                             sizeof("CAFET.BIN")-1u)==0);
            CHECK(equal_name(&v,invalid,sizeof(invalid)-1u,"CA",
                             sizeof("CA")-1u)==-1);
        }
    }
    return 0;
}
