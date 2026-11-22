#include "mdcat_remote.h"

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "mdcat_text.h"

namespace mdcat {

namespace {

// Extract the host from a git remote URL in either SSH or URL form. Returns "" if none can be
// found. Handles: scp-like SSH (git@host:path, host:path), ssh:// URLs, and https?:// URLs. Strips
// any userinfo (user@) and port (:NNN).
std::string gitRemoteHost(const std::string& remote) {
    std::string s = remote;
    // scheme://[userinfo@]host[:port]/...
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        std::string rest = s.substr(scheme + 3);
        size_t slash = rest.find('/');
        std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
        size_t at = authority.rfind('@');
        if (at != std::string::npos) authority = authority.substr(at + 1);
        size_t colon = authority.find(':');
        if (colon != std::string::npos) authority = authority.substr(0, colon);
        return toLowerCopy(authority);
    }
    // scp-like: [user@]host:path  (the first ':' that is NOT part of a path separates host and
    // path)
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        std::string authority = s.substr(0, colon);
        size_t at = authority.rfind('@');
        if (at != std::string::npos) authority = authority.substr(at + 1);
        return toLowerCopy(authority);
    }
    return std::string();
}

// Trusted-host entry: an exact host, optionally also matching subdomains.
struct TrustEntry {
    std::string host;        // already lowercased, no leading dot
    bool includeSubdomains;  // true for ".github.com" / "*.github.com" entries
};

bool trustEntryMatches(const TrustEntry& e, const std::string& host) {
    if (host == e.host) return true;
    if (e.includeSubdomains && host.size() > e.host.size() + 1 &&
        host.compare(host.size() - e.host.size(), e.host.size(), e.host) == 0 &&
        host[host.size() - e.host.size() - 1] == '.')
        return true;
    return false;
}

// Build (once) and return the trust list. fileDir scopes the git-remote lookup to the repository of
// the file being rendered.
const std::vector<TrustEntry>& trustedHosts(const std::string& fileDir) {
    static std::vector<TrustEntry> entries;
    static bool built = false;
    if (built) return entries;
    built = true;

    auto add = [&](std::string line) {
        // Strip comment and surrounding whitespace.
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(b, e - b + 1);
        if (line.empty()) return;
        bool sub = false;
        if (line.rfind("*.", 0) == 0) {
            sub = true;
            line = line.substr(2);
        } else if (line[0] == '.') {
            sub = true;
            line = line.substr(1);
        }
        if (!line.empty()) entries.push_back({toLowerCopy(line), sub});
    };

    // 1. Git remotes of the file's repository. Run git in fileDir (or CWD if empty) so the lookup
    //    reflects that repo. Failure (not a repo, no git) just yields no entries.
    {
        std::string dir = fileDir.empty() ? "." : fileDir;
        std::string q = "'";
        for (char c : dir) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
        q += "'";
        std::string cmd = "git -C " + q + " config --get-regexp '^remote\\..*\\.url' 2>/dev/null";
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[4096];
            std::string acc;
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, p)) > 0) acc.append(buf, got);
            pclose(p);
            size_t start = 0;
            while (start < acc.size()) {
                size_t nl = acc.find('\n', start);
                std::string row =
                    acc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                start = nl == std::string::npos ? acc.size() : nl + 1;
                // row is "remote.origin.url <value>"; take the value after the first space.
                size_t sp = row.find(' ');
                if (sp == std::string::npos) continue;
                std::string host = gitRemoteHost(row.substr(sp + 1));
                if (!host.empty()) entries.push_back({host, false});
            }
        }
    }

    // 2. User config file.
    {
        std::string base;
        if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            base = xdg;
        else if (const char* home = getenv("HOME"); home && *home)
            base = std::string(home) + "/.config";
        if (!base.empty()) {
            std::ifstream f(base + "/mdcat/trusted-hosts");
            std::string line;
            while (std::getline(f, line)) add(line);
        }
    }
    return entries;
}

}  // namespace

std::string curateRemoteImageUrl(const std::string& src, const std::string& fileDir) {
    static const std::string kHttps = "https://";
    if (src.size() <= kHttps.size() || toLowerCopy(src.substr(0, kHttps.size())) != kHttps)
        return std::string();
    for (unsigned char c : src)
        if (c < 0x20 || c == 0x7f || c == ' ') return std::string();
    std::string rest = src.substr(kHttps.size());
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.find('@') != std::string::npos) return std::string();  // reject embedded creds
    std::string host = authority;
    if (size_t colon = host.find(':'); colon != std::string::npos) host = host.substr(0, colon);
    host = toLowerCopy(host);
    if (host.empty()) return std::string();
    for (const TrustEntry& e : trustedHosts(fileDir))
        if (trustEntryMatches(e, host)) return src;
    return std::string();
}


std::string fetchRemoteImage(const std::string& url) {
    // Determine the extension from the path component (drop ?query / #fragment).
    std::string path = url;
    if (size_t q = path.find_first_of("?#"); q != std::string::npos) path = path.substr(0, q);
    std::string ext;
    if (size_t dot = path.rfind('.'); dot != std::string::npos && dot > path.rfind('/'))
        ext = path.substr(dot);

    // Build "<tmpdir>/mdcat-XXXXXX<ext>" via mkstemps so the suffix is preserved.
    const char* tmpdir = getenv("TMPDIR");
    std::string tmpl = (tmpdir && *tmpdir ? std::string(tmpdir) : std::string("/tmp"));
    if (!tmpl.empty() && tmpl.back() != '/') tmpl += '/';
    tmpl += "mdcat-XXXXXX" + ext;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), static_cast<int>(ext.size()));
    if (fd < 0) return std::string();
    close(fd);
    std::string out(buf.data());

    // Single-quote both the output path and URL for the shell.
    auto sh = [](const std::string& s) {
        std::string q = "'";
        for (char c : s) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
        q += "'";
        return q;
    };
    std::string cmd =
        "curl -fsSL --proto '=https' --max-time 15 --connect-timeout 8 "
        "--max-filesize 16000000 -o " +
        sh(out) + " " + sh(url) + " >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        unlink(out.c_str());
        return std::string();
    }
    // Reject an empty body (curl can write a 0-byte file on some odd success paths).
    std::ifstream f(out, std::ios::binary | std::ios::ate);
    if (!f.good() || f.tellg() == 0) {
        unlink(out.c_str());
        return std::string();
    }
    return out;
}

}  // namespace mdcat
