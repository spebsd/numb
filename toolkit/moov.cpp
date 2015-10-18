/*******************************************************************************
 moov.c (version 2)

 moov - A library for splitting Quicktime/MPEG4 files.
 http://h264.code-shop.com

 Copyright (C) 2007-2009 CodeShop B.V.

 Licensing
 The H264 Streaming Module is licened under a Creative Common License. It allows
 you to use, modify and redistribute the module, but only for *noncommercial*
 purposes. For corporate use, please apply for a commercial license.

 Creative Commons License:
 http://creativecommons.org/licenses/by-nc-sa/3.0/

 Commercial License:
 http://h264.code-shop.com/trac/wiki/Mod-H264-Streaming-License-Version2
******************************************************************************/ 

#include "moov.h"

#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) x
#endif

/* 
  The QuickTime File Format PDF from Apple:
    http://developer.apple.com/techpubs/quicktime/qtdevdocs/PDF/QTFileFormat.pdf
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <inttypes.h>
#define __STDC_LIMIT_MACROS
#include <stdint.h>

#define INT8_MAX 0x7f
#define INT8_MIN (-INT8_MAX - 1)
#define UINT8_MAX (__CONCAT(INT8_MAX, U) * 2U + 1U)
#define INT16_MAX 0x7fff
#define INT16_MIN (-INT16_MAX - 1)
#define UINT16_MAX (__CONCAT(INT16_MAX, U) * 2U + 1U)
#define INT32_MAX 0x7fffffffL
#define INT32_MIN (-INT32_MAX - 1L)
#define UINT32_MAX (__CONCAT(INT32_MAX, U) * 2UL + 1UL)
#define INT64_MAX 0x7fffffffffffffffLL
#define INT64_MIN (-INT64_MAX - 1LL)
#define UINT64_MAX (__CONCAT(INT64_MAX, U) * 2ULL + 1ULL) 

#define MAX_TRACKS 8

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24) + \
                           ((uint32_t)(b) << 16) + \
                           ((uint32_t)(c) << 8) + \
                           ((uint32_t)(d))


extern "C" {

/* Returns true when the test string is a prefix of the input */
int starts_with(const char* input, const char* test)
{
  while(*input && *test)
  {
    if(*input != *test)
      return 0;
    ++input;
    ++test;
  }

  return *test == '\0';
}

static unsigned int read_8(unsigned char const* buffer)
{
  return buffer[0];
}

static unsigned char* write_8(unsigned char* buffer, unsigned char v)
{
  buffer[0] = v;

  return buffer + 1;
}

static uint16_t read_16(unsigned char const* buffer)
{
  return (buffer[0] << 8) |
         (buffer[1] << 0);
}

static unsigned char* write_16(unsigned char* buffer, unsigned int v)
{
  buffer[0] = (unsigned char)(v >> 8);
  buffer[1] = (unsigned char)(v >> 0);

  return buffer + 2;
}

static unsigned int read_24(unsigned char const* buffer)
{
  return (buffer[0] << 16) |
         (buffer[1] << 8) |
         (buffer[2] << 0);
}

static unsigned char* write_24(unsigned char* buffer, unsigned int v)
{
  buffer[0] = (unsigned char)(v >> 16);
  buffer[1] = (unsigned char)(v >> 8);
  buffer[2] = (unsigned char)(v >> 0);

  return buffer + 3;
}

static uint32_t read_32(unsigned char const* buffer)
{
  return (buffer[0] << 24) |
         (buffer[1] << 16) |
         (buffer[2] << 8) |
         (buffer[3] << 0);
}

static unsigned char* write_32(unsigned char* buffer, uint32_t v)
{
  buffer[0] = (unsigned char)(v >> 24);
  buffer[1] = (unsigned char)(v >> 16);
  buffer[2] = (unsigned char)(v >> 8);
  buffer[3] = (unsigned char)(v >> 0);

  return buffer + 4;
}

static uint64_t read_64(unsigned char const* buffer)
{
  return ((uint64_t)(read_32(buffer)) << 32) + read_32(buffer + 4);
}

static unsigned char* write_64(unsigned char* buffer, uint64_t v)
{
  write_32(buffer + 0, (uint32_t)(v >> 32));
  write_32(buffer + 4, (uint32_t)(v >> 0));

  return buffer + 8;
}

#define ATOM_PREAMBLE_SIZE 8

struct atom_t
{
 uint32_t type_;
 uint32_t short_size_;
 uint64_t size_;
 unsigned char* start_;
 unsigned char* end_;
};

static unsigned char* atom_read_header(unsigned char* buffer, struct atom_t* atom)
{
  atom->start_ = buffer;
  atom->short_size_ = read_32(buffer);
  atom->type_ = read_32(buffer + 4);

  if(atom->short_size_ == 1)
    atom->size_ = read_64(buffer + 8);
  else
    atom->size_ = atom->short_size_;

  atom->end_ = atom->start_ + atom->size_;

  return buffer + ATOM_PREAMBLE_SIZE + (atom->short_size_ == 1 ? 8 : 0);
}

static void atom_print(struct atom_t const* atom)
{
#ifdef DEBUGMOOV
  systemLog->sysLog(DEBUG, "Atom(%c%c%c%c,%lld)\n",
         atom->type_ >> 24,
         atom->type_ >> 16,
         atom->type_ >> 8,
         atom->type_,
         atom->size_);
#endif
}

struct unknown_atom_t
{
  void* atom_;
  struct unknown_atom_t* next_;
};

static struct unknown_atom_t* unknown_atom_init()
{
  struct unknown_atom_t* atom = (struct unknown_atom_t*)malloc(sizeof(struct unknown_atom_t));
  atom->atom_ = 0;
  atom->next_ = 0;

  return atom;
}

static void unknown_atom_exit(struct unknown_atom_t* atom)
{
  while(atom)
  {
    struct unknown_atom_t* next = atom->next_;
    free(atom->atom_);
    free(atom);
    atom = next;
  }
}

static struct unknown_atom_t* unknown_atom_add_atom(struct unknown_atom_t* parent, void* atom)
{
  size_t size = read_32((const unsigned char *)atom);
  struct unknown_atom_t* unknown = unknown_atom_init();
  unknown->atom_ = malloc(size);
  memcpy(unknown->atom_, atom, size);
  unknown->next_ = parent;
  return unknown;
}

struct atom_read_list_t
{
  uint32_t type_;
  void* parent_;
  int (*destination_)(void* parent, void* child);
  void* (*reader_)(void* parent, unsigned char* buffer, uint64_t size);
};

static int atom_reader(struct atom_read_list_t* atom_read_list,
                       unsigned int atom_read_list_size,
                       void* parent,
                       unsigned char* buffer, uint64_t size)
{
  struct atom_t leaf_atom;
  unsigned char* buffer_start = buffer;

  while(buffer < buffer_start + size)
  {
    unsigned int i;
    buffer = atom_read_header(buffer, &leaf_atom);

    atom_print(&leaf_atom);

    for(i = 0; i != atom_read_list_size; ++i)
    {
      if(leaf_atom.type_ == atom_read_list[i].type_)
      {
        break;
      }
    }

    if(i == atom_read_list_size)
    {
      // add to unkown chunks
      (*(struct unknown_atom_t**)parent) =
        unknown_atom_add_atom(*(struct unknown_atom_t**)(parent), buffer - ATOM_PREAMBLE_SIZE);
    }
    else
    {
      void* child =
        atom_read_list[i].reader_(parent, buffer,
          leaf_atom.size_ - ATOM_PREAMBLE_SIZE);
      if(!child)
        break;
      if(!atom_read_list[i].destination_(parent, child))
        break;
    }
    buffer = leaf_atom.end_;
  }

  if(buffer < buffer_start + size)
  {
    return 0;
  }

  return 1;
}

struct atom_write_list_t
{
  uint32_t type_;
  void* parent_;
  void* source_;
  unsigned char* (*writer_)(void* parent, void* atom, unsigned char* buffer);
};

static unsigned char* atom_writer_unknown(struct unknown_atom_t* atoms,
                                          unsigned char* buffer)
{
  while(atoms)
  {
    size_t size = read_32((const unsigned char *)atoms->atom_);
    memcpy(buffer, atoms->atom_, size);
    buffer += size;
    atoms = atoms->next_;
  }

  return buffer;
}

static unsigned char* atom_writer(struct unknown_atom_t* unknown_atoms,
                                  struct atom_write_list_t* atom_write_list,
                                  unsigned int atom_write_list_size,
                                  unsigned char* buffer)
{
  unsigned i;
  const int write_box64 = 0;

  if(unknown_atoms)
  {
    buffer = atom_writer_unknown(unknown_atoms, buffer);
  }

  for(i = 0; i != atom_write_list_size; ++i)
  {
    if(atom_write_list[i].source_ != 0)
    {
      unsigned char* atom_start = buffer;
      // atom size
      if(write_box64)
      {
        write_32(buffer, 1); // box64
      }
      buffer += 4;

      // atom type
      buffer = write_32(buffer, atom_write_list[i].type_);
      if(write_box64)
      {
        buffer += 8; // box64
      }

      // atom payload
      buffer = atom_write_list[i].writer_(atom_write_list[i].parent_,
                                          atom_write_list[i].source_, buffer);

      if(write_box64)
        write_64(atom_start + 8, buffer - atom_start);
      else
        write_32(atom_start, buffer - atom_start);
    }
  }

  return buffer;
}

struct tkhd_t
{
  unsigned int version_;
  unsigned int flags_;
  uint64_t creation_time_;
  uint64_t modification_time_;
  uint32_t track_id_;
  uint32_t reserved_;
  uint64_t duration_;
  uint32_t reserved2_[2];
  uint16_t layer_;
  uint16_t predefined_;
  uint16_t volume_;
  uint16_t reserved3_;
  uint32_t matrix_[9];
  uint32_t width_;
  uint32_t height_;
};

struct mdhd_t
{
  unsigned int version_;
  unsigned int flags_;
  uint64_t creation_time_;
  uint64_t modification_time_;
  uint32_t timescale_;
  uint64_t duration_;
  unsigned int language_[3];
  uint16_t predefined_;
};

struct vmhd_t
{
  unsigned int version_;
  unsigned int flags_;
  uint16_t graphics_mode_;
  uint16_t opcolor_[3];
};

struct hdlr_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t predefined_;
  uint32_t handler_type_;
  uint32_t reserved1_;
  uint32_t reserved2_;
  uint32_t reserved3_;
  char* name_;
};

