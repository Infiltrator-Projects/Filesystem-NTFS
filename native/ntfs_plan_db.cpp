// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NTFS plan-database resource ownership.
 *
 * The on-disk planner and relocation engine remain C. This narrow C++ unit
 * exists because SQLite statements, rollback state and OpenSSL digest contexts
 * have deterministic lifetime semantics that are clearer and safer under RAII
 * than through repeated goto/finalize/free paths. All exported functions keep
 * the existing C ABI and no inheritance or runtime polymorphism is used.
 */
extern "C" {
#include "ntfs_native.h"
#include "ld_io.h"
}
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <unistd.h>
#include <vector>

namespace {
class SqliteStatement {
public:
    SqliteStatement() noexcept = default;
    ~SqliteStatement() { sqlite3_finalize(statement_); }
    SqliteStatement(const SqliteStatement &) = delete;
    SqliteStatement &operator=(const SqliteStatement &) = delete;
    sqlite3_stmt **out() noexcept { return &statement_; }
    sqlite3_stmt *get() const noexcept { return statement_; }
private:
    sqlite3_stmt *statement_ = nullptr;
};
class RollbackGuard {
public:
    explicit RollbackGuard(sqlite3 *db) noexcept : db_(db) {}
    ~RollbackGuard() { if (active_) (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    RollbackGuard(const RollbackGuard &) = delete;
    RollbackGuard &operator=(const RollbackGuard &) = delete;
    void release() noexcept { active_ = false; }
private:
    sqlite3 *db_;
    bool active_ = true;
};
struct EvpContextDeleter { void operator()(EVP_MD_CTX *p) const noexcept { EVP_MD_CTX_free(p); } };
using EvpContext = std::unique_ptr<EVP_MD_CTX, EvpContextDeleter>;
int sql_error(sqlite3 *db,char **error,const char *action){ntfs_set_error(error,"%s: %s",action,sqlite3_errmsg(db));return -1;}
int sql_exec(sqlite3 *db,const char *sql,char **error){char *message=nullptr;int code=sqlite3_exec(db,sql,nullptr,nullptr,&message);if(code==SQLITE_OK)return 0;ntfs_set_error(error,"NTFS plan database: %s",message!=nullptr?message:sqlite3_errmsg(db));sqlite3_free(message);return -1;}
int prepare(sqlite3 *db,SqliteStatement &statement,const char *sql,char **error,const char *action){if(sqlite3_prepare_v2(db,sql,-1,statement.out(),nullptr)==SQLITE_OK)return 0;return sql_error(db,error,action);}
const NtfsPlacement *find_placement(const NtfsPlacementVec *placements,std::uint64_t record,std::uint32_t offset) noexcept{for(std::size_t i=0;i<placements->count;++i){const NtfsPlacement *p=&placements->items[i];if(p->record_number==record&&p->attribute_offset==offset)return p;}return nullptr;}
bool primary_object_stream(const NtfsStream *stream) noexcept{if(stream->directory)return stream->attribute_type==NTFS_ATTR_INDEX_ALLOCATION;return stream->attribute_type==NTFS_ATTR_DATA&&stream->attribute_name[0]=='\0';}
}

extern "C" int ntfs_stream_digest(NtfsVolume *volume,const NtfsStream *stream,std::uint8_t *digest,char **error){
    EvpContext context(EVP_MD_CTX_new());
    if(!context||EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1){ntfs_set_error(error,"initializing NTFS payload digest failed");return -1;}
    const std::size_t cluster_size=static_cast<std::size_t>(volume->cluster_size);
    std::vector<std::uint8_t> buffer;
    try {
        buffer.resize(cluster_size);
    } catch (const std::bad_alloc &) {
        ntfs_set_error(error,"allocating NTFS payload digest buffer failed");
        return -1;
    }
    for(std::size_t r=0;r<stream->runs.count;++r){NtfsRun run=stream->runs.items[r];if(run.sparse){ntfs_set_error(error,"sparse NTFS stream entered native writer");return -1;}for(std::uint64_t c=0;c<run.length;++c){std::uint64_t offset=(run.lcn+c)*static_cast<std::uint64_t>(volume->cluster_size);ssize_t got=ld_pread_full(volume->fd,buffer.data(),cluster_size,offset);if(got<0||static_cast<std::size_t>(got)!=cluster_size||EVP_DigestUpdate(context.get(),buffer.data(),cluster_size)!=1){ntfs_set_error(error,"reading NTFS payload for verification failed");return -1;}}}
    unsigned int digest_length=0U;if(EVP_DigestFinal_ex(context.get(),digest,&digest_length)!=1||digest_length!=SHA256_DIGEST_LENGTH){ntfs_set_error(error,"finalizing NTFS payload digest failed");return -1;}return 0;
}

extern "C" int ntfs_create_plan_db(const char *path,NtfsVolume *volume,NtfsLayout *layout,NtfsCatalogue *catalogue,const NtfsPlacementVec *placements,bool growth,sqlite3 **db,char **error){
    (void)unlink(path);int flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags|=SQLITE_OPEN_NOFOLLOW;
#endif
    if(sqlite3_open_v2(path,db,flags,nullptr)!=SQLITE_OK)return sql_error(*db,error,"opening NTFS plan database");
    if(sql_exec(*db,"PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;CREATE TABLE streams(record INTEGER,attr INTEGER,type INTEGER,clusters INTEGER,target INTEGER,reserve INTEGER,sha BLOB,PRIMARY KEY(record,attr));CREATE TABLE fixed_primary(record INTEGER,attr INTEGER,clusters INTEGER,start INTEGER,reserve INTEGER,sha BLOB,PRIMARY KEY(record,attr));CREATE TABLE blocks(old INTEGER PRIMARY KEY,target INTEGER UNIQUE,placed INTEGER DEFAULT 0);CREATE TABLE metadata(key TEXT PRIMARY KEY,value BLOB);BEGIN IMMEDIATE",error)!=0)return -1;
    RollbackGuard rollback(*db);SqliteStatement ins_stream,ins_fixed,ins_block,ins_meta;
    if(prepare(*db,ins_stream,"INSERT INTO streams VALUES (?,?,?,?,?,?,?)",error,"preparing NTFS stream plan")!=0||prepare(*db,ins_fixed,"INSERT INTO fixed_primary VALUES (?,?,?,?,?,?)",error,"preparing NTFS fixed-stream plan")!=0||prepare(*db,ins_block,"INSERT INTO blocks(old,target) VALUES (?,?)",error,"preparing NTFS cluster permutation")!=0||prepare(*db,ins_meta,"INSERT INTO metadata VALUES (?,?)",error,"preparing NTFS plan metadata")!=0)return -1;
    for(std::size_t i=0;i<catalogue->count;++i){NtfsStream *stream=&catalogue->items[i];if(!stream->movable||stream->clusters==0U)continue;const NtfsPlacement *placement=find_placement(placements,stream->record_number,stream->attribute_offset);if(placement==nullptr){ntfs_set_error(error,"NTFS planner omitted a movable stream");return -1;}std::uint8_t digest[SHA256_DIGEST_LENGTH];if(ntfs_stream_digest(volume,stream,digest,error)!=0)return -1;sqlite3_reset(ins_stream.get());sqlite3_clear_bindings(ins_stream.get());sqlite3_bind_int64(ins_stream.get(),1,static_cast<sqlite3_int64>(stream->record_number));sqlite3_bind_int(ins_stream.get(),2,static_cast<int>(stream->attribute_offset));sqlite3_bind_int(ins_stream.get(),3,static_cast<int>(stream->attribute_type));sqlite3_bind_int64(ins_stream.get(),4,static_cast<sqlite3_int64>(stream->clusters));sqlite3_bind_int64(ins_stream.get(),5,static_cast<sqlite3_int64>(placement->start));sqlite3_bind_int64(ins_stream.get(),6,static_cast<sqlite3_int64>(placement->reserve));sqlite3_bind_blob(ins_stream.get(),7,digest,SHA256_DIGEST_LENGTH,SQLITE_TRANSIENT);if(sqlite3_step(ins_stream.get())!=SQLITE_DONE)return sql_error(*db,error,"recording NTFS stream plan");std::uint64_t target=placement->start;for(std::size_t r=0;r<stream->runs.count;++r){NtfsRun run=stream->runs.items[r];for(std::uint64_t c=0;c<run.length;++c){sqlite3_reset(ins_block.get());sqlite3_clear_bindings(ins_block.get());sqlite3_bind_int64(ins_block.get(),1,static_cast<sqlite3_int64>(run.lcn+c));sqlite3_bind_int64(ins_block.get(),2,static_cast<sqlite3_int64>(target++));if(sqlite3_step(ins_block.get())!=SQLITE_DONE)return sql_error(*db,error,"recording NTFS cluster permutation");}}if(target!=placement->start+placement->clusters){ntfs_set_error(error,"NTFS stream cluster count changed during planning");return -1;}}
    for(std::size_t i=0;i<catalogue->count;++i){NtfsStream *stream=&catalogue->items[i];if(stream->movable||stream->record_number<NTFS_FIRST_USER_RECORD||stream->clusters==0U||stream->directory||!primary_object_stream(stream))continue;if(stream->runs.count!=1U||stream->runs.items[0].sparse){ntfs_set_error(error,"NTFS fixed primary stream in MFT record %llu changed shape before plan persistence",static_cast<unsigned long long>(stream->record_number));return -1;}std::uint64_t reserve=growth?(stream->clusters*10U+99U)/100U:0U;std::uint64_t start_cluster=stream->runs.items[0].lcn;std::uint8_t digest[SHA256_DIGEST_LENGTH];if(ntfs_stream_digest(volume,stream,digest,error)!=0)return -1;sqlite3_reset(ins_fixed.get());sqlite3_clear_bindings(ins_fixed.get());sqlite3_bind_int64(ins_fixed.get(),1,static_cast<sqlite3_int64>(stream->record_number));sqlite3_bind_int(ins_fixed.get(),2,static_cast<int>(stream->attribute_offset));sqlite3_bind_int64(ins_fixed.get(),3,static_cast<sqlite3_int64>(stream->clusters));sqlite3_bind_int64(ins_fixed.get(),4,static_cast<sqlite3_int64>(start_cluster));sqlite3_bind_int64(ins_fixed.get(),5,static_cast<sqlite3_int64>(reserve));sqlite3_bind_blob(ins_fixed.get(),6,digest,SHA256_DIGEST_LENGTH,SQLITE_TRANSIENT);if(sqlite3_step(ins_fixed.get())!=SQLITE_DONE)return sql_error(*db,error,"recording NTFS fixed primary stream");}
    if(layout->bitmap_bytes>static_cast<std::size_t>(INT_MAX)){ntfs_set_error(error,"NTFS final bitmap is too large for SQLite blob storage");return -1;}sqlite3_reset(ins_meta.get());sqlite3_clear_bindings(ins_meta.get());sqlite3_bind_text(ins_meta.get(),1,"bitmap",-1,SQLITE_STATIC);sqlite3_bind_blob(ins_meta.get(),2,layout->bitmap,static_cast<int>(layout->bitmap_bytes),SQLITE_TRANSIENT);if(sqlite3_step(ins_meta.get())!=SQLITE_DONE)return sql_error(*db,error,"recording NTFS final bitmap");if(sql_exec(*db,"COMMIT",error)!=0)return -1;rollback.release();return 0;
}

extern "C" int ntfs_open_plan_db(const char *path,sqlite3 **db,char **error){int flags=SQLITE_OPEN_READWRITE;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags|=SQLITE_OPEN_NOFOLLOW;
#endif
    if(sqlite3_open_v2(path,db,flags,nullptr)!=SQLITE_OK) {
        return sql_error(*db,error,"opening NTFS plan database");
    }
    return 0;
}
extern "C" std::uint64_t ntfs_plan_move_count(sqlite3 *db,char **error){SqliteStatement statement;if(prepare(db,statement,"SELECT COUNT(*) FROM blocks WHERE old<>target",error,"reading NTFS move count")!=0)return UINT64_MAX;if(sqlite3_step(statement.get())!=SQLITE_ROW){ntfs_set_error(error,"reading NTFS move count returned no row");return UINT64_MAX;}return static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(),0));}
