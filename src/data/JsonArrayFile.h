#pragma once
#include <cstdio>
#include <string>

namespace blah2::data {
// Shared legacy append contract: preserve raw payload bytes, array-boundary
// checks and partial-write failure behavior. This does not parse/reformat JSON.
inline bool appendJsonArray(const std::string& _json, const std::string& filename)
{
  // create file if it doesn't exist
  if (std::FILE *fp = std::fopen(filename.c_str(), "r"); fp)
  {
    if (std::fclose(fp) != 0)
      return false;
  }
  else
  {
    if (fp = std::fopen(filename.c_str(), "w"); !fp)
      return false;
    const bool initialized = std::fputs("[]", fp) >= 0;
    if (std::fclose(fp) != 0 || !initialized)
      return false;
  }

  // add the document to the file
  if (std::FILE *fp = std::fopen(filename.c_str(), "rb+"); fp)
  {
    // check if first is [
    if (std::fseek(fp, 0, SEEK_SET) != 0)
    {
      std::fclose(fp);
      return false;
    }
    if (std::getc(fp) != '[')
    {
      std::fclose(fp);
      return false;
    }

    // is array empty?
    bool isEmpty = false;
    if (std::getc(fp) == ']')
      isEmpty = true;

    // check if last is ]
    if (std::fseek(fp, -1, SEEK_END) != 0)
    {
      std::fclose(fp);
      return false;
    }
    if (std::getc(fp) != ']')
    {
      std::fclose(fp);
      return false;
    }

    // replace ] by ,
    if (std::fseek(fp, -1, SEEK_END) != 0 ||
        (!isEmpty && std::fputc(',', fp) == EOF))
    {
      std::fclose(fp);
      return false;
    }

    // add json element
    const bool wroteJson = std::fwrite(_json.data(), sizeof(char),
      _json.size(), fp) == _json.size();

    // close the array
    const bool closedArray = wroteJson && std::fputc(']', fp) != EOF;
    const bool flushed = closedArray && std::fflush(fp) == 0;
    return std::fclose(fp) == 0 && flushed;
  }
  return false;
}
} // namespace blah2::data
