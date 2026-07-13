
#include "fcgi_request_cpp.hxx"

#ifdef __linux
#include <pthread.h>
#include <cstring>
#include <unistd.h>
#include <sys/syscall.h>
#endif

static std::string _serverName;

namespace FCGI
{

std::string serverName() { return _serverName; }
void setServerName(std::string s) { _serverName = s; }

void SetThreadName(const char* threadName)
{
    if (!*threadName)
        return;
#ifdef __linux
    if (getpid() != syscall(SYS_gettid))
    {
        // Basically strncpy into a preallocated buffer
        char namebuf[16];
        size_t sz = strlen(threadName);
        if (sz > sizeof(namebuf)-1)
            sz = sizeof(namebuf)-1;
        memset(namebuf,0,sizeof(namebuf));
        memcpy(namebuf,threadName,sz);
        namebuf[15] = 0; // Make sure its 0 terminated
        pthread_t myThread = pthread_self();
        pthread_setname_np(myThread,namebuf);
    }
#endif
}

std::vector<std::string> str_split(std::string haystack, char needle)
{
    std::vector<std::string> rv;
    std::string::size_type sz = 0;
    while (sz != std::string::npos)
    {
        std::string::size_type i = haystack.find_first_of(needle, sz);
        std::string token = haystack.substr(sz, i == std::string::npos ? i : i - sz);
        // Skip trailing empty tokens produced by a trailing delimiter.
        // "a=1&" was previously inserting a blank key into query fields.
        if (!token.empty() || i == std::string::npos)
            rv.push_back(token);
        sz = (i == std::string::npos) ? std::string::npos : i + 1;
    }
    return rv;
}


std::map<std::string,std::string> query_string_parse(std::string l)
{
    std::map<std::string,std::string> rv;
    if (l.empty())
        return rv;
    std::vector<std::string> nvpairs = str_split(l,'&');
    for (std::string pair: nvpairs)
    {
        std::string::size_type idx = pair.find_first_of('=');
        if (idx == std::string::npos)
            continue; // malformed pair, skip silently
        std::string key = pair.substr(0, idx);
        std::string val = urldecode(pair.substr(idx + 1));
        if (!key.empty())
            rv.insert({key, val});
    }
    return rv;
}

std::map<std::string,std::string> cookie_parse(std::string l)
{
    std::map<std::string,std::string> rv;
    if (l.empty())
        return rv;
    std::vector<std::string> nvpairs = str_split(l,';');
    for (std::string pair: nvpairs)
    {
        std::string::size_type idx = pair.find_first_of('=');
        if (idx == std::string::npos)
            continue;
        std::string key = string_trim(pair.substr(0, idx));
        // Cookie values are urldecoded for consistency with query fields.
        // Previously they were returned raw, causing subtle bugs when
        // cookie values contained encoded characters.
        std::string val = urldecode(string_trim(pair.substr(idx + 1)));
        if (!key.empty())
            rv.insert({key, val});
    }
    return rv;
}

/**
 * @brief sanitize_header_value strips CR and LF characters from a
 * header name or value string, preventing HTTP response header injection.
 * Call this on any user-influenced data before passing to set_header().
 * @param s the string to sanitize
 * @return the string with all \\r and \\n characters removed
 */
std::string sanitize_header_value(std::string s)
{
    std::string rv;
    rv.reserve(s.size());
    for (char c : s)
    {
        if (c != '\r' && c != '\n')
            rv.push_back(c);
    }
    return rv;
}

std::string string_trim(std::string s)
{
    std::string rv = s;
    while (std::isspace(rv.front()))
    {
        rv = rv.substr(1);
    }
    while (std::isspace(rv.back()))
    {
        rv = rv.substr(0,rv.length()-1);
    }
    return rv;
}

}; // namespace FCGI
