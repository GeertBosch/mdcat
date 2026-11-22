// Remote image trust: which https image URLs mdcat may fetch, and fetching them.
#pragma once

#include <string>

namespace mdcat {

// --- Remote image trust ----------------------------------------------------------------------
//
// Viewing a Markdown file should not fetch arbitrary network resources. mdcat will fetch a remote
// image (e.g. a CI badge) ONLY when the URL is https and its host is on a trust list built from two
// sources, with no built-in default hosts:
//   1. The git remotes of the repository containing the file being rendered: if you push/pull to a
//      host you already trust it. Both SSH (git@github.com:owner/repo.git) and HTTPS
//      (https://github.com/owner/repo.git) remote forms are parsed for their host.
//   2. A user-maintained file at $XDG_CONFIG_HOME/mdcat/trusted-hosts (or ~/.config/mdcat/
//      trusted-hosts): one host per line, '#' starts a comment. A bare host (github.com) trusts
//      exactly that host; a line starting with '.' or '*.' (.github.com / *.github.com) also trusts
//      its subdomains.
// Anything not trusted falls back to the image's alt text, exactly as a non-graphics terminal does.
// mdcat fetches with curl (https-only, redirect-following, with a timeout and size cap) into a temp
// file and then hands that LOCAL file to the normal timg pipeline. The trust check is on the
// INITIAL url's host only; curl follows redirects to other hosts (GitHub's badge redirects to a
// pipelines host), an accepted trade-off so badges work.

// If src is a remote image URL we are permitted to fetch, return the (validated) URL; otherwise
// return "". Only https is allowed; the URL must have a clean host (no userinfo, no control/space
// characters) that is on the trust list for fileDir.
std::string curateRemoteImageUrl(const std::string& src, const std::string& fileDir);

// Fetch a (already trusted, already validated) https image URL into a temp file and return its
// path, or "" on failure. The temp file keeps the URL's file extension (query string stripped) so
// the format-by-extension checks and timg both see e.g. ".svg". curl is invoked with a hard
// https-only proto restriction, redirect following (-L), a connect+total timeout, and a size cap so
// a viewing session can never block indefinitely or pull an unbounded body. The caller must unlink
// the returned path when done.
std::string fetchRemoteImage(const std::string& url);

}  // namespace mdcat