struct stbl_t
{
  struct unknown_atom_t* unknown_atoms_;
//struct stsd_t* stsd_;     // sample description
  struct stts_t* stts_;     // decoding time-to-sample
  struct stss_t* stss_;     // sync sample
  struct stsc_t* stsc_;     // sample-to-chunk
  struct stsz_t* stsz_;     // sample size
  struct stco_t* stco_;     // chunk offset
  struct ctts_t* ctts_;     // composition time-to-sample

  void* stco_inplace_;      // newly generated stco (patched inplace)
};

struct stts_table_t
{
  uint32_t sample_count_;
  uint32_t sample_duration_;
};

struct stts_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t entries_;
  struct stts_table_t* table_;
};

struct stss_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t entries_;
  uint32_t* sample_numbers_;
};

struct stsc_table_t
{
  uint32_t chunk_;
  uint32_t samples_;
  uint32_t id_;
};

struct stsc_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t entries_;
  struct stsc_table_t* table_;
};

struct stsz_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t sample_size_;
  uint32_t entries_;
  uint32_t* sample_sizes_;
};

struct stco_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t entries_;
  uint64_t* chunk_offsets_;
};

struct ctts_table_t
{
  uint32_t sample_count_;
  uint32_t sample_offset_;
};

struct ctts_t
{
  unsigned int version_;
  unsigned int flags_;
  uint32_t entries_;
  struct ctts_table_t* table_;
};

struct minf_t
{
  struct unknown_atom_t* unknown_atoms_;
  struct vmhd_t* vmhd_;
//  struct dinf_t* dinf_;
  struct stbl_t* stbl_;
};

struct mdia_t
{
  struct unknown_atom_t* unknown_atoms_;
  struct mdhd_t* mdhd_;
  struct hdlr_t* hdlr_;
  struct minf_t* minf_;
};

struct chunks_t
{
  unsigned int sample_;   // number of the first sample in the chunk
  unsigned int size_;     // number of samples in the chunk
  int id_;                // for multiple codecs mode - not used
  uint64_t pos_;          // start byte position of chunk
};

struct samples_t
{
  unsigned int pts_;      // decoding/presentation time
  unsigned int size_;     // size in bytes
  uint64_t pos_;          // byte offset
  unsigned int cto_;      // composition time offset
};

struct trak_t
{
  struct unknown_atom_t* unknown_atoms_;
  struct tkhd_t* tkhd_;
  struct mdia_t* mdia_;

  /* temporary indices */
  unsigned int chunks_size_;
  struct chunks_t* chunks_;

  unsigned int samples_size_;
  struct samples_t* samples_;
};

struct mvhd_t
{
  unsigned int version_;
  unsigned int flags_;
  uint64_t creation_time_;
  uint64_t modification_time_;
  uint32_t timescale_;
  uint64_t duration_;
  uint32_t rate_;
  uint16_t volume_;
  uint16_t reserved1_;
  uint32_t reserved2_[2];
  uint32_t matrix_[9];
  uint32_t predefined_[6];
  uint32_t next_track_id_;
};

struct moov_t
{
  struct unknown_atom_t* unknown_atoms_;
  struct mvhd_t* mvhd_;
  unsigned int tracks_;
  struct trak_t* traks_[MAX_TRACKS];
};


static struct tkhd_t* tkhd_init()
{
  struct tkhd_t* tkhd = (struct tkhd_t*)malloc(sizeof(struct tkhd_t));

  return tkhd;
}

static void tkhd_exit(struct tkhd_t* tkhd)
{
  free(tkhd);
}

static void* tkhd_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct tkhd_t* tkhd = tkhd_init();

  tkhd->version_ = read_8(buffer + 0);
  tkhd->flags_ = read_24(buffer + 1);
  if(tkhd->version_ == 0)
  {
    if(size < 92-8)
      return 0;

    tkhd->creation_time_ = read_32(buffer + 4);
    tkhd->modification_time_ = read_32(buffer + 8);
    tkhd->track_id_ = read_32(buffer + 12);
    tkhd->reserved_ = read_32(buffer + 16);
    tkhd->duration_ = read_32(buffer + 20);
    buffer += 24;
  }
  else
  {
    if(size < 104-8)
      return 0;

    tkhd->creation_time_ = read_64(buffer + 4);
    tkhd->modification_time_ = read_64(buffer + 12);
    tkhd->track_id_ = read_32(buffer + 20);
    tkhd->reserved_ = read_32(buffer + 24);
    tkhd->duration_ = read_64(buffer + 28);
    buffer += 36;
  }

  tkhd->reserved2_[0] = read_32(buffer + 0);
  tkhd->reserved2_[1] = read_32(buffer + 4);
  tkhd->layer_ = read_16(buffer + 8);
  tkhd->predefined_ = read_16(buffer + 10);
  tkhd->volume_ = read_16(buffer + 12);
  tkhd->reserved3_ = read_16(buffer + 14);
  buffer += 16;

  for(i = 0; i != 9; ++i)
  {
    tkhd->matrix_[i] = read_32(buffer);
    buffer += 4;
  }

  tkhd->width_ = read_32(buffer + 0);
  tkhd->height_ = read_32(buffer + 4);

  return tkhd;
}

static unsigned char* tkhd_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct tkhd_t const* tkhd = (const tkhd_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, tkhd->version_);
  buffer = write_24(buffer, tkhd->flags_);

  if(tkhd->version_ == 0)
  {
    buffer = write_32(buffer, (uint32_t)tkhd->creation_time_);
    buffer = write_32(buffer, (uint32_t)tkhd->modification_time_);
    buffer = write_32(buffer, tkhd->track_id_);
    buffer = write_32(buffer, tkhd->reserved_);
    buffer = write_32(buffer, (uint32_t)tkhd->duration_);
  }
  else
  {
    buffer = write_64(buffer, tkhd->creation_time_);
    buffer = write_64(buffer, tkhd->modification_time_);
    buffer = write_32(buffer, tkhd->track_id_);
    buffer = write_32(buffer, tkhd->reserved_);
    buffer = write_64(buffer, tkhd->duration_);
  }

  buffer = write_32(buffer, tkhd->reserved2_[0]);
  buffer = write_32(buffer, tkhd->reserved2_[1]);
  buffer = write_16(buffer, tkhd->layer_);
  buffer = write_16(buffer, tkhd->predefined_);
  buffer = write_16(buffer, tkhd->volume_);
  buffer = write_16(buffer, tkhd->reserved3_);

  for(i = 0; i != 9; ++i)
  {
    buffer = write_32(buffer, tkhd->matrix_[i]);
  }

  buffer = write_32(buffer, tkhd->width_);
  buffer = write_32(buffer, tkhd->height_);

  return buffer;
}

static struct mdhd_t* mdhd_init()
{
  struct mdhd_t* mdhd = (struct mdhd_t*)malloc(sizeof(struct mdhd_t));

  return mdhd;
}

static void mdhd_exit(struct mdhd_t* mdhd)
{
  free(mdhd);
}

static void* mdhd_read(void* UNUSED(parent), unsigned char* buffer, uint64_t UNUSED(size))
{
  uint16_t language;
  unsigned int i;

  struct mdhd_t* mdhd = mdhd_init();
  mdhd->version_ = read_8(buffer + 0);
  mdhd->flags_ = read_24(buffer + 1);
  if(mdhd->version_ == 0)
  {
    mdhd->creation_time_ = read_32(buffer + 4);
    mdhd->modification_time_ = read_32(buffer + 8);
    mdhd->timescale_ = read_32(buffer + 12);
    mdhd->duration_ = read_32(buffer + 16);
    buffer += 20;
  }
  else
  {
    mdhd->creation_time_ = read_64(buffer + 4);
    mdhd->modification_time_ = read_64(buffer + 12);
    mdhd->timescale_ = read_32(buffer + 20);
    mdhd->duration_ = read_64(buffer + 24);
    buffer += 32;
  }

  language = read_16(buffer + 0);
  for(i = 0; i != 3; ++i)
  {
    mdhd->language_[i] = ((language >> ((2 - i) * 5)) & 0x1f) + 0x60;
  }

  mdhd->predefined_ = read_16(buffer + 2);

  return mdhd;
}

static unsigned char* mdhd_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct mdhd_t const* mdhd = (const mdhd_t *)atom;

  buffer = write_8(buffer, mdhd->version_);
  buffer = write_24(buffer, mdhd->flags_);

  if(mdhd->version_ == 0)
  {
    buffer = write_32(buffer, (uint32_t)mdhd->creation_time_);
    buffer = write_32(buffer, (uint32_t)mdhd->modification_time_);
    buffer = write_32(buffer, mdhd->timescale_);
    buffer = write_32(buffer, (uint32_t)mdhd->duration_);
  }
  else
  {
    buffer = write_64(buffer, mdhd->creation_time_);
    buffer = write_64(buffer, mdhd->modification_time_);
    buffer = write_32(buffer, mdhd->timescale_);
    buffer = write_64(buffer, mdhd->duration_);
  }

  buffer = write_16(buffer,
                    ((mdhd->language_[0] - 0x60) << 10) +
                    ((mdhd->language_[1] - 0x60) << 5) +
                    ((mdhd->language_[2] - 0x60) << 0));

  buffer = write_16(buffer, mdhd->predefined_);

  return buffer;
}

static struct vmhd_t* vmhd_init()
{
  struct vmhd_t* atom = (struct vmhd_t*)malloc(sizeof(struct vmhd_t));

  return atom;
}

void vmhd_exit(struct vmhd_t* atom)
{
  free(atom);
}

static void* vmhd_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct vmhd_t* atom;

  if(size < 20-8)
    return 0;

  atom = vmhd_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);

  atom->graphics_mode_ = read_16(buffer + 4);
  buffer += 6;
  for(i = 0; i != 3; ++i)
  {
    atom->opcolor_[i] = read_16(buffer);
    buffer += 2;
  }

  return atom;
}

static unsigned char* vmhd_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct vmhd_t const* vmhd = (const vmhd_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, vmhd->version_);
  buffer = write_24(buffer, vmhd->flags_);
  buffer = write_16(buffer, vmhd->graphics_mode_);
  for(i = 0; i != 3; ++i)
  {
    buffer = write_16(buffer, vmhd->opcolor_[i]);
  }

  return buffer;
}

static struct hdlr_t* hdlr_init()
{
  struct hdlr_t* atom = (struct hdlr_t*)malloc(sizeof(struct hdlr_t));
  atom->name_ = 0;

  return atom;
}

static void hdlr_exit(struct hdlr_t* atom)
{
  if(atom->name_)
  {
    free(atom->name_);
  }
  free(atom);
}

