#include "zip_unpack.h"

#include <SD.h>
#include <miniz.h>

#include "log_buf.h"

namespace Zip {

namespace {

const uint32_t kSigEocd = 0x06054b50;
const uint32_t kSigCentral = 0x02014b50;
const uint32_t kSigLocal = 0x04034b50;

const size_t kInChunk = 2048;

uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Finds the end-of-central-directory record, which sits at the very end of the
 * file unless there is a trailing comment - hence the backwards scan. */
bool findEocd(File &f, uint32_t &centralOffset, uint16_t &entries) {
  const uint32_t size = f.size();
  if (size < 22) {
    return false;
  }
  const uint32_t window = size < 66000 ? size : 66000;
  const uint32_t from = size - window;

  uint8_t buf[512];
  for (int32_t at = (int32_t)size - 22; at >= (int32_t)from; at -= 400) {
    const uint32_t start = at > 400 ? (uint32_t)(at - 400) : 0;
    const size_t want = (size_t)min<uint32_t>(sizeof(buf), size - start);
    f.seek(start);
    if (f.read(buf, want) != (int)want) {
      return false;
    }
    for (int32_t i = (int32_t)want - 22; i >= 0; --i) {
      if (rd32(buf + i) == kSigEocd) {
        entries = rd16(buf + i + 10);
        centralOffset = rd32(buf + i + 16);
        return true;
      }
    }
    if (start == 0) {
      break;
    }
  }
  return false;
}

/* Keeps every extracted path inside the target directory.
 *
 * Archives are allowed to contain "../../etc/passwd" and plenty have; the trick
 * is old enough to have a name. Anything with "..", a leading slash, a drive
 * letter or a backslash is refused outright rather than cleaned up, because
 * cleaning up is where the subtle holes live. */
bool safeEntryName(String &name) {
  name.replace('\\', '/');
  if (name.length() == 0 || name.length() > 120) {
    return false;
  }
  if (name.startsWith("/") || name.indexOf("..") >= 0 || name.indexOf(':') >= 0) {
    return false;
  }
  /* Rubbish macOS puts in every archive it makes. */
  if (name.startsWith("__MACOSX/") || name.endsWith(".DS_Store")) {
    return false;
  }
  return true;
}

/* Creates any directories the entry names, one level at a time - SD.mkdir does
 * not make parents. */
void ensureParents(const String &fullPath) {
  int at = fullPath.indexOf('/', 1);
  while (at > 0) {
    const String part = fullPath.substring(0, at);
    if (!SD.exists(part)) {
      SD.mkdir(part);
    }
    at = fullPath.indexOf('/', at + 1);
  }
}

struct Inflater {
  tinfl_decompressor *decomp = nullptr;
  uint8_t *dict = nullptr;
  uint8_t *in = nullptr;

