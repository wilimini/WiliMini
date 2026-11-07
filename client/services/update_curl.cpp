#include "update_curl.hpp"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
namespace pspapp {
namespace {
struct Ctx {
    const std::function<bool(int, const std::string&)>* onHeaders;
    const std::function<bool(const void*, size_t)>* onData;
    int status = 0;
    std::string contentRange;
    bool headersSent = false, abortedByCallback = false;
    const volatile bool* cancel;
};
bool iequalsPrefix(const char* s, size_t n, const char* prefix) {
    size_t p = std::strlen(prefix);
    if (n < p) return false;
    for (size_t i = 0; i < p; ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i])) return false;
    return true;
}
// Called for every header line of every response, including redirects, so the
// state is reset at each status line and only the final response survives.
size_t onHeaderLine(char* b, size_t sz, size_t n, void* u) {
    Ctx* c = (Ctx*)u;
    size_t len = sz * n;
    if (len >= 5 && std::strncmp(b, "HTTP/", 5) == 0) {
        const char* sp = (const char*)std::memchr(b, ' ', len);
        c->status = sp ? std::atoi(sp + 1) : 0;
        c->contentRange.clear();
    } else if (iequalsPrefix(b, len, "content-range:")) {
        std::string v(b + 14, len - 14);
        size_t s = v.find_first_not_of(" \t"), e = v.find_last_not_of(" \t\r\n");
        c->contentRange = s == std::string::npos ? "" : v.substr(s, e - s + 1);
    }
    return len;
}
bool sendHeaders(Ctx* c) {
    if (c->headersSent) return true;
    c->headersSent = true;
    return (*c->onHeaders)(c->status, c->contentRange);
}
size_t onBody(char* d, size_t sz, size_t n, void* u) {
    Ctx* c = (Ctx*)u;
    if (c->status >= 300 && c->status < 400) return sz * n; // redirect body: ignore
    if (!sendHeaders(c)) { c->abortedByCallback = true; return 0; }
    if (!(*c->onData)(d, sz * n)) { c->abortedByCallback = true; return 0; }
    return sz * n;
}
int onProgress(void* u, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    Ctx* c = (Ctx*)u;
    return c->cancel && *c->cancel ? 1 : 0;
}
} // namespace

bool CurlUpdateTransport::get(const std::string& url, uint64_t rangeFrom,
                              const std::function<bool(int, const std::string&)>& onHeaders,
                              const std::function<bool(const void*, size_t)>& onData) {
    if (url.compare(0, 8, "https://") != 0) return false; // never plain HTTP
    CURL* h = curl_easy_init();
    if (!h) return false;
    Ctx c;
    c.onHeaders = &onHeaders;
    c.onData = &onData;
    c.cancel = cancel_;
    char err[CURL_ERROR_SIZE] = {0};
    curl_slist* hdr = curl_slist_append(nullptr, "Accept: application/vnd.github+json, application/json, */*");
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_USERAGENT, "WiliMini-updater"); // GitHub rejects requests without one
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CAINFO, ca_);
    curl_easy_setopt(h, CURLOPT_CAPATH, (char*)nullptr);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L); // release assets redirect to a download host
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeaderLine);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &c);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &c);
    if (rangeFrom) {
        std::string r = std::to_string(rangeFrom) + "-";
        curl_easy_setopt(h, CURLOPT_RANGE, r.c_str()); // curl copies the string
    }
    CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(h);
    // An empty body (e.g. 416) never reaches the write callback.
    if (rc == CURLE_OK && !c.headersSent) sendHeaders(&c);
    if (c.abortedByCallback) return false;
    // 416 for a stale .part is a protocol answer, not a transport failure.
    if (rc == CURLE_HTTP_RETURNED_ERROR) return false;
    return rc == CURLE_OK && c.status >= 200 && c.status < 300;
}
}