static void* hdlr_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  struct hdlr_t* atom;

  if(size < 8)
    return 0;

  atom = hdlr_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->predefined_ = read_32(buffer + 4);
  atom->handler_type_ = read_32(buffer + 8);
  atom->reserved1_ = read_32(buffer + 12);
  atom->reserved2_ = read_32(buffer + 16);
  atom->reserved3_ = read_32(buffer + 20);
  buffer += 24;
  size -= 24;
  if(size > 0)
  {
    size_t length = (size_t)size;
    atom->name_ = (char *)malloc(length + 1);
    if(atom->predefined_ == FOURCC('m', 'h', 'l', 'r'))
    {
      length = read_8(buffer);
      buffer += 1;
      if(size < length)
        length = (size_t)size;
    }
    memcpy(atom->name_, buffer, length);
    atom->name_[length] = '\0';
  }

  return atom;
}

static unsigned char* hdlr_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct hdlr_t* hdlr = (hdlr_t *)atom;
  buffer = write_8(buffer, hdlr->version_);
  buffer = write_24(buffer, hdlr->flags_);

  buffer = write_32(buffer, hdlr->predefined_);
  buffer = write_32(buffer, hdlr->handler_type_);
  buffer = write_32(buffer, hdlr->reserved1_);
  buffer = write_32(buffer, hdlr->reserved2_);
  buffer = write_32(buffer, hdlr->reserved3_);
  if(hdlr->name_)
  {
    char const* p;
    if(hdlr->predefined_ == FOURCC('m', 'h', 'l', 'r'))
    {
      buffer = write_8(buffer, strlen(hdlr->name_));
    }

    for(p = hdlr->name_; *p; ++p)
      buffer = write_8(buffer, *p);
  }

  return buffer;
}

static struct stts_t* stts_init()
{
  struct stts_t* atom = (struct stts_t*)malloc(sizeof(struct stts_t));
  atom->table_ = 0;

  return atom;
}

void stts_exit(struct stts_t* atom)
{
  if(atom->table_)
  {
    free(atom->table_);
  }
  free(atom);
}

static void* stts_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct stts_t* atom;

  if(size < 8)
    return 0;

  atom = stts_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->entries_ = read_32(buffer + 4);

  if(size < 8 + atom->entries_ * sizeof(struct stts_table_t))
    return 0;

  buffer += 8;

  atom->table_ = (struct stts_table_t*)(malloc(atom->entries_ * sizeof(struct stts_table_t)));

  for(i = 0; i != atom->entries_; ++i)
  {
    atom->table_[i].sample_count_ = read_32(buffer + 0);
    atom->table_[i].sample_duration_ = read_32(buffer + 4);
    buffer += 8;
  }

  return atom;
}

static unsigned char* stts_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct stts_t* stts = (stts_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, stts->version_);
  buffer = write_24(buffer, stts->flags_);
  buffer = write_32(buffer, stts->entries_);
  for(i = 0; i != stts->entries_; ++i)
  {
    buffer = write_32(buffer, stts->table_[i].sample_count_);
    buffer = write_32(buffer, stts->table_[i].sample_duration_);
  }

  return buffer;
}

static unsigned int stts_get_sample(struct stts_t const* stts, uint64_t time)
{
  unsigned int stts_index = 0;
  unsigned int stts_count;

  unsigned int ret = 0;
  uint64_t time_count = 0;

  for(; stts_index != stts->entries_; ++stts_index)
  {
    unsigned int sample_count = stts->table_[stts_index].sample_count_;
    unsigned int sample_duration = stts->table_[stts_index].sample_duration_;
    if(time_count + (uint64_t)sample_duration * (uint64_t)sample_count >= time)
    {
      stts_count = (unsigned int)((time - time_count) / sample_duration);
      time_count += (uint64_t)stts_count * (uint64_t)sample_duration;
      ret += stts_count;
      break;
    }
    else
    {
      time_count += (uint64_t)sample_duration * (uint64_t)sample_count;
      ret += sample_count;
    }
  }
  return ret;
}

static uint64_t stts_get_time(struct stts_t const* stts, unsigned int sample)
{
  uint64_t ret = 0;
  unsigned int stts_index = 0;
  unsigned int sample_count = 0;
  
  for(;;)
  {
    unsigned int table_sample_count = stts->table_[stts_index].sample_count_;
    unsigned int table_sample_duration = stts->table_[stts_index].sample_duration_;
    if(sample_count + table_sample_count > sample)
    {
      unsigned int stts_count = (sample - sample_count);
      ret += (uint64_t)stts_count * (uint64_t)table_sample_duration;
      break;
    }
    else
    {
      sample_count += table_sample_count;
      ret += (uint64_t)table_sample_count * (uint64_t)table_sample_duration;
      stts_index++;
    }
  }
  return ret;
}

static uint64_t stts_get_duration(struct stts_t const* stts)
{
  uint64_t duration = 0;
  unsigned int i;
  for(i = 0; i != stts->entries_; ++i)
  {
    unsigned int sample_count = stts->table_[i].sample_count_;
    unsigned int sample_duration = stts->table_[i].sample_duration_;
    duration += (uint64_t)sample_duration * (uint64_t)sample_count;
  }

  return duration;
}

static unsigned int stts_get_samples(struct stts_t const* stts)
{
  unsigned int samples = 0;
  unsigned int entries = stts->entries_;
  unsigned int i;
  for(i = 0; i != entries; ++i)
  {
    unsigned int sample_count = stts->table_[i].sample_count_;
//  unsigned int sample_duration = stts->table_[i].sample_duration_;
    samples += sample_count;
  }

  return samples;
}

static struct stss_t* stss_init()
{
  struct stss_t* atom = (struct stss_t*)malloc(sizeof(struct stss_t));
  atom->sample_numbers_ = 0;

  return atom;
}

void stss_exit(struct stss_t* atom)
{
  if(atom->sample_numbers_)
  {
    free(atom->sample_numbers_);
  }
  free(atom);
}

static void* stss_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct stss_t* atom;

  if(size < 8)
    return 0;

  atom = stss_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->entries_ = read_32(buffer + 4);

  if(size < 8 + atom->entries_ * sizeof(uint32_t))
    return 0;

  buffer += 8;

  atom->sample_numbers_ = (uint32_t*)malloc(atom->entries_ * sizeof(uint32_t));

  for(i = 0; i != atom->entries_; ++i)
  {
    atom->sample_numbers_[i] = read_32(buffer);
    buffer += 4;
  }

  return atom;
}

static unsigned char* stss_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct stss_t const* stss = (const stss_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, stss->version_);
  buffer = write_24(buffer, stss->flags_);
  buffer = write_32(buffer, stss->entries_);
  for(i = 0; i != stss->entries_; ++i)
  {
    buffer = write_32(buffer, stss->sample_numbers_[i]);
  }

  return buffer;
}

static unsigned int stss_get_nearest_keyframe(struct stss_t const* stss, unsigned int sample)
{
  // scan the sync samples to find the key frame that precedes the sample number
  unsigned int i;
  unsigned int table_sample = 0;
  for(i = 0; i != stss->entries_; ++i)
  {
    table_sample = stss->sample_numbers_[i];
    if(table_sample >= sample)
      break;
  }
  if(table_sample == sample)
    return table_sample;
  else
    return stss->sample_numbers_[i - 1];
}

static struct stsc_t* stsc_init()
{
  struct stsc_t* atom = (struct stsc_t*)malloc(sizeof(struct stsc_t));
  atom->table_ = 0;

  return atom;
}

static void stsc_exit(struct stsc_t* atom)
{
  if(atom->table_)
  {
    free(atom->table_);
  }
  free(atom);
}

static void* stsc_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct stsc_t* atom;

  if(size < 8)
    return 0;

  atom = stsc_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->entries_ = read_32(buffer + 4);

  if(size < 8 + atom->entries_ * sizeof(struct stsc_table_t))
    return 0;

  buffer += 8;

  // reserve space for one extra entry as when splitting the video we may have to
  // split the first entry
  atom->table_ = (struct stsc_table_t*)(malloc((atom->entries_ + 1) * sizeof(struct stsc_table_t)));

  for(i = 0; i != atom->entries_; ++i)
  {
    atom->table_[i].chunk_ = read_32(buffer + 0) - 1; // Note: we use zero based
    atom->table_[i].samples_ = read_32(buffer + 4);
    atom->table_[i].id_ = read_32(buffer + 8);
    buffer += 12;
  }

  return atom;
}

static unsigned char* stsc_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct stsc_t* stsc = (stsc_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, stsc->version_);
  buffer = write_24(buffer, stsc->flags_);
  buffer = write_32(buffer, stsc->entries_);
  for(i = 0; i != stsc->entries_; ++i)
  {
    buffer = write_32(buffer, stsc->table_[i].chunk_ + 1);
    buffer = write_32(buffer, stsc->table_[i].samples_);
    buffer = write_32(buffer, stsc->table_[i].id_);
  }

  return buffer;
}

static struct stsz_t* stsz_init()
{
  struct stsz_t* atom = (struct stsz_t*)malloc(sizeof(struct stsz_t));
  atom->sample_sizes_ = 0;

  return atom;
}

static void stsz_exit(struct stsz_t* atom)
{
  if(atom->sample_sizes_)
  {
    free(atom->sample_sizes_);
  }
  free(atom);
}

static void* stsz_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct stsz_t* atom;

  if(size < 12)
  {
#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "Error: not enough bytes for stsz atom\n");
#endif
    return 0;
  }

  atom = stsz_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->sample_size_ = read_32(buffer + 4);
  atom->entries_ = read_32(buffer + 8);
  buffer += 12;

  // fix for clayton.mp4, it mistakenly says there is 1 entry
  if(atom->sample_size_ && atom->entries_)
    atom->entries_ = 0;

  if(size < 12 + atom->entries_ * sizeof(uint32_t))
  {
#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "Error: stsz.entries don't match with size\n");
#endif
    stsz_exit(atom);
    return 0;
  }

  if(!atom->sample_size_)
  {
    atom->sample_sizes_ = (uint32_t*)malloc(atom->entries_ * sizeof(uint32_t));
    for(i = 0; i != atom->entries_; ++i)
    {
      atom->sample_sizes_[i] = read_32(buffer);
      buffer += 4;
    }
  }

  return atom;
}

