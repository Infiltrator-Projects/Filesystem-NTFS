/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native, bounded, read-only NTFS namespace for Common V2.
 *
 * Reads on-disk FILE records, USA-protected INDEX_ALLOCATION blocks, MFT
 * runlists and $DATA streams solely through partition-relative host I/O.
 * It refuses attribute lists, compressed/encrypted streams, impossible
 * geometry, malformed runlists, stale references and unsupported trees.
 * No NTFS-3G, C runtime allocation, Linux VFS or kernel driver is used.
 */
#include "ntfs_ro_v2.h"
#include "ntfs_disk.h"
#include <infiltratr/endian.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define U16(p) infiltratr_load_le16(p)
#define U32(p) infiltratr_load_le32(p)
#define U64(p) infiltratr_load_le64(p)
#define NTFS_ATTR_LIST 0x20u
#define NTFS_ATTR_DATA 0x80u
#define NTFS_ATTR_INDEX_ROOT 0x90u
#define NTFS_ATTR_INDEX_ALLOCATION 0xa0u
#define NTFS_ATTR_REPARSE 0xc0u
#define NTFS_ATTR_END 0xffffffffu
#define NTFS_MAX_RECORD 65536u
#define NTFS_MAX_INDEX 65536u
#define NTFS_MAX_INDEX_NODES 8192u
#define NTFS_MAX_DEPTH 12u
#define NTFS_ROOT_RECORD 5u
#define NTFS_VOLUME_RECORD 3u
#define NTFS_BITMAP_RECORD 6u

struct NtfsReadOnlyV2 {
    IfsFsHostRuntimeV2 host;
    NtfsDiskGeometry geometry;
    uint8_t *mft;
    uint64_t mft_length;
    uint64_t free_clusters;
};

static void *take(NtfsReadOnlyV2 *v,size_t n)
{ return v->host.allocate(v->host.context,n); }
static void drop(NtfsReadOnlyV2 *v,void *p)
{ if(p)v->host.release(v->host.context,p); }
static int bytes(NtfsReadOnlyV2 *v,uint64_t offset,void *output,uint32_t n)
{
    if(offset>v->host.volume_bytes || n>v->host.volume_bytes-offset)
        return IFS_FS_CORRUPT;
    return ifs_fs_host_read_bytes_v2(&v->host,offset,output,n);
}
static int fixup(uint8_t *p,uint32_t n,const char magic[4])
{
    if(n<512u || n%512u || memcmp(p,magic,4u))
        return IFS_FS_CORRUPT;
    uint16_t at=0u,count=0u;
    int rc=ntfs_disk_usa(p,n,&at,&count);
    if(rc!=IFS_FS_OK)return rc;
    uint16_t check=U16(p+at);
    for(uint32_t i=1u;i<count;++i) {
        uint32_t tail=i*512u-2u;
        if(U16(p+tail)!=check)return IFS_FS_CORRUPT;
        p[tail]=p[at+i*2u];
        p[tail+1u]=p[at+i*2u+1u];
    }
    return IFS_FS_OK;
}

/* Locate a complete, unnamed or $I30-named attribute in a validated FILE.
 * Attribute-list extensions are never silently ignored. */
