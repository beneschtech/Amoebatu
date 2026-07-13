#include <string>
#include "fcgi_request_cpp.hxx"

namespace FCGI
{
/**
 * @brief urldecode - Takes a url encoded string and decodes it into its clean,
 * binary representation if needed. Decode is an easy operation, encoding
 * has a standard to follow, but we can accomodate both in this file
 * @param s string to urldecode
 * @return std::string with the urldecoded data
 */
std::string urldecode(std::string s)
{
    std::string rv;
    const char *p  = s.data();
    const char *ep = s.data() + s.length();
    while (p < ep)
    {
        switch (*p)
        {
        case '+':
            rv.append(" ");
            break;
        case '%': {
            // Need exactly 2 hex digits after '%'. If the string is
            // truncated, treat the remainder as literal characters
            // rather than walking past the end of the buffer.
            if (p + 2 >= ep)
            {
                rv.append(p, ep - p);
                p = ep;
                continue;
            }
            char hexv[3] = { p[1], p[2], '\0' };
            char *endp = nullptr;
            long val = std::strtol(hexv, &endp, 16);
            if (endp == hexv + 2)
            {
                // Both digits were valid hex
                rv.push_back(static_cast<char>(val & 0xff));
                p += 2; // skip the two hex digits; loop increment gets '%'
            }
            else
            {
                // Not valid hex — emit the '%' literally
                rv.push_back('%');
            }
            break;
        }
        default:
            rv.push_back(*p);
        }
        p++;
    }
    return rv;
} // urldecode

// Complaint with RFC 3986
std::string urlencode(std::string s)
{
  std::string resvdChars = ":/?#[]@!$&'()*+,;= ";
  /* ":" / "/" / "?" / "#" / "[" / "]" / "@"
   * "!" / "$" / "&" / "'" / "(" / ")"
  / "*" / "+" / "," / ";" / "=" */

  std::string rv;
  for (char c: s)
  {
    if (resvdChars.find(c) == std::string::npos) // not a reserved char
    {
      rv.append({c});
    } else if (c == ' ') {
      rv.append("+");
    } else {
      char buf[8] = { 0, };
      snprintf(buf,sizeof(buf),"%%%02x",c);
      rv.append(buf);
    }
  }
  return rv;
} // urlencode

} // namespace FCGI