static unsigned char* stsz_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct stsz_t* stsz = (stsz_t *)atom;
  unsigned int i;
  unsigned int entries = stsz->sample_size_ ? 0 : stsz->entries_;

  buffer = write_8(buffer, stsz->version_);
  buffer = write_24(buffer, stsz->flags_);
  buffer = write_32(buffer, stsz->sample_size_);
  buffer = write_32(buffer, entries);
  for(i = 0; i != entries; ++i)
  {
    buffer = write_32(buffer, stsz->sample_sizes_[i]);
  }

  return buffer;
}

static struct stco_t* stco_init()
{
  struct stco_t* atom = (struct stco_t*)malloc(sizeof(struct stco_t));
  atom->chunk_offsets_ = 0;

  return atom;
}

static void stco_exit(struct stco_t* atom)
{
  if(atom->chunk_offsets_)
  {
    free(atom->chunk_offsets_);
  }
  free(atom);
}

static void* stco_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct stco_t* atom;

  if(size < 8)
    return 0;

  atom = stco_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->entries_ = read_32(buffer + 4);
  buffer += 8;

  if(size < 8 + atom->entries_ * sizeof(uint32_t))
    return 0;

  atom->chunk_offsets_ = (uint64_t*)malloc(atom->entries_ * sizeof(uint64_t));
  for(i = 0; i != atom->entries_; ++i)
  {
    atom->chunk_offsets_[i] = read_32(buffer);
    buffer += 4;
  }

  return atom;
}

static void* co64_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct stco_t* atom;

  if(size < 8)
    return 0;

  atom = stco_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->entries_ = read_32(buffer + 4);
  buffer += 8;

  if(size < 8 + atom->entries_ * sizeof(uint64_t))
    return 0;

  atom->chunk_offsets_ = (uint64_t*)malloc(atom->entries_ * sizeof(uint64_t));
  for(i = 0; i != atom->entries_; ++i)
  {
    atom->chunk_offsets_[i] = read_64(buffer);
    buffer += 8;
  }

  return atom;
}

static unsigned char* stco_write(void* parent, void* atom, unsigned char* buffer)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  struct stco_t* stco = (stco_t *)atom;
  unsigned int i;

  stbl->stco_inplace_ = buffer;      // newly generated stco (patched inplace)

  buffer = write_8(buffer, stco->version_);
  buffer = write_24(buffer, stco->flags_);
  buffer = write_32(buffer, stco->entries_);
  for(i = 0; i != stco->entries_; ++i)
  {
    buffer = write_32(buffer, (uint32_t)(stco->chunk_offsets_[i]));
  }

  return buffer;
}

static void stco_shift_offsets(struct stco_t* stco, int offset)
{
  unsigned int i;
  for(i = 0; i != stco->entries_; ++i)
    stco->chunk_offsets_[i] += offset;
}

static void stco_shift_offsets_inplace(unsigned char* stco, int offset)
{
  unsigned int entries = read_32(stco + 4);
  unsigned int* table = (unsigned int*)(stco + 8);
  unsigned int i;
  for(i = 0; i != entries; ++i)
    write_32((unsigned char*)&table[i], (read_32((unsigned char*)&table[i]) + offset));
}

static struct ctts_t* ctts_init()
{
  struct ctts_t* atom = (struct ctts_t*)malloc(sizeof(struct ctts_t));
  atom->table_ = 0;

  return atom;
}

static void ctts_exit(struct ctts_t* atom)
{
  if(atom->table_)
  {
    free(atom->table_);
  }
  free(atom);
}

static void* ctts_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct ctts_t* atom;

  if(size < 8)
    return 0;

  atom = ctts_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  atom->entries_ = read_32(buffer + 4);

  if(size < 8 + atom->entries_ * sizeof(struct ctts_table_t))
    return 0;

  buffer += 8;

  atom->table_ = (struct ctts_table_t*)(malloc(atom->entries_ * sizeof(struct ctts_table_t)));

  for(i = 0; i != atom->entries_; ++i)
  {
    atom->table_[i].sample_count_ = read_32(buffer + 0);
    atom->table_[i].sample_offset_ = read_32(buffer + 4);
    buffer += 8;
  }

  return atom;
}

static unsigned char* ctts_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct ctts_t const* ctts = (const ctts_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, ctts->version_);
  buffer = write_24(buffer, ctts->flags_);
  buffer = write_32(buffer, ctts->entries_);
  for(i = 0; i != ctts->entries_; ++i)
  {
    buffer = write_32(buffer, (uint32_t)(ctts->table_[i].sample_count_));
    buffer = write_32(buffer, (uint32_t)(ctts->table_[i].sample_offset_));
  }

  return buffer;
}

static unsigned int ctts_get_samples(struct ctts_t const* ctts)
{
  unsigned int samples = 0;
  unsigned int entries = ctts->entries_;
  unsigned int i;
  for(i = 0; i != entries; ++i)
  {
    unsigned int sample_count = ctts->table_[i].sample_count_;
//  unsigned int sample_offset = ctts->table_[i].sample_offset_;
    samples += sample_count;
  }

  return samples;
}

static struct stbl_t* stbl_init()
{
  struct stbl_t* atom = (struct stbl_t*)malloc(sizeof(struct stbl_t));
  atom->unknown_atoms_ = 0;
  atom->stts_ = 0;
  atom->stss_ = 0;
  atom->stsc_ = 0;
  atom->stsz_ = 0;
  atom->stco_ = 0;
  atom->ctts_ = 0;

  return atom;
}

static void stbl_exit(struct stbl_t* atom)
{
  if(atom->unknown_atoms_)
  {
    unknown_atom_exit(atom->unknown_atoms_);
  }
  if(atom->stts_)
  {
    stts_exit(atom->stts_);
  }
  if(atom->stss_)
  {
    stss_exit(atom->stss_);
  }
  if(atom->stsc_)
  {
    stsc_exit(atom->stsc_);
  }
  if(atom->stsz_)
  {
    stsz_exit(atom->stsz_);
  }
  if(atom->stco_)
  {
    stco_exit(atom->stco_);
  }
  if(atom->ctts_)
  {
    ctts_exit(atom->ctts_);
  }

  free(atom);
}

static int stbl_add_stts(void* parent, void* child)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  stbl->stts_ = (stts_t *)child;

  return 1;
}

static int stbl_add_stss(void* parent, void* child)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  stbl->stss_ = (stss_t *)child;

  return 1;
}

static int stbl_add_stsc(void* parent, void* child)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  stbl->stsc_ = (stsc_t *)child;

  return 1;
}

static int stbl_add_stsz(void* parent, void* child)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  stbl->stsz_ = (stsz_t *)child;

  return 1;
}

static int stbl_add_stco(void* parent, void* child)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  stbl->stco_ = (stco_t *)child;

  return 1;
}

static int stbl_add_ctts(void* parent, void* child)
{
  struct stbl_t* stbl = (stbl_t *)parent;
  stbl->ctts_ = (ctts_t *)child;

  return 1;
}

static void* stbl_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  struct stbl_t* atom = stbl_init();

  struct atom_read_list_t atom_read_list[] = {
    { FOURCC('s', 't', 't', 's'), atom, &stbl_add_stts, &stts_read },
    { FOURCC('s', 't', 's', 's'), atom, &stbl_add_stss, &stss_read },
    { FOURCC('s', 't', 's', 'c'), atom, &stbl_add_stsc, &stsc_read },
    { FOURCC('s', 't', 's', 'z'), atom, &stbl_add_stsz, &stsz_read },
    { FOURCC('s', 't', 'c', 'o'), atom, &stbl_add_stco, &stco_read },
    { FOURCC('c', 'o', '6', '4'), atom, &stbl_add_stco, &co64_read },
    { FOURCC('c', 't', 't', 's'), atom, &stbl_add_ctts, &ctts_read },
  };

  int result = atom_reader(atom_read_list,
                  sizeof(atom_read_list) / sizeof(atom_read_list[0]),
                  atom,
                  buffer, size);

  // check for mandatory atoms
  if(!atom->stts_)
  {
    systemLog->sysLog(ERROR, "stbl: missing stts\n");
    result = 0;
  }

  if(!atom->stsc_)
  {
    systemLog->sysLog(ERROR, "stbl: missing stsc\n");
    result = 0;
  }

  if(!atom->stsz_)
  {
    systemLog->sysLog(ERROR, "stbl: missing stsz\n");
    result = 0;
  }

  if(!atom->stco_)
  {
    systemLog->sysLog(ERROR, "stbl: missing stco\n");
    result = 0;
  }

  if(!result)
  {
    stbl_exit(atom);
    return 0;
  }

  return atom;
}

static unsigned char* stbl_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct stbl_t* stbl = (stbl_t *)atom;
  struct atom_write_list_t atom_write_list[] = {
    { FOURCC('s', 't', 't', 's'), stbl, stbl->stts_, &stts_write },
    { FOURCC('s', 't', 's', 's'), stbl, stbl->stss_, &stss_write },
    { FOURCC('s', 't', 's', 'c'), stbl, stbl->stsc_, &stsc_write },
    { FOURCC('s', 't', 's', 'z'), stbl, stbl->stsz_, &stsz_write },
    { FOURCC('s', 't', 'c', 'o'), stbl, stbl->stco_, &stco_write },
    { FOURCC('c', 't', 't', 's'), stbl, stbl->ctts_, &ctts_write },
  };

  buffer = atom_writer(stbl->unknown_atoms_,
                       atom_write_list,
                       sizeof(atom_write_list) / sizeof(atom_write_list[0]),
                       buffer);

  return buffer;
}

static unsigned int stbl_get_nearest_keyframe(struct stbl_t const* stbl, unsigned int sample)
{
  // If the sync atom is not present, all samples are implicit sync samples.
  if(!stbl->stss_)
    return sample;

  return stss_get_nearest_keyframe(stbl->stss_, sample);
}

static struct minf_t* minf_init()
{
  struct minf_t* atom = (struct minf_t*)malloc(sizeof(struct minf_t));
  atom->unknown_atoms_ = 0;
  atom->vmhd_ = 0;
  atom->stbl_ = 0;

  return atom;
}

static void minf_exit(struct minf_t* atom)
{
  if(atom->unknown_atoms_)
  {
    unknown_atom_exit(atom->unknown_atoms_);
  }
  if(atom->vmhd_)
  {
    vmhd_exit(atom->vmhd_);
  }
  if(atom->stbl_)
  {
    stbl_exit(atom->stbl_);
  }
  free(atom);
}

static int minf_add_vmhd(void* parent, void* child)
{
  struct minf_t* minf = (minf_t *)parent;
  minf->vmhd_ = (vmhd_t *)child;

  return 1;
}

