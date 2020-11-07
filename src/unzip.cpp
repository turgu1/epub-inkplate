#define __UNZIP__ 1
#include "unzip.hpp"
#include "logging.hpp"
#include "alloc.hpp"

#include "stb_image.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

#define ZLIB 0

#if ZLIB
  #include "zlib.h" // ToDo: Migrate to stb
#endif

static const char * TAG = "Unzip";

Unzip::Unzip()
{
  zip_file_is_open = false; 
}

bool 
Unzip::open_zip_file(const char * zip_filename)
{
  // Open zip file
  if (zip_file_is_open) close_zip_file();
  if ((fd = open(zip_filename, O_RDONLY)) == -1) {
    LOG_E(TAG, "Uzip: Unable to open file: %s", zip_filename);
    return false;
  }
  zip_file_is_open = true;

  int err = 0;

  #define ERR(e) { err = e; break; }

  bool completed = false;
  while (true) {
    // Seek to beginning of central directory
    //
    // We seek the file back until we reach the "End Of Central Directory"
    // signature "PK\5\6".
    //
    // end of central dir signature    4 bytes  (0x06054b50)
    // number of this disk             2 bytes   4
    // number of the disk with the
    // start of the central directory  2 bytes   6
    // total number of entries in the
    // central directory on this disk  2 bytes   8
    // total number of entries in
    // the central directory           2 bytes  10
    // size of the central directory   4 bytes  12
    // offset of start of central
    // directory with respect to
    // the starting disk number        4 bytes  16
    // .ZIP file comment length        2 bytes  20
    // --- SIZE UNTIL HERE: UNZIP_EOCD_SIZE ---
    // .ZIP file comment       (variable size)

    const int FILE_CENTRAL_SIZE = 22;

    buffer[FILE_CENTRAL_SIZE] = 0;

    off_t length = lseek(fd, 0, SEEK_END);
    if (length < FILE_CENTRAL_SIZE) ERR(1); 
    off_t offset = length - FILE_CENTRAL_SIZE;

    if (lseek(fd, offset, SEEK_SET) != offset) ERR(2);
    if (read(fd, buffer, FILE_CENTRAL_SIZE) != FILE_CENTRAL_SIZE) ERR(3);
    if (!((buffer[0] == 'P') && (buffer[1] == 'K') && (buffer[2] == 5) && (buffer[3] == 6))) {
      // There must be a comment in the last entry. Search for the beginning of the entry
      offset -= FILE_CENTRAL_SIZE;
      bool found = false;
      while (!found && (offset > 0)) {
        if (lseek(fd, offset, SEEK_SET) != offset) ERR(4);
        if (read(fd, buffer, FILE_CENTRAL_SIZE) != FILE_CENTRAL_SIZE) ERR(5);
        char * p;
        if ((p = strstr(buffer, "PK\5\6")) != nullptr) {
          offset += (p - buffer);
          if (lseek(fd, offset, SEEK_SET) != offset) ERR(6);
          if (read(fd, buffer, FILE_CENTRAL_SIZE) != FILE_CENTRAL_SIZE) ERR(7);
          found = true;
          break;
        }
        offset -= FILE_CENTRAL_SIZE;
      }
      if (!found) offset = 0;
    }

    if (offset > 0) {
      offset         = getuint32((const unsigned char *) &buffer[16]);
      uint16_t count = getuint16((const unsigned char *) &buffer[10]);
  
      // Central Directory record structure:

      // [file header 1]
      // .
      // .
      // .
      // [file header n]
      // [digital signature] // PKZip 6.2 or later only

      // File header:

      // central file header signature   4 bytes  (0x02014b50)
      // version made by                 2 bytes   0
      // version needed to extract       2 bytes   2
      // general purpose bit flag        2 bytes   4
      // compression method              2 bytes   6
      // last mod file time              2 bytes   8
      // last mod file date              2 bytes  10
      // crc-32                          4 bytes  12
      // compressed size                 4 bytes  16
      // uncompressed size               4 bytes  20
      // file name length                2 bytes  24
      // extra field length              2 bytes  26
      // file comment length             2 bytes  28
      // disk number start               2 bytes  30
      // internal file attributes        2 bytes  32
      // external file attributes        4 bytes  34
      // relative offset of local header 4 bytes  38

      // file name (variable size)
      // extra field (variable size)
      // file comment (variable size)

      const int FILE_ENTRY_SIZE   = 42;

      if (count == 0) ERR(8);

      if (lseek(fd, offset, SEEK_SET) != offset) ERR(9);
      
      file_entries.reserve(count);

      while (true) {
        if (read(fd, buffer, 4) != 4) ERR(10);
        if (!((buffer[0] == 'P') && (buffer[1] == 'K') && (buffer[2] == 1) && (buffer[3] == 2))) {
          // End of list...
          completed = true;
          break;
        }
        if (read(fd, buffer, FILE_ENTRY_SIZE) != FILE_ENTRY_SIZE) ERR(11);

        uint16_t filename_size = getuint16((const unsigned char *) &buffer[24]);
        uint16_t extra_size    = getuint16((const unsigned char *) &buffer[26]);
        uint16_t comment_size  = getuint16((const unsigned char *) &buffer[28]);
        
        char * fname = new char[filename_size + 1];
        if (read(fd, fname, filename_size) != filename_size) {
          delete [] fname;
          break;
        }
        fname[filename_size] = 0;

        FileEntry fe;

        fe.filename        = fname;
        fe.start_pos       = getuint32((const unsigned char *) &buffer[38]);
        fe.compressed_size = getuint32((const unsigned char *) &buffer[16]);
        fe.size            = getuint32((const unsigned char *) &buffer[20]);
        fe.method          = getuint16((const unsigned char *) &buffer[ 6]);

        //LOG_D(TAG, "File: %s %d %d %d %d", fe.filename, fe.start_pos, fe.compressed_size, fe.size, fe.method);
        file_entries.push_back(fe);

        offset += FILE_ENTRY_SIZE + 4 + filename_size + extra_size + comment_size;
        if (lseek(fd, extra_size + comment_size, SEEK_CUR) != offset) ERR(12);
      }
    }
    break;
  }

  if (!completed) {
    LOG_E(TAG, "Unzip: open_file error: %d", err);
    close_zip_file();
  }
  else {
    // LOG_D(TAG, "Unzip: open_file completed!");
  }
  return completed;
}