static int attribute(const uint8_t *record,uint32_t n,uint32_t type,
                     const char *name,const uint8_t **out,uint32_t *length)
{
    if(n<48u || memcmp(record,"FILE",4u))return IFS_FS_CORRUPT;
    uint32_t used=U32(record+24u),at=U16(record+20u);
    if(used>n || at<48u || at>=used || (at&7u))
        return IFS_FS_CORRUPT;
    while(at<=used-4u) {
        const uint8_t *a=record+at;
        uint32_t kind=U32(a);
        if(kind==NTFS_ATTR_END)return IFS_FS_NOT_FOUND;
        uint32_t size=U32(a+4u);
        if(size<24u || (size&7u) || size>used-at)
            return IFS_FS_CORRUPT;
        if(kind==NTFS_ATTR_LIST)return IFS_FS_UNSUPPORTED;
        if(kind==type) {
            uint32_t nchars=a[9u],noff=U16(a+10u);
            uint32_t expected=0u;
            if(name)while(name[expected])++expected;
            if((name==NULL && nchars==0u) ||
               (name && expected==nchars)) {
                int same=1;
                if(name) {
                    if(noff>size || nchars>(size-noff)/2u)
                        return IFS_FS_CORRUPT;
                    for(uint32_t i=0u;i<nchars;++i)
                        if(U16(a+noff+2u*i)!=(uint8_t)name[i])same=0;
                }
                if(same) {
                    if(a[8u]>1u)return IFS_FS_CORRUPT;
                    *out=a;*length=size;
                    return IFS_FS_OK;
                }
            }
        }
        at+=size;
    }
    return IFS_FS_CORRUPT;
}
static int resident(const uint8_t *a,uint32_t n,
                    const uint8_t **p,uint32_t *count)
{
    if(n<24u || a[8u]!=0u)return IFS_FS_UNSUPPORTED;
    uint32_t at=U16(a+20u),length=U32(a+16u);
    if(at<24u || at>n || length>n-at)return IFS_FS_CORRUPT;
    *p=a+at;*count=length;
    return IFS_FS_OK;
}
static int stream_size(const uint8_t *a,uint32_t size,uint64_t *out)
{
    if(!a || size<24u)return IFS_FS_CORRUPT;
    if(U16(a+12u)&0xc001u)return IFS_FS_UNSUPPORTED;
    if(!a[8u]) {
        const uint8_t *data;uint32_t length;
        int rc=resident(a,size,&data,&length);
        if(rc!=IFS_FS_OK)return rc;
        *out=length;
        return IFS_FS_OK;
    }
    if(size<64u || U64(a+16u)!=0u || U16(a+32u)<64u ||
       U16(a+32u)>=size || a[34u]!=0u)
        return IFS_FS_UNSUPPORTED;
    uint64_t allocated=U64(a+40u),actual=U64(a+48u),initialized=U64(a+56u);
    if(initialized>actual || actual>allocated || allocated>UINT64_MAX-1u)
        return IFS_FS_CORRUPT;
    *out=actual;
    return IFS_FS_OK;
}

/* Return the actual cluster or a sparse marker for a VCN. Runlist offsets
 * are signed deltas; their magnitude is checked before every addition. */