static int minf_add_stbl(void* parent, void* child)
{
  struct minf_t* minf = (minf_t *)parent;
  minf->stbl_ = (stbl_t *)child;

  return 1;
}

static void* minf_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  struct minf_t* atom = minf_init();

  struct atom_read_list_t atom_read_list[] = {
    { FOURCC('v', 'm', 'h', 'd'), atom, &minf_add_vmhd, &vmhd_read },
    { FOURCC('s', 't', 'b', 'l'), atom, &minf_add_stbl, &stbl_read }
  };

  int result = atom_reader(atom_read_list,
                  sizeof(atom_read_list) / sizeof(atom_read_list[0]),
                  atom,
                  buffer, size);

  // check for mandatory atoms
  if(!atom->stbl_)
  {
    systemLog->sysLog(ERROR, "minf: missing stbl\n");
    result = 0;
  }

  if(!result)
  {
    minf_exit(atom);
    return 0;
  }

  return atom;
}

static unsigned char* minf_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct minf_t const* minf = (const minf_t *)atom;
  struct atom_write_list_t atom_write_list[] = {
    { FOURCC('v', 'm', 'h', 'd'), atom, minf->vmhd_, &vmhd_write },
    { FOURCC('s', 't', 'b', 'l'), atom, minf->stbl_, &stbl_write }
  };

  buffer = atom_writer(minf->unknown_atoms_,
                       atom_write_list,
                       sizeof(atom_write_list) / sizeof(atom_write_list[0]),
                       buffer);

  return buffer;
}

static struct mdia_t* mdia_init()
{
  struct mdia_t* atom = (struct mdia_t*)malloc(sizeof(struct mdia_t));
  atom->unknown_atoms_ = 0;
  atom->mdhd_ = 0;
  atom->hdlr_ = 0;
  atom->minf_ = 0;

  return atom;
}

static void mdia_exit(struct mdia_t* atom)
{
  if(atom->unknown_atoms_)
  {
    unknown_atom_exit(atom->unknown_atoms_);
  }
  if(atom->mdhd_)
  {
    mdhd_exit(atom->mdhd_);
  }
  if(atom->hdlr_)
  {
    hdlr_exit(atom->hdlr_);
  }
  if(atom->minf_)
  {
    minf_exit(atom->minf_);
  }
  free(atom);
}

static int mdia_add_mdhd(void* parent, void* child)
{
  struct mdia_t* mdia = (mdia_t *)parent;
  mdia->mdhd_ = (mdhd_t *)child;

  return 1;
}

static int mdia_add_hdlr(void* parent, void* child)
{
  struct mdia_t* mdia = (mdia_t *)parent;
  mdia->hdlr_ = (hdlr_t *)child;

  return 1;
}

static int mdia_add_minf(void* parent, void* child)
{
  struct mdia_t* mdia = (mdia_t *)parent;
  mdia->minf_ = (minf_t *)child;

  return 1;
}

static void* mdia_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  struct mdia_t* atom = mdia_init();

  struct atom_read_list_t atom_read_list[] = {
    { FOURCC('m', 'd', 'h', 'd'), atom, &mdia_add_mdhd, &mdhd_read },
    { FOURCC('h', 'd', 'l', 'r'), atom, &mdia_add_hdlr, &hdlr_read },
    { FOURCC('m', 'i', 'n', 'f'), atom, &mdia_add_minf, &minf_read }
  };

  int result = atom_reader(atom_read_list,
                  sizeof(atom_read_list) / sizeof(atom_read_list[0]),
                  atom,
                  buffer, size);

  // check for mandatory atoms
  if(!atom->mdhd_)
  {
    systemLog->sysLog(ERROR, "mdia: missing mdhd\n");
    result = 0;
  }

  if(!atom->hdlr_)
  {
    systemLog->sysLog(ERROR, "mdia: missing hdlr\n");
    result = 0;
  }

  if(!atom->minf_)
  {
    systemLog->sysLog(ERROR, "mdia: missing minf\n");
    result = 0;
  }

  if(!result)
  {
    mdia_exit(atom);
    return 0;
  }

  return atom;
}

static unsigned char* mdia_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct mdia_t const* mdia = (const mdia_t *)atom;
  struct atom_write_list_t atom_write_list[] = {
    { FOURCC('m', 'd', 'h', 'd'), atom, mdia->mdhd_, &mdhd_write },
    { FOURCC('h', 'd', 'l', 'r'), atom, mdia->hdlr_, &hdlr_write },
    { FOURCC('m', 'i', 'n', 'f'), atom, mdia->minf_, &minf_write }
  };

  buffer = atom_writer(mdia->unknown_atoms_,
                       atom_write_list,
                       sizeof(atom_write_list) / sizeof(atom_write_list[0]),
                       buffer);

  return buffer;
}

void trak_build_index(struct trak_t* trak)
{
  struct stco_t const* stco = trak->mdia_->minf_->stbl_->stco_;

  trak->chunks_size_ = stco->entries_;
  trak->chunks_ = (chunks_t *)malloc(trak->chunks_size_ * sizeof(struct chunks_t));

  {
    unsigned int i;
    for(i = 0; i != trak->chunks_size_; ++i)
    {
      trak->chunks_[i].pos_ = stco->chunk_offsets_[i];
    }
  }

  // process chunkmap:
  {
    struct stsc_t const* stsc = trak->mdia_->minf_->stbl_->stsc_;
    unsigned int last = trak->chunks_size_;
    unsigned int i = stsc->entries_;
    while(i > 0)
    {
      unsigned int j;

      --i;

      for(j = stsc->table_[i].chunk_; j < last; j++)
      {
        trak->chunks_[j].id_ = stsc->table_[i].id_;
        trak->chunks_[j].size_ = stsc->table_[i].samples_;
      }
      last = stsc->table_[i].chunk_;
    }
  }

  // calc pts of chunks:
  {
    struct stsz_t const* stsz = trak->mdia_->minf_->stbl_->stsz_;
    unsigned int sample_size = stsz->sample_size_;
    unsigned int s = 0;
    {
      unsigned int j;
      for(j = 0; j < trak->chunks_size_; j++)
      {
        trak->chunks_[j].sample_ = s;
        s += trak->chunks_[j].size_;
      }
    }

    if(sample_size == 0)
    {
      trak->samples_size_ = stsz->entries_;
    }
    else
    {
      trak->samples_size_ = s;
    }

    trak->samples_ = (samples_t *)malloc(trak->samples_size_ * sizeof(struct samples_t));

    if(sample_size == 0)
    {
      unsigned int i;
      for(i = 0; i != trak->samples_size_ ; ++i)
        trak->samples_[i].size_ = stsz->sample_sizes_[i];
    }
    else
    {
      unsigned int i;
      for(i = 0; i != trak->samples_size_ ; ++i)
        trak->samples_[i].size_ = sample_size;
    }
  }

//  i = 0;
//  for (j = 0; j < trak->durmap_size; j++)
//    i += trak->durmap[j].num;
//  if (i != s) {
//    mp_msg(MSGT_DEMUX, MSGL_WARN,
//           "MOV: durmap and chunkmap sample count differ (%i vs %i)\n", i, s);
//    if (i > s) s = i;
//  }

  // calc pts:
  {
    struct stts_t const* stts = trak->mdia_->minf_->stbl_->stts_;
    unsigned int s = 0;
    unsigned int pts = 0;
    unsigned int entries = stts->entries_;
    unsigned int j;
    for(j = 0; j < entries; j++)
    {
      unsigned int i;
      unsigned int sample_count = stts->table_[j].sample_count_;
      unsigned int sample_duration = stts->table_[j].sample_duration_;
      for(i = 0; i < sample_count; i++)
      {
        trak->samples_[s].pts_ = pts;
        ++s;
        pts += sample_duration;
      }
    }
  }

  // calc composition times:
  {
    struct ctts_t const* ctts = trak->mdia_->minf_->stbl_->ctts_;
    if(ctts)
    {
      unsigned int s = 0;
      unsigned int entries = ctts->entries_;
      unsigned int j;
      for(j = 0; j != entries; j++)
      {
        unsigned int i;
        unsigned int sample_count = ctts->table_[j].sample_count_;
        unsigned int sample_offset = ctts->table_[j].sample_offset_;
        for(i = 0; i < sample_count; i++)
        {
          trak->samples_[s].cto_ = sample_offset;
          ++s;
        }
      }
    }
  }

  // calc sample offsets
  {
    unsigned int s = 0;
    unsigned int j;
    for(j = 0; j != trak->chunks_size_; j++)
    {
      uint64_t pos = trak->chunks_[j].pos_;
      unsigned int i;
      for(i = 0; i != trak->chunks_[j].size_; i++)
      {
        trak->samples_[s].pos_ = pos;
        pos += trak->samples_[s].size_;
        ++s;
      }
    }
  }
}