void 
Unzip::close_zip_file()
{
  if (zip_file_is_open) {
    file_entries.clear();
    close(fd);
    zip_file_is_open = false;
  }

  // LOG_D(TAG, "Zip file closed.");
}

std::string 
clean_fname(const char * filename)
{
  std::string str = "";
  std::string str2 = filename;
  const char * s;
  while ((s = strstr(str2.c_str(), "/../")) != nullptr) {
    const char *ss = s-1;
    while ((ss != str2.c_str()) && (*ss != '/')) ss--;
    if (ss != str2.c_str()) {
      const char * t = str2.c_str();
      while (t != (ss + 1)) str.push_back(*t++);
    }
    str.append(s + 4);
    str2 = str;
    str = "";
  }
  return str2;
}

char * 
Unzip::get_file(const char * filename, int & file_size)
{
  char * data = nullptr;
  int err = 0;
  file_size = 0;
  
  if (!zip_file_is_open) return nullptr;

  std::string the_filename = clean_fname(filename);

  std::vector<FileEntry>::iterator fe = file_entries.begin();

  while (fe != file_entries.end()) {
    if (strcmp(fe->filename, the_filename.c_str()) == 0) break;
    fe++;
  }


  if (fe == file_entries.end()) {
    LOG_E(TAG, "Unzip Get: File not found: %s", the_filename.c_str());
    return nullptr;
  }
  else {
    //LOG_D(TAG, "File: %s at pos: %d", fe->filename, fe->start_pos);
  }

  bool completed = false;
  while (true) {

    // Local header record.

    // local file header signature     4 bytes  (0x04034b50)
    // version needed to extract       2 bytes   0
    // general purpose bit flag        2 bytes   2
    // compression method              2 bytes   4
    // last mod file time              2 bytes   6
    // last mod file date              2 bytes   8
    // crc-32                          4 bytes  10
    // compressed size                 4 bytes  14
    // uncompressed size               4 bytes  18
    // file name length                2 bytes  22
    // extra field length              2 bytes  24

    // file name (variable size)
    // extra field (variable size)
    
    const int LOCAL_HEADER_SIZE = 26;

    if (lseek(fd, fe->start_pos, SEEK_SET) != fe->start_pos) ERR(20);
    if (read(fd, buffer, 4) != 4) ERR(21);
    if (!((buffer[0] == 'P') && (buffer[1] == 'K') && (buffer[2] == 3) && (buffer[3] == 4))) ERR(22);

    if (read(fd, buffer, LOCAL_HEADER_SIZE) != LOCAL_HEADER_SIZE) ERR(23);

    uint16_t filename_size = getuint16((const unsigned char *) &buffer[22]);
    uint16_t extra_size    = getuint16((const unsigned char *) &buffer[24]);

    // bool has_data_descriptor = ((buffer[2] & 8) != 0);
    // if (has_data_descriptor) {
    //   LOG_D(TAG, "Unzip: with data descriptor...");
    // }

    if (lseek(fd, filename_size + extra_size, SEEK_CUR) != (fe->start_pos + 4 + LOCAL_HEADER_SIZE + filename_size + extra_size)) ERR(24);
    // LOG_D(TAG, "Unzip Get Method: ", fe->method);
    
    data = (char *) allocate(fe->size + 1);

    if (data == nullptr) ERR(25);
    data[fe->size] = 0;

    if (fe->method == 0) {
      if (read(fd, data, fe->size) != fe->size) ERR(26);
      //LOG_E(TAG, "Unzip: read %d bytes at pos %d", fe->size, (fe->start_pos + 4 + LOCAL_HEADER_SIZE + filename_size + extra_size));
    }
    else if (fe->method == 8) {

      #if ZLIB
        if (result != fe->size) ERR(29);

        uint16_t rep   = fe->compressed_size / BUFFER_SIZE;
        uint16_t rem   = fe->compressed_size % BUFFER_SIZE;
        uint16_t cur   = 0;
        uint32_t total = 0;

        /* Allocate inflate state */
        z_stream zstr;

        zstr.zalloc    = nullptr;
        zstr.zfree     = nullptr;
        zstr.opaque    = nullptr;
        zstr.next_in   = nullptr;
        zstr.avail_in  = 0;
        zstr.avail_out = fe->size;
        zstr.next_out  = (Bytef *) data;

        int zret;

        // Use inflateInit2 with negative windowBits to get raw decompression
        if ((zret = inflateInit2_(&zstr, -MAX_WBITS, ZLIB_VERSION, sizeof(z_stream))) != Z_OK) goto error;

        // int szDecomp;

        // Decompress until deflate stream ends or end of file
        do
        {
          uint16_t size = cur < rep ? BUFFER_SIZE : rem;
          if (read(fd, buffer, size) != size) {
            inflateEnd(&zstr);
            goto error;
          }

          cur++;
          total += size;

          zstr.avail_in = size;
          zstr.next_in = (Bytef *) buffer;

          zret = inflate(&zstr, Z_NO_FLUSH);

          switch (zret) {
            case Z_NEED_DICT:
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
              inflateEnd(&zstr);
              goto error;
            default:
              ;
          }
        } while (zret != Z_STREAM_END);

        inflateEnd(&zstr);
      #else
        char * compressed_data = (char *) allocate(fe->compressed_size + 2);
        if (compressed_data == nullptr) ERR(27);
        if (read(fd, compressed_data, fe->compressed_size) != (fe->compressed_size)) ERR(28);
        compressed_data[fe->compressed_size] = 0;
        compressed_data[fe->compressed_size + 1] = 0;

        int32_t result = stbi_zlib_decode_noheader_buffer(data, fe->size, compressed_data, fe->compressed_size + 2);
        if (result != fe->size) {
          ERR(29);
        }
        // std::cout << "[FILE CONTENT:]" << std::endl << data << std::endl << "[END]" << std::endl;

        free(compressed_data);
      #endif
    }
    else break;

    completed = true;
    break;
  }
// error:
  if (!completed) {
    free(data);
    file_size = 0;
    LOG_E(TAG, "Unzip get: Error!: %d", err);
  }
  else {
    file_size = fe->size;
  }
  return data;
}