static int run_cluster(NtfsReadOnlyV2 *v,const uint8_t *attr,uint32_t size,
                       uint64_t wanted,uint64_t *physical,int *sparse)
{
    uint32_t at=U16(attr+32u);
    uint64_t vcn=0u,lcn=0u;
    if(at<64u || at>=size)return IFS_FS_CORRUPT;
    for(unsigned limit=0u;at<size && limit<65536u;++limit) {
        uint8_t tag=attr[at++];
        if(tag==0u)break;
        uint32_t a=tag&15u,b=tag>>4u;
        if(!a || a>8u || b>8u || a+b>size-at)return IFS_FS_CORRUPT;
        uint64_t count=0u,delta=0u;
        for(unsigned i=0u;i<a;++i)count|=(uint64_t)attr[at+i]<<(8u*i);
        if(!count || count>UINT64_MAX-vcn)return IFS_FS_CORRUPT;
        at+=a;
        if(b) {
            for(unsigned i=0u;i<b;++i)delta|=(uint64_t)attr[at+i]<<(8u*i);
            int negative=(attr[at+b-1u]&0x80u)!=0u;
            if(negative) {
                uint64_t magnitude=b==8u?((~delta)+1u):
                    ((UINT64_C(1)<<(8u*b))-delta);
                if(!magnitude || magnitude>lcn)return IFS_FS_CORRUPT;
                lcn-=magnitude;
            } else {
                if(delta>v->geometry.cluster_count-lcn)return IFS_FS_CORRUPT;
                lcn+=delta;
            }
        }
        at+=b;
        if(wanted>=vcn && wanted<vcn+count) {
            *sparse=(b==0u);
            if(!*sparse) {
                uint64_t pos=wanted-vcn;
                if(lcn>=v->geometry.cluster_count ||
                   pos>=v->geometry.cluster_count-lcn)
                    return IFS_FS_CORRUPT;
                *physical=lcn+pos;
            } else *physical=0u;
            return IFS_FS_OK;
        }
        vcn+=count;
    }
    return IFS_FS_CORRUPT;
}
static int stream_read(NtfsReadOnlyV2 *v,const uint8_t *a,uint32_t size,
                       uint64_t at,void *out,uint32_t n)
{
    uint64_t length=0u;
    int rc=stream_size(a,size,&length);
    if(rc!=IFS_FS_OK)return rc;
    if(at>length || n>length-at || (!out && n))
        return IFS_FS_INVALID;
    if(!n)return IFS_FS_OK;
    if(!a[8u]) {
        const uint8_t *data;uint32_t resident_bytes;
        rc=resident(a,size,&data,&resident_bytes);
        if(rc!=IFS_FS_OK)return rc;
        memcpy(out,data+(size_t)at,n);
        return IFS_FS_OK;
    }
    uint64_t initialized=U64(a+56u);
    uint32_t cluster=v->geometry.cluster_bytes;
    uint8_t *dest=out;
    while(n) {
        uint64_t vcn=at/cluster;
        uint32_t offset=(uint32_t)(at%cluster);
        uint32_t chunk=cluster-offset;
        if(chunk>n)chunk=n;
        if(at>=initialized) {
            memset(dest,0u,chunk);
        } else {
            if((uint64_t)chunk>initialized-at)
                chunk=(uint32_t)(initialized-at);
            uint64_t pcn=0u;int sparse=0;
            rc=run_cluster(v,a,size,vcn,&pcn,&sparse);
            if(rc!=IFS_FS_OK)return rc;
            if(sparse)memset(dest,0u,chunk);
            else {
                if(pcn>UINT64_MAX/cluster)return IFS_FS_CORRUPT;
                rc=bytes(v,pcn*(uint64_t)cluster+offset,dest,chunk);
                if(rc!=IFS_FS_OK)return rc;
            }
        }
        dest+=chunk;at+=chunk;n-=chunk;
    }
    return IFS_FS_OK;
}
static int record_load(NtfsReadOnlyV2 *v,uint64_t record,uint8_t **output)
{
    if(record>UINT64_MAX/v->geometry.record_bytes)
        return IFS_FS_CORRUPT;
    uint64_t pos=record*v->geometry.record_bytes;
    if(pos>v->mft_length || v->geometry.record_bytes>v->mft_length-pos)
        return IFS_FS_CORRUPT;
    uint8_t *p=take(v,v->geometry.record_bytes);
    if(!p)return IFS_FS_NO_MEMORY;
    const uint8_t *a;uint32_t size;
    int rc=attribute(v->mft,v->geometry.record_bytes,
                     NTFS_ATTR_DATA,NULL,&a,&size);
    if(rc==IFS_FS_OK)
        rc=stream_read(v,a,size,pos,p,v->geometry.record_bytes);
    if(rc==IFS_FS_OK)rc=fixup(p,v->geometry.record_bytes,"FILE");
    if(rc==IFS_FS_OK && !(U16(p+22u)&1u))
        rc=IFS_FS_NOT_FOUND;
    if(rc!=IFS_FS_OK) {drop(v,p);return rc;}
    *output=p;
    return IFS_FS_OK;
}
static int file_data(NtfsReadOnlyV2 *v,const uint8_t *record,
                     const uint8_t **attr,uint32_t *size,uint64_t *length)
{
    int rc=attribute(record,v->geometry.record_bytes,
                     NTFS_ATTR_DATA,NULL,attr,size);
    if(rc!=IFS_FS_OK)return rc;
    return stream_size(*attr,*size,length);
}