void trak_update_index(struct trak_t* trak, unsigned int start, unsigned int end)
{
  // write samples [start,end>

  // stts = [entries * [sample_count, sample_duration]
  {
    struct stts_t* stts = trak->mdia_->minf_->stbl_->stts_;

    unsigned int entries = 0;
    unsigned int s;

    for(s = start; s != end; ++s)
    {
      unsigned int sample_count = 1;
      unsigned int sample_duration =
        trak->samples_[s + 1].pts_ - trak->samples_[s].pts_;
      while(s != end - 1)
      {
        if((trak->samples_[s + 1].pts_ - trak->samples_[s].pts_) != sample_duration)
          break;
        ++sample_count;
        ++s;
      }
      stts->table_[entries].sample_count_ = sample_count;
      stts->table_[entries].sample_duration_ = sample_duration;
      ++entries;
    }
    stts->entries_ = entries;

    if(stts_get_samples(stts) != end - start)
    {
      systemLog->sysLog(ERROR, "ERROR: stts_get_samples=%d, should be %d\n",
             stts_get_samples(stts), end - start);
    }
  }

  // ctts = [entries * [sample_count, sample_offset]
  {
    struct ctts_t* ctts = trak->mdia_->minf_->stbl_->ctts_;
    if(ctts)
    {
      unsigned int entries = 0;
      unsigned int s;

      for(s = start; s != end; ++s)
      {
        unsigned int sample_count = 1;
        unsigned int sample_offset = trak->samples_[s].cto_;
        while(s != end - 1)
        {
          if(trak->samples_[s + 1].cto_ != sample_offset)
            break;
          ++sample_count;
          ++s;
        }
        // write entry
        ctts->table_[entries].sample_count_ = sample_count;
        ctts->table_[entries].sample_offset_ = sample_offset;
        ++entries;
      }
      ctts->entries_ = entries;
      if(ctts_get_samples(ctts) != end - start)
      {
        systemLog->sysLog(ERROR, "ERROR: ctts_get_samples=%d, should be %d\n",
               ctts_get_samples(ctts), end - start);
      }
    }
  }

  // process chunkmap:
  {
    struct stsc_t* stsc = trak->mdia_->minf_->stbl_->stsc_;
    unsigned int i;

    for(i = 0; i != trak->chunks_size_; ++i)
    {
      if(trak->chunks_[i].sample_ + trak->chunks_[i].size_ > start)
        break;
    }

    {
      unsigned int stsc_entries = 0;
      unsigned int chunk_start = i;
      unsigned int chunk_end;
      // problem.mp4: reported by Jin-seok Lee. Second track contains no samples
      if(trak->chunks_size_ != 0)
      {
        unsigned int samples =
          trak->chunks_[i].sample_ + trak->chunks_[i].size_ - start;
        unsigned int id = trak->chunks_[i].id_;

        // write entry [chunk,samples,id]
        stsc->table_[stsc_entries].chunk_ = 0;
        stsc->table_[stsc_entries].samples_ = samples;
        stsc->table_[stsc_entries].id_ = id;
        ++stsc_entries;

        if(i != trak->chunks_size_)
        {
          for(i += 1; i != trak->chunks_size_; ++i)
          {
            if(trak->chunks_[i].sample_ >= end)
              break;

            if(trak->chunks_[i].size_ != samples)
            {
              samples = trak->chunks_[i].size_;
              id = trak->chunks_[i].id_;

              stsc->table_[stsc_entries].chunk_ = i - chunk_start;
              stsc->table_[stsc_entries].samples_ = samples;
              stsc->table_[stsc_entries].id_ = id;
              ++stsc_entries;
            }
          }
        }
      }
      chunk_end = i;
      stsc->entries_ = stsc_entries;

      {
        struct stco_t* stco = trak->mdia_->minf_->stbl_->stco_;
        unsigned int entries = 0;
        for(i = chunk_start; i != chunk_end; ++i)
        {
          stco->chunk_offsets_[entries] = stco->chunk_offsets_[i];
          ++entries;
        }
        stco->entries_ = entries;

        // patch first chunk with correct sample offset
        stco->chunk_offsets_[0] = (uint32_t)trak->samples_[start].pos_;
      }
    }
  }

  // process sync samples:
  if(trak->mdia_->minf_->stbl_->stss_)
  {
    struct stss_t* stss = trak->mdia_->minf_->stbl_->stss_;
    unsigned int entries = 0;
    unsigned int stss_start;
    unsigned int i;

    for(i = 0; i != stss->entries_; ++i)
    {
      if(stss->sample_numbers_[i] >= start + 1)
        break;
    }
    stss_start = i;
    for(; i != stss->entries_; ++i)
    {
      unsigned int sync_sample = stss->sample_numbers_[i];
      if(sync_sample >= end + 1)
        break;
      stss->sample_numbers_[entries] = sync_sample - start;
      ++entries;
    }
    stss->entries_ = entries;
  }

  // process sample sizes
  {
    struct stsz_t* stsz = trak->mdia_->minf_->stbl_->stsz_;

    if(stsz->sample_size_ == 0)
    {
      unsigned int entries = 0;
      unsigned int i;
      for(i = start; i != end; ++i)
      {
        stsz->sample_sizes_[entries] = stsz->sample_sizes_[i];
        ++entries;
      }
      stsz->entries_ = entries;
    }
  }
}

static struct trak_t* trak_init()
{
  struct trak_t* trak = (struct trak_t*)malloc(sizeof(struct trak_t));
  trak->unknown_atoms_ = 0;
  trak->tkhd_ = 0;
  trak->mdia_ = 0;
  trak->chunks_size_ = 0;
  trak->chunks_ = 0;
  trak->samples_size_ = 0;
  trak->samples_ = 0;

  return trak;
}

static void trak_exit(struct trak_t* trak)
{
  if(trak->unknown_atoms_)
  {
    unknown_atom_exit(trak->unknown_atoms_);
  }
  if(trak->tkhd_)
  {
    tkhd_exit(trak->tkhd_);
  }
  if(trak->mdia_)
  {
    mdia_exit(trak->mdia_);
  }
  if(trak->chunks_)
  {
    free(trak->chunks_);
  }
  if(trak->samples_)
  {
    free(trak->samples_);
  }
  free(trak);
}

static int trak_add_tkhd(void* parent, void* tkhd)
{
  struct trak_t* trak = (trak_t *)parent;
  trak->tkhd_ = (tkhd_t *)tkhd;

  return 1;
}

static int trak_add_mdia(void* parent, void* mdia)
{
  struct trak_t* trak = (trak_t *)parent;
  trak->mdia_ = (mdia_t *)mdia;

  return 1;
}

static void* trak_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  struct trak_t* atom = trak_init();

  struct atom_read_list_t atom_read_list[] = {
    { FOURCC('t', 'k', 'h', 'd'), atom, &trak_add_tkhd, &tkhd_read },
    { FOURCC('m', 'd', 'i', 'a'), atom, &trak_add_mdia, &mdia_read }
  };

  int result = atom_reader(atom_read_list,
                  sizeof(atom_read_list) / sizeof(atom_read_list[0]),
                  atom,
                  buffer, size);

  // check for mandatory atoms
  if(!atom->tkhd_)
  {
    systemLog->sysLog(ERROR, "trak: missing tkhd\n");
    result = 0;
  }

  if(!atom->mdia_)
  {
    systemLog->sysLog(ERROR, "trak: missing mdia\n");
    result = 0;
  }

  if(!result)
  {
    trak_exit(atom);
    return 0;
  }

  trak_build_index(atom);

  return atom;
}

static unsigned char* trak_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct trak_t* trak = (trak_t *)atom;
  struct atom_write_list_t atom_write_list[] = {
    { FOURCC('t', 'k', 'h', 'd'), atom, trak->tkhd_, &tkhd_write },
    { FOURCC('m', 'd', 'i', 'a'), atom, trak->mdia_, &mdia_write }
  };

  buffer = atom_writer(trak->unknown_atoms_,
                       atom_write_list,
                       sizeof(atom_write_list) / sizeof(atom_write_list[0]),
                       buffer);

  return buffer;
}

void trak_shift_offsets(struct trak_t* trak, int64_t offset)
{
  struct stco_t* stco = trak->mdia_->minf_->stbl_->stco_;
  stco_shift_offsets(stco, (int32_t)offset);
}

void trak_shift_offsets_inplace(struct trak_t* trak, int64_t offset)
{
  void* stco = trak->mdia_->minf_->stbl_->stco_inplace_;
  stco_shift_offsets_inplace((unsigned char *)stco, (int32_t)offset);
}

static struct mvhd_t* mvhd_init()
{
  struct mvhd_t* atom = (struct mvhd_t*)malloc(sizeof(struct mvhd_t));

  return atom;
}

void mvhd_exit(struct mvhd_t* atom)
{
  free(atom);
}

static void* mvhd_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  unsigned int i;

  struct mvhd_t* atom = mvhd_init();
  atom->version_ = read_8(buffer + 0);
  atom->flags_ = read_24(buffer + 1);
  if(atom->version_ == 0)
  {
    if(size < 108-8)
      return 0;

    atom->creation_time_ = read_32(buffer + 4);
    atom->modification_time_ = read_32(buffer + 8);
    atom->timescale_ = read_32(buffer + 12);
    atom->duration_ = read_32(buffer + 16);
    buffer += 20;
  }
  else
  {
    if(size < 120-8)
      return 0;

    atom->creation_time_ = read_64(buffer + 4);
    atom->modification_time_ = read_64(buffer + 12);
    atom->timescale_ = read_32(buffer + 20);
    atom->duration_ = read_64(buffer + 24);
    buffer += 32;
  }
  atom->rate_ = read_32(buffer + 0);
  atom->volume_ = read_16(buffer + 4);
  atom->reserved1_ = read_16(buffer + 6);
  atom->reserved2_[0] = read_32(buffer + 8);
  atom->reserved2_[1] = read_32(buffer + 12);
  buffer += 16;

  for(i = 0; i != 9; ++i)
  {
    atom->matrix_[i] = read_32(buffer);
    buffer += 4;
  }

  for(i = 0; i != 6; ++i)
  {
    atom->predefined_[i] = read_32(buffer);
    buffer += 4;
  }

  atom->next_track_id_ = read_32(buffer + 0);

  return atom;
}

static unsigned char* mvhd_write(void* UNUSED(parent), void* atom, unsigned char* buffer)
{
  struct mvhd_t const* mvhd = (const mvhd_t *)atom;
  unsigned int i;

  buffer = write_8(buffer, mvhd->version_);
  buffer = write_24(buffer, mvhd->flags_);

  if(mvhd->version_ == 0)
  {
    buffer = write_32(buffer, (uint32_t)mvhd->creation_time_);
    buffer = write_32(buffer, (uint32_t)mvhd->modification_time_);
    buffer = write_32(buffer, mvhd->timescale_);
    buffer = write_32(buffer, (uint32_t)mvhd->duration_);
  }
  else
  {
    buffer = write_64(buffer, mvhd->creation_time_);
    buffer = write_64(buffer, mvhd->modification_time_);
    buffer = write_32(buffer, mvhd->timescale_);
    buffer = write_64(buffer, mvhd->duration_);
  }

  buffer = write_32(buffer, mvhd->rate_);
  buffer = write_16(buffer, mvhd->volume_);
  buffer = write_16(buffer, mvhd->reserved1_);
  buffer = write_32(buffer, mvhd->reserved2_[0]);
  buffer = write_32(buffer, mvhd->reserved2_[1]);

  for(i = 0; i != 9; ++i)
  {
    buffer = write_32(buffer, mvhd->matrix_[i]);
  }

  for(i = 0; i != 6; ++i)
  {
    buffer = write_32(buffer, mvhd->predefined_[i]);
  }

  buffer = write_32(buffer, mvhd->next_track_id_);

  return buffer;
}

static struct moov_t* moov_init()
{
  struct moov_t* moov = (moov_t *)malloc(sizeof(struct moov_t));
  moov->unknown_atoms_ = 0;
  moov->mvhd_ = 0;
  moov->tracks_ = 0;

  return moov;
}