  bool alloc() {
    decomp = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
    dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
    in = (uint8_t *)malloc(kInChunk);
    return decomp && dict && in;
  }
  ~Inflater() {
    free(decomp);
    free(dict);
    free(in);
  }
};

/* The canonical miniz streaming loop: the dictionary doubles as the output
 * window, and every byte it produces is flushed to the card before the window
 * wraps around. */
bool inflateTo(File &src, uint32_t compressed, File &dst, uint32_t &wrote,
               Inflater &z) {
  tinfl_init(z.decomp);
  size_t dictOfs = 0;
  size_t inAvail = 0, inPos = 0;
  uint32_t left = compressed;
  wrote = 0;

  for (;;) {
    if (inAvail == 0) {
      const size_t want = (size_t)min<uint32_t>(kInChunk, left);
      if (want == 0) {
        return false; /* ran out of input before the stream ended */
      }
      const int got = src.read(z.in, want);
      if (got <= 0) {
        return false;
      }
      inAvail = (size_t)got;
      inPos = 0;
      left -= (uint32_t)got;
    }

    size_t inSize = inAvail;
    size_t outSize = TINFL_LZ_DICT_SIZE - dictOfs;
    const tinfl_status st =
        tinfl_decompress(z.decomp, z.in + inPos, &inSize, z.dict, z.dict + dictOfs,
                         &outSize, left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
    inPos += inSize;
    inAvail -= inSize;

    if (outSize) {
      if (dst.write(z.dict + dictOfs, outSize) != outSize) {
        return false;
      }
      wrote += (uint32_t)outSize;
      dictOfs = (dictOfs + outSize) & (TINFL_LZ_DICT_SIZE - 1);
    }

    if (st == TINFL_STATUS_DONE) {
      return true;
    }
    if (st < TINFL_STATUS_DONE) {
      return false;
    }
  }
}

bool copyStored(File &src, uint32_t bytes, File &dst, uint8_t *buf) {
  uint32_t left = bytes;
  while (left) {
    const size_t want = (size_t)min<uint32_t>(kInChunk, left);
    const int got = src.read(buf, want);
    if (got <= 0 || dst.write(buf, (size_t)got) != (size_t)got) {
      return false;
    }
    left -= (uint32_t)got;
  }
  return true;
}

} // namespace

Result extract(const String &archivePath, const String &intoDir) {
  Result res;

  File zip = SD.open(archivePath, FILE_READ);
  if (!zip) {
    res.message = "the uploaded archive could not be reopened";
    return res;
  }

  uint32_t centralOffset = 0;
  uint16_t entries = 0;
  if (!findEocd(zip, centralOffset, entries)) {
    zip.close();
    res.message = "not a ZIP file, or its directory is damaged";
    return res;
  }

  Inflater z;
  if (!z.alloc()) {
    zip.close();
    res.message = "not enough memory to unpack";
    return res;
  }

  uint32_t at = centralOffset;
  for (uint16_t i = 0; i < entries; ++i) {
    uint8_t hdr[46];
    zip.seek(at);
    if (zip.read(hdr, sizeof(hdr)) != (int)sizeof(hdr) || rd32(hdr) != kSigCentral) {
      res.message = "the archive directory ends sooner than it claims";
      zip.close();
      return res;
    }
    const uint16_t method = rd16(hdr + 10);
    const uint32_t compressed = rd32(hdr + 20);
    const uint16_t nameLen = rd16(hdr + 28);
    const uint16_t extraLen = rd16(hdr + 30);
    const uint16_t commentLen = rd16(hdr + 32);
    const uint32_t localAt = rd32(hdr + 42);

    String name;
    {
      char nb[128];
      const size_t take = nameLen < sizeof(nb) - 1 ? nameLen : sizeof(nb) - 1;
      zip.read((uint8_t *)nb, take);
      nb[take] = '\0';
      name = String(nb);
    }
    at += 46 + nameLen + extraLen + commentLen;

    if (name.endsWith("/") || !safeEntryName(name)) {
      continue; /* a directory entry, or something we will not touch */
    }
    if (method != 0 && method != 8) {
      res.message = "entry \"" + name + "\" uses a compression this cannot read";
      zip.close();
      return res;
    }

    /* The local header repeats the name and extra fields, and their lengths can
     * differ from the central copy - so read them again rather than reusing. */
    uint8_t loc[30];
    zip.seek(localAt);
    if (zip.read(loc, sizeof(loc)) != (int)sizeof(loc) || rd32(loc) != kSigLocal) {
      res.message = "entry \"" + name + "\" is not where the directory says";
      zip.close();
      return res;
    }
    zip.seek(localAt + 30 + rd16(loc + 26) + rd16(loc + 28));

    const String target = intoDir + "/" + name;
    ensureParents(target);
    if (SD.exists(target)) {
      SD.remove(target);
    }
    File out = SD.open(target, FILE_WRITE);
    if (!out) {
      res.message = "could not create " + name;
      zip.close();
      return res;
    }

    uint32_t wrote = 0;
    const bool good = method == 0 ? copyStored(zip, compressed, out, z.in)
                                  : inflateTo(zip, compressed, out, wrote, z);
    if (method == 0) {
      wrote = compressed;
    }
    out.close();

    if (!good) {
      SD.remove(target);
      res.message = "entry \"" + name + "\" would not unpack";
      zip.close();
      return res;
    }

    res.files++;
    res.bytes += wrote;
  }

  zip.close();
  res.ok = true;
  return res;
}

} // namespace Zip