/* Convert a validated NTFS file-name key to bounded UTF-8. We intentionally
 * reject noncanonical surrogates and embedded NUL/slash, not truncate them. */
static int name_utf8(const uint8_t *key,uint32_t key_len,
                     char name[768],size_t *written,int *dos_only)
{
    if(key_len<66u)return IFS_FS_CORRUPT;
    unsigned chars=key[64u],kind=key[65u];
    if(kind>3u || chars>255u || 66u+2u*chars>key_len)
        return IFS_FS_CORRUPT;
    *dos_only=kind==2u;
    size_t n=0u;
    for(unsigned i=0u;i<chars;++i) {
        uint32_t cp=U16(key+66u+i*2u);
        if(cp>=0xd800u && cp<=0xdbffu) {
            if(i+1u>=chars)return IFS_FS_CORRUPT;
            uint32_t low=U16(key+66u+(i+1u)*2u);
            if(low<0xdc00u || low>0xdfffu)return IFS_FS_CORRUPT;
            cp=0x10000u+((cp-0xd800u)<<10u)+(low-0xdc00u);
            ++i;
        } else if(cp>=0xdc00u && cp<=0xdfffu)return IFS_FS_CORRUPT;
        if(cp==0u || cp=='/')return IFS_FS_CORRUPT;
        if(cp<0x80u)name[n++]=(char)cp;
        else if(cp<0x800u) {
            name[n++]=(char)(0xc0u|(cp>>6u));
            name[n++]=(char)(0x80u|(cp&63u));
        } else if(cp<0x10000u) {
            name[n++]=(char)(0xe0u|(cp>>12u));
            name[n++]=(char)(0x80u|((cp>>6u)&63u));
            name[n++]=(char)(0x80u|(cp&63u));
        } else {
            name[n++]=(char)(0xf0u|(cp>>18u));
            name[n++]=(char)(0x80u|((cp>>12u)&63u));
            name[n++]=(char)(0x80u|((cp>>6u)&63u));
            name[n++]=(char)(0x80u|(cp&63u));
        }
    }
    if(!n || n>=768u)return IFS_FS_CORRUPT;
    name[n]=0;*written=n;
    return IFS_FS_OK;
}
static int equal_name(const char *a,size_t n,const char *b,size_t m)
{
    if(n!=m)return 0;
    for(size_t i=0;i<n;++i) {
        unsigned char x=(unsigned char)a[i],y=(unsigned char)b[i];
        if(x>='A' && x<='Z')x=(unsigned char)(x+32u);
        if(y>='A' && y<='Z')y=(unsigned char)(y+32u);
        if(x!=y)return 0;
    }
    return 1;
}
typedef struct {
    const char *wanted;
    size_t wanted_length;
    uint32_t index;
    uint64_t reference;
    char name[768];
    size_t name_length;
    uint32_t nodes;
    int found;
} Search;

static int index_walk(NtfsReadOnlyV2 *v,const uint8_t *header,
                      uint32_t available,const uint8_t *allocation,
                      uint32_t allocation_bytes,uint32_t index_bytes,
                      Search *find,unsigned depth);

