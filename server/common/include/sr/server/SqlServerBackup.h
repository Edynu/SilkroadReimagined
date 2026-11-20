#pragma once

// A SQL Server backup (.bak) read directly, without SQL Server: the vSRO 1.188 server's SRO_VT_SHARD.bak, for SroSetup
// (docs/SERVER_SETUP.md). Only what an uncompressed full backup of the original's databases holds is read:
//
// - The pages: a full backup carries the database's 8 KB pages as they are. Each is found by its header (version 1, a
//   known page type) at a 512-byte step; the backup's own blocks put them at a few fixed places modulo 8 KB, so only
//   places many pages share are kept (a stray match inside a page is not), and a page copied twice is taken from the
//   later copy. A page written with torn-page detection has the low two bits of the last byte of each 512-byte sector
//   (1..15) replaced by a marker; the header keeps the originals (m_tornBits), and they are put back.
// - Allocation: a page counts only when its PFS byte says allocated (0x40), so a freed page's old rows never come back.
// - The catalog, from the system tables' own pages: sysschobjs (tables), sysrowsets, sysallocunits (a table's
//   allocation units and their first IAM page), syscolpars (columns: type, size, NULL, identity, default),
//   sysrscols (where a column is in a record), sysobjvalues (defaults' text), sysidxstats and sysiscols (primary keys).
// - A table's rows: the pages of its in-row allocation unit, from its IAM chain (single pages and 8-page extents; pages
//   SQL Server 2000 wrote carry index 0 in their header even in a clustered index, so the header cannot say), their
//   records in FixedVar form (primary and forwarded; stubs and ghosts left out).
//
// Text: varchar and char from their collation's code page (BackupColumn::collation), nvarchar and nchar from UTF-16;
// trailing empty variable columns are left out of a record (empty, unless its null bit says NULL).
//
// Measured against an export of the same backups made through SQL Server 2022 (194,447 rows in 210 tables: identical
// but for one SQL Server diagram table's large values). Not read: compressed backups, compressed pages, large values
// (text, image, varchar(max) out of the row); a table holding one says so.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

struct BackupColumn {
    std::string name;
    std::string type;         // SQL Server's name: int, varchar, datetime, ...
    int length = 0;           // bytes (syscolpars.length; -1: max)
    int precision = 0, scale = 0;
    bool nullable = true, identity = false;
    int keyOrdinal = 0;       // its place in the primary key, 0: not in it
    std::string defaultValue; // as SQL Server keeps it: (0), ('xxx'), (getdate())
    // Where its value is in a record (sysrscols).
    int typeId = 0;           // syscolpars.xtype
    int leafOffset = 0;       // > 0: its fixed place; < 0: -n, the n-th variable column
    int nullBit = 0;          // its bit in the null bitmap, from 1
    int bitPosition = 0;      // a bit column's bit
    // Text's code page from its collation (syscolpars.collationid): a SQL collation (its top byte, the sort id, set:
    // SQL_Latin1_General_CP1 is 52) is Windows-1252; a Windows one, in the original's databases, Korean (949). Measured
    // on the 1.188 shard: 0x3400D008 cp1252 (19 of 19 values the export's), 0xD011 cp949 (1,651 of 1,651).
    int collation = 0;
};

struct BackupTable {
    std::string owner; // dbo
    std::string name;
    std::vector<BackupColumn> columns;
    std::uint64_t allocationUnit = 0; // the in-row data's
};

class SqlServerBackup {
public:
    SqlServerBackup();
    ~SqlServerBackup();
    SqlServerBackup(SqlServerBackup&&) noexcept;
    SqlServerBackup& operator=(SqlServerBackup&&) noexcept;

    // Reads the file and its catalog; false with `error` when it is not a backup this reads.
    bool Open(const std::filesystem::path& file, std::string& error);

    const std::vector<BackupTable>& Tables() const;
    const BackupTable* Find(std::string_view name) const; // by name, any case
    std::size_t Pages() const;

    // Each row of `table` as one line of COPY's text form (cells joined by tabs, \N for NULL, backslash escapes; text as
    // UTF-8 from CP949 or UTF-16; bytea as \\x..; timestamps as YYYY-MM-DD HH:MM:SS.mmm), without the newline. False
    // with `error` when a value cannot be read (a type it does not read, a large value out of the row).
    bool ReadRows(const BackupTable& table, const std::function<void(std::string&& line)>& row, std::string& error) const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