static void moov_exit(struct moov_t* atom)
{
  unsigned int i;
  if(atom->unknown_atoms_)
  {
    unknown_atom_exit(atom->unknown_atoms_);
  }
  if(atom->mvhd_)
  {
    mvhd_exit(atom->mvhd_);
  }
  for(i = 0; i != atom->tracks_; ++i)
  {
    trak_exit(atom->traks_[i]);
  }
  free(atom);
}

static int moov_add_mvhd(void* parent, void* mvhd)
{
  struct moov_t* moov = (moov_t *)parent;
  moov->mvhd_ = (mvhd_t *)mvhd;

  return 1;
}

static int moov_add_trak(void* parent, void* child)
{
  struct moov_t* moov = (moov_t *)parent;
  struct trak_t* trak = (trak_t *)child;
  if(moov->tracks_ == MAX_TRACKS)
  {
    trak_exit(trak);
    return 0;
  }

  if(trak->mdia_->hdlr_->handler_type_ != FOURCC('v', 'i', 'd', 'e') &&
     trak->mdia_->hdlr_->handler_type_ != FOURCC('s', 'o', 'u', 'n'))
  {
    systemLog->sysLog(ERROR, "Trak ignored (handler_type=%c%c%c%c, name=%s)\n",
      trak->mdia_->hdlr_->handler_type_ >> 24,
      trak->mdia_->hdlr_->handler_type_ >> 16,
      trak->mdia_->hdlr_->handler_type_ >> 8,
      trak->mdia_->hdlr_->handler_type_,
      trak->mdia_->hdlr_->name_);
    trak_exit(trak);
    return 1; // continue
  }
  
  moov->traks_[moov->tracks_] = trak;
  ++moov->tracks_;

  return 1;
}

static void* moov_read(void* UNUSED(parent), unsigned char* buffer, uint64_t size)
{
  struct moov_t* atom = moov_init();

  struct atom_read_list_t atom_read_list[] = {
    { FOURCC('m', 'v', 'h', 'd'), atom, &moov_add_mvhd, &mvhd_read },
    { FOURCC('t', 'r', 'a', 'k'), atom, &moov_add_trak, &trak_read }
  };

  int result = atom_reader(atom_read_list,
                  sizeof(atom_read_list) / sizeof(atom_read_list[0]),
                  atom,
                  buffer, size);

  // check for mandatory atoms
  if(!atom->mvhd_)
  {
    systemLog->sysLog(ERROR, "moov: missing mvhd\n");
    result = 0;
  }

  if(!atom->tracks_)
  {
    systemLog->sysLog(ERROR, "moov: missing trak\n");
    result = 0;
  }

  if(!result)
  {
    moov_exit(atom);
    return 0;
  }

  return atom;
}

static void moov_write(struct moov_t* atom, unsigned char* buffer)
{
  unsigned i;

  unsigned char* atom_start = buffer;

  struct atom_write_list_t atom_write_list[] = {
    { FOURCC('m', 'v', 'h', 'd'), atom, atom->mvhd_, &mvhd_write },
  };

  // atom size
  buffer += 4;

  // atom type
  buffer = write_32(buffer, FOURCC('m', 'o', 'o', 'v'));

  buffer = atom_writer(atom->unknown_atoms_,
                       atom_write_list,
                       sizeof(atom_write_list) / sizeof(atom_write_list[0]),
                       buffer);

  for(i = 0; i != atom->tracks_; ++i)
  {
    struct atom_write_list_t trak_atom_write_list[] = {
      { FOURCC('t', 'r', 'a', 'k'), atom, atom->traks_[i], &trak_write },
    };
    buffer = atom_writer(0,
                         trak_atom_write_list,
                         sizeof(trak_atom_write_list) / sizeof(trak_atom_write_list[0]),
                         buffer);
  }
  write_32(atom_start, buffer - atom_start);
}

void moov_shift_offsets(struct moov_t* moov, int64_t offset)
{
  unsigned int i;
  for(i = 0; i != moov->tracks_; ++i)
  {
    trak_shift_offsets(moov->traks_[i], offset);
  }
}

void moov_shift_offsets_inplace(struct moov_t* moov, int64_t offset)
{
  unsigned int i;
  for(i = 0; i != moov->tracks_; ++i)
  {
    trak_shift_offsets_inplace(moov->traks_[i], offset);
  }
}

unsigned int moov_seek(unsigned char* moov_data,
                       uint64_t *moov_size,
                       float start_time,
                       float end_time,
                       uint64_t* mdat_start,
                       uint64_t* mdat_size,
                       uint64_t offset,
                       int client_is_flash)
{
  struct moov_t* moov = (moov_t *)moov_read(NULL, moov_data + ATOM_PREAMBLE_SIZE,
                                  *moov_size - ATOM_PREAMBLE_SIZE);

  if(moov == 0 || moov->mvhd_ == 0)
  {
    systemLog->sysLog(ERROR, "Error parsing moov header\n");
    return 0;
  }

  {
    long moov_time_scale = moov->mvhd_->timescale_;
    unsigned int start = (unsigned int)(start_time * moov_time_scale);
    unsigned int end = (unsigned int)(end_time * moov_time_scale);
    uint64_t skip_from_start = UINT64_MAX;
    uint64_t end_offset = 0;
    unsigned int i;
    unsigned int pass;

    // for every trak, convert seconds to sample (time-to-sample).
    // adjust sample to keyframe
    unsigned int trak_sample_start[MAX_TRACKS];
    unsigned int trak_sample_end[MAX_TRACKS];

    uint64_t moov_duration = 0;

    // clayton.mp4 has a third track with one sample that lasts the whole clip.
    // Assuming the first two tracks are the audio and video track, we patch
    // the remaining tracks to 'free' atoms.
//    if(moov->tracks_ > 2)
//    {
//      for(i = 2; i != moov->tracks_; ++i)
//      {
//        // patch 'trak' to 'free'
//        unsigned char* p = moov->traks_[i].start_ - 4;
//        p[0] = 'f';
//        p[1] = 'r';
//        p[2] = 'e';
//        p[3] = 'e';
//      }
//      moov->tracks_ = 2;
//    }

    // reported by everwanna:
    // av out of sync because: 
    // audio track 0 without stss, seek to the exact time. 
    // video track 1 with stss, seek to the nearest key frame time.
    //
    // fixed:
    // first pass we get the new aligned times for traks with an stss present
    // second pass is for traks without an stss
    for(pass = 0; pass != 2; ++pass)
    {
      for(i = 0; i != moov->tracks_; ++i)
      {
        struct trak_t* trak = moov->traks_[i];
        struct stbl_t* stbl = trak->mdia_->minf_->stbl_;
        long trak_time_scale = trak->mdia_->mdhd_->timescale_;
        float moov_to_trak_time = (float)trak_time_scale / (float)moov_time_scale;
        float trak_to_moov_time = (float)moov_time_scale / (float)trak_time_scale;

        // 1st pass: stss present, 2nd pass: no stss present
        if(pass == 0 && !stbl->stss_)
          continue;
        if(pass == 1 && stbl->stss_)
          continue;

        // ignore empty track
        if(trak->mdia_->mdhd_->duration_ == 0)
          continue;

        // get start
        if(start == 0)
        {
          trak_sample_start[i] = start;
        }
        else
        {
          start = stts_get_sample(stbl->stts_, (uint64_t)(start * moov_to_trak_time));
#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "start=%u (trac time)=%.2f (seconds)", start,
            stts_get_time(stbl->stts_, start) / (float)trak_time_scale);
#endif
          start = stbl_get_nearest_keyframe(stbl, start + 1) - 1;
#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "=%u (zero based keyframe)", start);
#endif
          trak_sample_start[i] = start;
          start = (unsigned int)(stts_get_time(stbl->stts_, start) * trak_to_moov_time);
#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "=%u (moov time)\n", start);
#endif
        }

        // get end
        if(end == 0)
        {
          trak_sample_end[i] = trak->samples_size_;
        }
        else
        {
          end = stts_get_sample(stbl->stts_, (uint64_t)(end * moov_to_trak_time));
          if(end >= trak->samples_size_)
          {
            end = trak->samples_size_;
          }
          else
          {
            end = stbl_get_nearest_keyframe(stbl, end + 1) - 1;
          }
          trak_sample_end[i] = end;
#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "endframe=%u, samples_size_=%u\n", end, trak->samples_size_);
#endif
          end = (unsigned int)(stts_get_time(stbl->stts_, end) * trak_to_moov_time);
        }
      }
    }

#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "start=%u\n", start);
    systemLog->sysLog(DEBUG, "end=%u\n", end);
#endif

    if(end && start >= end)
    {
      moov_exit(moov);
      return 0;
    }

    for(i = 0; i != moov->tracks_; ++i)
    {
      struct trak_t* trak = moov->traks_[i];
      struct stbl_t* stbl = trak->mdia_->minf_->stbl_;

      unsigned int start_sample = trak_sample_start[i];
      unsigned int end_sample = trak_sample_end[i];

      // ignore empty track
      if(trak->mdia_->mdhd_->duration_ == 0)
        continue;

      trak_update_index(trak, start_sample, end_sample);

      {
        uint64_t skip =
          trak->samples_[start_sample].pos_ - trak->samples_[0].pos_;
        if(skip < skip_from_start)
          skip_from_start = skip;
#ifdef DEBUGMOOV
        systemLog->sysLog(DEBUG, "Trak can skip %llu bytes\n", skip);
#endif

        if(end_sample != trak->samples_size_)
        {
          uint64_t end_pos = trak->samples_[end_sample].pos_;
          if(end_pos > end_offset)
            end_offset = end_pos;
#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "New endpos=%llu\n", end_pos);
          systemLog->sysLog(DEBUG, "Trak can skip %llu bytes at end\n",
                 *mdat_start + *mdat_size - end_offset);
#endif
        }
      }

      {
        // fixup trak (duration)
        uint64_t trak_duration = stts_get_duration(stbl->stts_);
        long trak_time_scale = trak->mdia_->mdhd_->timescale_;
        float trak_to_moov_time = (float)moov_time_scale / (float)trak_time_scale;
        {
        uint64_t duration = (long)((float)trak_duration * trak_to_moov_time);
        trak->mdia_->mdhd_->duration_= trak_duration;
        trak->tkhd_->duration_ = duration;
#ifdef DEBUGMOOV
        systemLog->sysLog(DEBUG, "trak: new_duration=%lld\n", duration);
#endif

        if(duration > moov_duration)
          moov_duration = duration;
        }
      }

//      systemLog->sysLog(DEBUG, "stco.size=%d, ", read_int32(stbl->stco_ + 4));
//      systemLog->sysLog(DEBUG, "stts.size=%d samples=%d\n", read_int32(stbl->stts_ + 4), stts_get_samples(stbl->stts_));
//      systemLog->sysLog(DEBUG, "stsz.size=%d\n", read_int32(stbl->stsz_ + 8));
//      systemLog->sysLog(DEBUG, "stsc.samples=%d\n", stsc_get_samples(stbl->stsc_));
    }
    moov->mvhd_->duration_ = moov_duration;

    // subtract bytes we skip at the front of the mdat atom
    offset -= skip_from_start;

    // subtract old moov size
    offset -= *moov_size;

#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "moov: writing header\n");
#endif

    moov_write(moov, moov_data);
    *moov_size = read_32(moov_data);

    // add new moov size
    offset += *moov_size;

#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "shifting offsets by %lld\n", offset);
#endif
    moov_shift_offsets_inplace(moov, offset);

//    moov_write(moov, moov_data);

#ifdef COMPRESS_MOOV_ATOM
    if(!client_is_flash)
    {
      uLong sourceLen = *moov_size - ATOM_PREAMBLE_SIZE;
      uLong destLen = compressBound(sourceLen);
      unsigned char* cmov = malloc(destLen);
      int zstatus = compress(cmov, &destLen, moov_data, sourceLen);
      if(zstatus == Z_OK)
      {
#ifdef DEBUGMOOV
        systemLog->sysLog(DEBUG, "cmov size = %lu (%ld%%)\n", destLen, 100 * destLen / sourceLen);
#endif
      }

      {
        const int extra_space = 4096;
        if(destLen + extra_space < sourceLen)
        {
          const int bytes_saved = sourceLen - destLen;
          uLong destLen2;
          int extra = 0;
#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "shifting offsets by %d\n", -bytes_saved);
#endif
          moov_shift_offsets_inplace(moov, -bytes_saved);

          extra += ATOM_PREAMBLE_SIZE + 4;            // dcom
          extra += ATOM_PREAMBLE_SIZE + 4;            // cmvd
          extra += ATOM_PREAMBLE_SIZE;                // cmov
          extra += ATOM_PREAMBLE_SIZE + extra_space;  // free

#ifdef DEBUGMOOV
          systemLog->sysLog(DEBUG, "shifting offsets by %d\n", extra);
#endif
          moov_shift_offsets_inplace(moov, extra);

          // recompress
          destLen2 = compressBound(sourceLen);
          zstatus = compress(cmov, &destLen2, moov_data, sourceLen);
          if(zstatus == Z_OK)
          {
#ifdef DEBUGMOOV
            systemLog->sysLog(DEBUG, "cmov size = %lu (%ld%%)\n", destLen2, 100 * destLen2 / sourceLen);
#endif

            if(destLen2 < destLen + extra_space)
            {
              // copy compressed movie atom
              unsigned char* outbuffer = moov_data;

              uint32_t dcom_size = ATOM_PREAMBLE_SIZE + 4;
              uint32_t cmvd_size = ATOM_PREAMBLE_SIZE + 4 + destLen2;
              uint32_t cmov_size = ATOM_PREAMBLE_SIZE + dcom_size + cmvd_size;
              uint32_t free_size = ATOM_PREAMBLE_SIZE + extra_space + destLen - destLen2;
              *moov_size = ATOM_PREAMBLE_SIZE + cmov_size + free_size;

              outbuffer = write_32(outbuffer, (uint32_t)*moov_size);

              // skip 'moov'
              outbuffer += 4;

              outbuffer = write_32(outbuffer, cmov_size);
              {
                outbuffer = write_32(outbuffer, FOURCC('c', 'm', 'o', 'v'));
                outbuffer = write_32(outbuffer, dcom_size);
                outbuffer = write_32(outbuffer, FOURCC('d', 'c', 'o', 'm'));
                outbuffer = write_32(outbuffer, FOURCC('z', 'l', 'i', 'b'));

                outbuffer = write_32(outbuffer, cmvd_size);
                {
                  outbuffer = write_32(outbuffer, FOURCC('c', 'm', 'v', 'd'));
                  outbuffer = write_32(outbuffer, sourceLen);
                  memcpy(outbuffer, cmov, destLen2);
                  outbuffer += destLen2;
                }
              }

              // add final padding
              outbuffer = write_32(outbuffer, free_size);
              outbuffer = write_32(outbuffer, FOURCC('f', 'r', 'e', 'e'));
              {
                const char free_bytes[8] =
                {
                  'C', 'o', 'd', 'e','S','h', 'o', 'p'
                };
                uint32_t padding_index;
                for(padding_index = ATOM_PREAMBLE_SIZE; padding_index != free_size; ++padding_index)
                {
                  outbuffer[padding_index] = free_bytes[padding_index % 8];
                }
              }
            }
            else
            {
              systemLog->sysLog(ERROR, "2nd pass compress overflow\n");
            }
          }
        }
      }
      free(cmov);
    }
#endif

    *mdat_start += skip_from_start;
    if(end_offset != 0)
    {
      *mdat_size = end_offset;
    }
    *mdat_size -= skip_from_start;
  }

  moov_exit(moov);

  return 1;
}

