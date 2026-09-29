#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "hyperlink/json.h"
#include "monitors.h"

namespace updater {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    Handle(HINTERNET x = nullptr) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

// GETs a URL, following redirects. Streams the body to onData.
bool httpGet(const std::string& url, const wchar_t* accept,
             const std::function<bool(const uint8_t*, size_t, uint64_t total)>& onData,
             std::string& error) {
    std::wstring wurl = fromUtf8(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        error = "Bad URL";
        return false;
    }
    Handle session(WinHttpOpen(L"Hyperlink-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) session.h = WinHttpOpen(L"Hyperlink-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    Handle conn(WinHttpConnect(session, host, uc.nPort, 0));
    std::wstring full = std::wstring(path) + extra;
    Handle req(WinHttpOpenRequest(conn, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        error = "Cannot open connection";
        return false;
    }
    std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
    if (!WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        error = "No connection to GitHub (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                        nullptr);
    if (status != 200) {
        error = "GitHub answered HTTP " + std::to_string(status);
        return false;
    }
    uint64_t total = 0;
    wchar_t cl[32];
    DWORD clLen = sizeof cl;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, cl, &clLen, nullptr))
        total = _wcstoui64(cl, nullptr, 10);
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got)) {
            error = "Download interrupted";
            return false;
        }
        if (got == 0) break;
        if (!onData(buf.data(), got, total)) {
            error = "Cancelled";
            return false;
        }
    }
    return true;
}

}  // namespace

bool isNewer(const std::string& version, const std::string& current) {
    return hl::json::versionCode(version) > hl::json::versionCode(current);
}

bool latest(ReleaseInfo& out, std::string& error) {
    std::string body;
    if (!httpGet("https://api.github.com/repos/" HYPERLINK_REPO "/releases/latest",
                 L"application/vnd.github+json",
                 [&](const uint8_t* p, size_t n, uint64_t) {
                     body.append((const char*)p, n);
                     return body.size() < 4 * 1024 * 1024;
                 },
                 error))
        return false;
    hl::json::Value v;
    if (!hl::json::parse(body, v)) {
        error = "Unexpected reply from GitHub";
        return false;
    }
    std::string tag = v["tag_name"].asString();
    out.version = tag.size() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    out.notes = v["body"].asString();
    for (auto& a : v["assets"].arr) {
        std::string name = a["name"].asString();
        if (name.rfind("Hyperlink-Setup-", 0) == 0 && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".exe") == 0) {
            out.assetName = name;
            out.assetUrl = a["browser_download_url"].asString();
            out.size = (uint64_t)a["size"].asNumber();
            std::string digest = a["digest"].asString();
            if (digest.rfind("sha256:", 0) == 0) out.sha256 = digest.substr(7);
        }
    }
    if (out.assetUrl.empty()) {
        error = "The latest release has no Windows setup program";
        return false;
    }
    return true;
}

bool download(const ReleaseInfo& r, const std::wstring& path,
              const std::function<void(uint64_t, uint64_t)>& progress, std::string& error) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        error = "Cannot write the download";
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (alg) BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    uint64_t done = 0;
    bool ok = httpGet(r.assetUrl, L"application/octet-stream",
                      [&](const uint8_t* p, size_t n, uint64_t total) {
                          if (fwrite(p, 1, n, f) != n) return false;
                          if (hash) BCryptHashData(hash, (PUCHAR)p, (ULONG)n, 0);
                          done += n;
                          progress(done, total ? total : r.size);
                          return true;
                      },
                      error);
    fclose(f);
    std::string hex;
    if (hash) {
        uint8_t digest[32];
        BCryptFinishHash(hash, digest, 32, 0);
        char b[3];
        for (uint8_t x : digest) {
            snprintf(b, sizeof b, "%02x", x);
            hex += b;
        }
        BCryptDestroyHash(hash);
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (ok && r.size && done != r.size) {
        error = "Download was incomplete";
        ok = false;
    }
    if (ok && !r.sha256.empty() && hex != r.sha256) {
        error = "Download is damaged (checksum mismatch)";
        ok = false;
    }
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool runInstaller(const std::wstring& path) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

}  // namespace updater