static int index_child(NtfsReadOnlyV2 *v,const uint8_t *allocation,
                       uint32_t allocation_bytes,uint32_t index_bytes,
                       uint64_t vcn,Search *find,unsigned depth)
{
    if(!allocation || !index_bytes || index_bytes>NTFS_MAX_INDEX ||
       index_bytes%512u || depth>=NTFS_MAX_DEPTH ||
       vcn>UINT64_MAX/v->geometry.cluster_bytes)
        return IFS_FS_UNSUPPORTED;
    uint64_t at=vcn*(uint64_t)v->geometry.cluster_bytes;
    uint8_t *block=take(v,index_bytes);
    if(!block)return IFS_FS_NO_MEMORY;
    int rc=stream_read(v,allocation,allocation_bytes,at,block,index_bytes);
    if(rc==IFS_FS_OK)rc=fixup(block,index_bytes,"INDX");
    if(rc==IFS_FS_OK && U64(block+16u)!=vcn)
        rc=IFS_FS_CORRUPT;
    if(rc==IFS_FS_OK)
        rc=index_walk(v,block+24u,index_bytes-24u,allocation,
                      allocation_bytes,index_bytes,find,depth+1u);
    drop(v,block);
    return rc;
}
static int index_walk(NtfsReadOnlyV2 *v,const uint8_t *header,
                      uint32_t available,const uint8_t *allocation,
                      uint32_t allocation_bytes,uint32_t index_bytes,
                      Search *find,unsigned depth)
{
    if(depth>=NTFS_MAX_DEPTH || ++find->nodes>NTFS_MAX_INDEX_NODES)
        return IFS_FS_UNSUPPORTED;
    if(available<16u)return IFS_FS_CORRUPT;
    uint32_t start=U32(header),length=U32(header+4u);
    if(start<16u || start>length || length>available)
        return IFS_FS_CORRUPT;
    for(uint32_t pos=start;pos<length;) {
        if(length-pos<16u)return IFS_FS_CORRUPT;
        const uint8_t *item=header+pos;
        uint32_t n=U16(item+8u),keylen=U16(item+10u),flags=U16(item+12u);
        if(n<16u || (n&7u) || n>length-pos ||
           (flags&~3u) || keylen>n-16u ||
           ((flags&1u) && n<24u))
            return IFS_FS_CORRUPT;
        if(flags&1u) {
            uint64_t vcn=U64(item+n-8u);
            int rc=index_child(v,allocation,allocation_bytes,
                               index_bytes,vcn,find,depth);
            if(rc!=IFS_FS_OK)return rc;
            if(find->found)return IFS_FS_OK;
        }
        if(!(flags&2u)) {
            if(n<16u+keylen+((flags&1u)?8u:0u))
                return IFS_FS_CORRUPT;
            char name[768];size_t length_name=0u;int dos=0;
            int rc=name_utf8(item+16u,keylen,name,&length_name,&dos);
            if(rc!=IFS_FS_OK)return rc;
            if(!dos) {
                if((find->wanted && equal_name(name,length_name,
                                               find->wanted,find->wanted_length)) ||
                   (!find->wanted && find->index--==0u)) {
                    uint64_t reference=U64(item)&UINT64_C(0x0000ffffffffffff);
                    /* NTFS MFT record zero is the real $MFT system file.
                     * It is a valid directory reference, not a null ID. */
                    find->reference=reference;
                    memcpy(find->name,name,length_name+1u);
                    find->name_length=length_name;
                    find->found=1;
                    return IFS_FS_OK;
                }
            }
        }
        pos+=n;
        if(flags&2u)return IFS_FS_OK;
    }
    return IFS_FS_CORRUPT;
}
static int index_find(NtfsReadOnlyV2 *v,const uint8_t *directory,Search *find)
{
    const uint8_t *root=NULL,*allocation=NULL,*data=NULL;
    uint32_t root_len=0u,alloc_len=0u,n=0u;
    int rc=attribute(directory,v->geometry.record_bytes,
                     NTFS_ATTR_INDEX_ROOT,"$I30",&root,&root_len);
    if(rc!=IFS_FS_OK)return rc;
    rc=resident(root,root_len,&data,&n);
    if(rc!=IFS_FS_OK)return rc;
    if(n<32u || U32(data)!=0x30u || U32(data+4u)!=1u)
        return IFS_FS_UNSUPPORTED;
    uint32_t block_bytes=U32(data+8u);
    if(block_bytes<512u || block_bytes>NTFS_MAX_INDEX ||
       block_bytes%512u)
        return IFS_FS_UNSUPPORTED;
    rc=attribute(directory,v->geometry.record_bytes,
                 NTFS_ATTR_INDEX_ALLOCATION,"$I30",&allocation,&alloc_len);
    if(rc!=IFS_FS_OK && rc!=IFS_FS_NOT_FOUND)return rc;
    if(rc==IFS_FS_NOT_FOUND){allocation=NULL;alloc_len=0u;}
    rc=index_walk(v,data+16u,n-16u,allocation,
                  alloc_len,block_bytes,find,0u);
    if(rc!=IFS_FS_OK)return rc;
    return find->found?IFS_FS_OK:IFS_FS_NOT_FOUND;
}
static int resolve(NtfsReadOnlyV2 *v,const char *path,
                   uint64_t *record,char *name,size_t *name_length)
{
    if(!path || path[0]!='/')return IFS_FS_INVALID;
    uint64_t current=NTFS_ROOT_RECORD;
    const char *p=path+1u;
    *name_length=1u;name[0]='/';name[1]=0;
    if(!*p){*record=current;return IFS_FS_OK;}
    for(unsigned depth=0u;depth<64u;++depth) {
        const char *start=p;
        while(*p && *p!='/') {
            if((size_t)(p-start)>=767u)return IFS_FS_INVALID;
            ++p;
        }
        size_t length=(size_t)(p-start);
        if(!length || (length==1u && start[0]=='.') ||
           (length==2u && start[0]=='.' && start[1]=='.'))
            return IFS_FS_INVALID;
        uint8_t *dir=NULL;
        int rc=record_load(v,current,&dir);
        if(rc!=IFS_FS_OK)return rc;
        if(!(U16(dir+22u)&2u)) {drop(v,dir);return IFS_FS_NOT_FOUND;}
        Search search={0};
        search.wanted=start;search.wanted_length=length;
        rc=index_find(v,dir,&search);
        drop(v,dir);
        if(rc!=IFS_FS_OK)return rc;
        current=search.reference;
        memcpy(name,search.name,search.name_length+1u);
        *name_length=search.name_length;
        if(!*p){*record=current;return IFS_FS_OK;}
        ++p;
    }
    return IFS_FS_INVALID;
}
static int entry(NtfsReadOnlyV2 *v,uint64_t record,const char *name,
                 size_t name_length,IfsFsEntryV2 *out)
{
    if(!out || out->structure_bytes<sizeof(*out) ||
       !out->name || name_length>=out->name_capacity)
        return out && out->name && name_length>=out->name_capacity?
            IFS_FS_NO_SPACE:IFS_FS_INVALID;
    uint8_t *mft=NULL;int rc=record_load(v,record,&mft);
    if(rc!=IFS_FS_OK)return rc;
    const uint8_t *reparse;uint32_t rep_len=0u;
    rc=attribute(mft,v->geometry.record_bytes,
                 NTFS_ATTR_REPARSE,NULL,&reparse,&rep_len);
    /* A reparse point has a real directory entry, but cannot be followed
     * without implementing its specific reparse tag semantics. */
    int is_reparse=rc==IFS_FS_OK;
    if(rc!=IFS_FS_OK && rc!=IFS_FS_NOT_FOUND){drop(v,mft);return rc;}
    uint32_t type=(U16(mft+22u)&2u)?
        IFS_FS_ENTRY_DIRECTORY:(is_reparse?
            IFS_FS_ENTRY_LINK:IFS_FS_ENTRY_FILE);
    uint64_t length=0u;
    if(type==IFS_FS_ENTRY_FILE) {
        const uint8_t *attr;uint32_t size;
        rc=file_data(v,mft,&attr,&size,&length);
        /* NTFS allows regular records with only named data streams.
         * Such a file has an empty unnamed (default) stream. */
        if(rc!=IFS_FS_OK && rc!=IFS_FS_NOT_FOUND) {
            drop(v,mft);return rc;
        }
    }
    drop(v,mft);
    memcpy(out->name,name,name_length);
    out->name[name_length]=0;
    out->name_bytes=(uint32_t)name_length;
    out->type=type;
    out->attributes=0u;
    out->size_bytes=length;
    return IFS_FS_OK;
}
static int bitmap_free(NtfsReadOnlyV2 *v,uint64_t *free_clusters)
{
    uint8_t *record=NULL;
    int rc=record_load(v,NTFS_BITMAP_RECORD,&record);
    if(rc!=IFS_FS_OK)return rc;
    const uint8_t *attr;uint32_t size;uint64_t length;
    rc=file_data(v,record,&attr,&size,&length);
    if(rc!=IFS_FS_OK){drop(v,record);return rc;}
    const uint64_t total=v->geometry.cluster_count;
    if(length<(total+7u)/8u){drop(v,record);return IFS_FS_CORRUPT;}
    uint64_t used=0u,at=0u,need=(total+7u)/8u;
    uint8_t buffer[4096];
    while(at<need) {
        uint32_t chunk=(need-at)>sizeof(buffer)?
            (uint32_t)sizeof(buffer):(uint32_t)(need-at);
        rc=stream_read(v,attr,size,at,buffer,chunk);
        if(rc!=IFS_FS_OK){drop(v,record);return rc;}
        for(uint32_t i=0u;i<chunk;++i) {
            uint8_t bits=buffer[i];
            if(at+i==need-1u && total%8u)
                bits&=(uint8_t)((1u<<(total%8u))-1u);
            while(bits){bits&=(uint8_t)(bits-1u);++used;}
        }
        at+=chunk;
    }
    drop(v,record);
    if(used>total)return IFS_FS_CORRUPT;
    *free_clusters=total-used;
    return IFS_FS_OK;
}
int ntfs_ro_open_v2(const IfsFsHostRuntimeV2 *host,
                    NtfsReadOnlyV2 **output,IfsFsVolumeInfoV2 *info)
{
    if(!output || !info || info->structure_bytes<sizeof(*info) ||
       !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *output=NULL;
    if(!(host->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    uint8_t boot[512];int rc=ifs_fs_host_read_bytes_v2(host,0u,boot,sizeof(boot));
    if(rc!=IFS_FS_OK)return rc;
    NtfsDiskGeometry g;
    rc=ntfs_disk_geometry(boot,host->volume_bytes,&g);
    if(rc!=IFS_FS_OK)return rc;
    if(g.record_bytes>NTFS_MAX_RECORD)return IFS_FS_UNSUPPORTED;
    NtfsReadOnlyV2 *v=host->allocate(host->context,sizeof(*v));
    if(!v)return IFS_FS_NO_MEMORY;
    memset(v,0,sizeof(*v));
    size_t supplied=host->structure_bytes<sizeof(v->host)?
                    host->structure_bytes:sizeof(v->host);
    memcpy(&v->host,host,supplied);
    v->host.structure_bytes=(uint32_t)supplied;
    v->geometry=g;
    v->mft=take(v,g.record_bytes);
    if(!v->mft){drop(v,v);return IFS_FS_NO_MEMORY;}
    rc=bytes(v,g.mft_lcn*(uint64_t)g.cluster_bytes,v->mft,g.record_bytes);
    if(rc==IFS_FS_OK)rc=fixup(v->mft,g.record_bytes,"FILE");
    if(rc==IFS_FS_OK && !(U16(v->mft+22u)&1u))
        rc=IFS_FS_CORRUPT;
    const uint8_t *attr=NULL;uint32_t a_bytes=0u;
    if(rc==IFS_FS_OK)
        rc=attribute(v->mft,g.record_bytes,NTFS_ATTR_DATA,NULL,&attr,&a_bytes);
    if(rc==IFS_FS_OK) {
        if(attr[8u]!=1u)rc=IFS_FS_CORRUPT;
        else rc=stream_size(attr,a_bytes,&v->mft_length);
    }
    if(rc==IFS_FS_OK) {
        uint8_t *root=NULL;
        rc=record_load(v,NTFS_ROOT_RECORD,&root);
        if(rc==IFS_FS_OK) {
            if(!(U16(root+22u)&2u))rc=IFS_FS_CORRUPT;
            drop(v,root);
        }
    }
    if(rc==IFS_FS_OK)rc=bitmap_free(v,&v->free_clusters);
    if(rc!=IFS_FS_OK) {ntfs_ro_close_v2(v);return rc;}
    *info=(IfsFsVolumeInfoV2){
        .structure_bytes=sizeof(*info),.variant=1u,
        .block_bytes=g.cluster_bytes,.max_name_bytes=255u,
        .total_bytes=g.volume_bytes,
        .free_bytes=v->free_clusters*g.cluster_bytes
    };
    *output=v;
    return IFS_FS_OK;
}
void ntfs_ro_close_v2(NtfsReadOnlyV2 *v)
{
    if(!v)return;
    drop(v,v->mft);
    drop(v,v);
}
int ntfs_ro_query_v2(NtfsReadOnlyV2 *v,IfsFsVolumeInfoV2 *info)
{
    if(!v || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *info=(IfsFsVolumeInfoV2){
        .structure_bytes=sizeof(*info),.variant=1u,
        .block_bytes=v->geometry.cluster_bytes,.max_name_bytes=255u,
        .total_bytes=v->geometry.volume_bytes,
        .free_bytes=v->free_clusters*v->geometry.cluster_bytes
    };
    return IFS_FS_OK;
}
int ntfs_ro_stat_v2(NtfsReadOnlyV2 *v,const char *path,IfsFsEntryV2 *out)
{
    if(!v)return IFS_FS_INVALID;
    uint64_t record=0u;char name[768];size_t length=0u;
    int rc=resolve(v,path,&record,name,&length);
    return rc==IFS_FS_OK?entry(v,record,name,length,out):rc;
}
int ntfs_ro_list_v2(NtfsReadOnlyV2 *v,const char *path,
                    uint32_t index,IfsFsEntryV2 *out)
{
    if(!v)return IFS_FS_INVALID;
    uint64_t record=0u;char name[768];size_t n=0u;
    int rc=resolve(v,path,&record,name,&n);
    if(rc!=IFS_FS_OK)return rc;
    uint8_t *dir=NULL;
    rc=record_load(v,record,&dir);
    if(rc!=IFS_FS_OK)return rc;
    if(!(U16(dir+22u)&2u)){drop(v,dir);return IFS_FS_INVALID;}
    Search search={0};search.index=index;
    rc=index_find(v,dir,&search);
    drop(v,dir);
    if(rc==IFS_FS_NOT_FOUND)return IFS_FS_END;
    return rc==IFS_FS_OK?
        entry(v,search.reference,search.name,search.name_length,out):rc;
}
int ntfs_ro_read_v2(NtfsReadOnlyV2 *v,const char *path,
                    uint64_t offset,void *out,uint32_t n)
{
    if(!v || (!out && n))return IFS_FS_INVALID;
    uint64_t number=0u;char name[768];size_t len=0u;
    int rc=resolve(v,path,&number,name,&len);
    if(rc!=IFS_FS_OK)return rc;
    uint8_t *record=NULL;
    rc=record_load(v,number,&record);
    if(rc!=IFS_FS_OK)return rc;
    if(U16(record+22u)&2u)rc=IFS_FS_INVALID;
    else {
        const uint8_t *attr;uint32_t size;uint64_t length;
        rc=file_data(v,record,&attr,&size,&length);
        if(rc==IFS_FS_OK)rc=stream_read(v,attr,size,offset,out,n);
        else if(rc==IFS_FS_NOT_FOUND)
            rc=(!offset && !n)?IFS_FS_OK:IFS_FS_INVALID;
    }
    drop(v,record);
    return rc;
}