////////////////////////////////////////////////////////////////////////////////

struct mp4_atom_t
{
  uint32_t type_;
  uint32_t short_size_;
  uint64_t size_;
  uint64_t start_;
  uint64_t end_;
};

static int mp4_atom_read_header(FILE* infile, struct mp4_atom_t* atom)
{
  unsigned char atom_header[8];

  atom->start_ = ftell(infile);
  fread(atom_header, 8, 1, infile);
  atom->short_size_ = read_32(&atom_header[0]);
  atom->type_ = read_32(&atom_header[4]);

  if(atom->short_size_ == 1)
  {
    fread(atom_header, 8, 1, infile);
    atom->size_ = read_64(&atom_header[0]);
  }
  else
  {
    atom->size_ = atom->short_size_;
  }

  atom->end_ = atom->start_ + atom->size_;

  return 1;
}

static int mp4_atom_write_header(unsigned char* outbuffer, struct mp4_atom_t* atom)
{
  int write_box64 = atom->short_size_ == 1 ? 1 : 0;

  if(write_box64)
    write_32(outbuffer, 1);
  else
    write_32(outbuffer, (uint32_t)atom->size_);

  write_32(outbuffer + 4, atom->type_);

  if(write_box64)
  {
    write_64(outbuffer + 8, atom->size_);
    return 16;
  }
  else
  {
    return 8;
  }
}

int mp4_split(const char* filename, int64_t filesize,
              float start_time, float end_time,
              void** mp4_header, uint32_t* mp4_header_size,
              uint64_t* mdat_offset, uint64_t* mdat_size,
              int client_is_flash)
{
  FILE* infile;
  struct mp4_atom_t ftyp_atom;
  struct mp4_atom_t moov_atom;
  struct mp4_atom_t mdat_atom;
  unsigned char* moov_data = 0;
  unsigned char* buffer;
  uint64_t new_mdat_start;

  *mp4_header = 0;
  memset(&ftyp_atom, 0, sizeof(ftyp_atom));
  memset(&moov_atom, 0, sizeof(moov_atom));
  memset(&mdat_atom, 0, sizeof(mdat_atom));

  infile = fopen(filename, "rb");
  if(infile == NULL)
  {
    return 0;
  }

  while(ftello(infile) < filesize)
  {
    struct mp4_atom_t leaf_atom;

    if(!mp4_atom_read_header(infile, &leaf_atom))
      break;

#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "Atom(%c%c%c%c,%lld)\n",
           leaf_atom.type_ >> 24, leaf_atom.type_ >> 16,
           leaf_atom.type_ >> 8, leaf_atom.type_,
           leaf_atom.size_);
#endif

    switch(leaf_atom.type_)
    {
    case FOURCC('f', 't', 'y', 'p'):
      ftyp_atom = leaf_atom;
      break;
    case FOURCC('m', 'o', 'o', 'v'):
      moov_atom = leaf_atom;
      moov_data = (unsigned char*)malloc((size_t)moov_atom.size_);
      fseeko(infile, moov_atom.start_, SEEK_SET);
      fread(moov_data, (off_t)moov_atom.size_, 1, infile);
      break;
    case FOURCC('m', 'd', 'a', 't'):
      mdat_atom = leaf_atom;
      break;
    }
    fseeko(infile, leaf_atom.end_, SEEK_SET);
  }

  if(moov_atom.size_ == 0)
  {
#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "Error: moov atom not found\n");
#endif
    fclose(infile);
    return 0;
  }

  if(mdat_atom.size_ == 0)
  {
#ifdef DEBUGMOOV
    systemLog->sysLog(DEBUG, "Error: mdat atom not found\n");
#endif
    fclose(infile);
    return 0;
  }

  buffer = (unsigned char*)malloc((uint32_t)moov_atom.size_ + 4 * 1024);
  *mp4_header = buffer;

  if(ftyp_atom.size_)
  {
    fseeko(infile, ftyp_atom.start_, SEEK_SET);
    fread(buffer, (off_t)ftyp_atom.size_, 1, infile);
    buffer += ftyp_atom.size_;
  }

  {
    static char const free_data[] = {
      0x0, 0x0, 0x0,  42, 'f', 'r', 'e', 'e',
      'v', 'i', 'd', 'e', 'o', ' ', 's', 'e',
      'r', 'v', 'e', 'd', ' ', 'b', 'y', ' ',
      'm', 'o', 'd', '_', 'h', '2', '6', '4',
      '_', 's', 't', 'r', 'e', 'a', 'm', 'i',
      'n', 'g'
    };
    memcpy(buffer, free_data, sizeof(free_data));
    buffer += sizeof(free_data);
  }

  new_mdat_start = buffer - (unsigned char*)(*mp4_header) + moov_atom.size_;
  if(!moov_seek(moov_data,
                &moov_atom.size_,
                start_time,
                end_time,
                &mdat_atom.start_,
                &mdat_atom.size_,
                new_mdat_start - mdat_atom.start_,
                client_is_flash))
  {
    free(moov_data);
    fclose(infile);
    return 0;
  }

  memcpy(buffer, moov_data, (uint32_t)moov_atom.size_);
  buffer += moov_atom.size_;
  free(moov_data);

  {
    int mdat_header_size = mp4_atom_write_header(buffer, &mdat_atom);
    buffer += mdat_header_size;
    *mdat_offset = mdat_atom.start_ + mdat_header_size;
    *mdat_size = mdat_atom.size_ - mdat_header_size;
  }

  *mp4_header_size = (uint32_t)(buffer - (unsigned char*)(*mp4_header));

  fclose(infile);

  return 1;
}

}

// End Of File

