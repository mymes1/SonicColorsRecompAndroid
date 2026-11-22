#include "update_apply.h"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

std::string UpdateApply_PsSingleQuote(const std::string& s)
{
    std::string o = "'";
    for (char c : s)
    {
        if (c == '\'') o += "''";
        else o += c;
    }
    o += '\'';
    return o;
}

std::string UpdateApply_WindowsPs1(const std::string& zipUtf8,
                                   const std::string& destUtf8,
                                   const std::string& exeName,
                                   uint32_t waitPid,
                                   const std::string& stageDirUtf8,
                                   const std::string& logPathUtf8)
{
    // Paths are embedded as PowerShell single-quoted literals (no batch % expansion).
    // The helper lives under user downloads — outside the files being replaced.
    return std::format(R"PS($ErrorActionPreference = 'Stop'
$Zip = {0}
$Dest = {1}
$ExeName = {2}
$WaitPid = {3}
$Stage = {4}
$Log = {5}

function Write-Log([string]$m) {{
  $line = ('[{{0:yyyy-MM-dd HH:mm:ss}}] {{1}}' -f (Get-Date), $m)
  Add-Content -LiteralPath $Log -Value $line -Encoding UTF8
}}

try {{
  Write-Log ('waiting for pid ' + $WaitPid)
  $deadline = (Get-Date).AddMinutes(15)
  while ($true) {{
    $proc = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue
    if (-not $proc) {{ break }}
    if ((Get-Date) -gt $deadline) {{ throw ('timed out waiting for process ' + $WaitPid) }}
    Start-Sleep -Milliseconds 400
  }}
  Write-Log 'process exited; preparing stage'
  if (Test-Path -LiteralPath $Stage) {{ Remove-Item -LiteralPath $Stage -Recurse -Force }}
  New-Item -ItemType Directory -Path $Stage | Out-Null
  Write-Log ('extracting ' + $Zip)
  Expand-Archive -LiteralPath $Zip -DestinationPath $Stage -Force
  $Src = Join-Path $Stage 'SonicColorsRecomp'
  if (-not (Test-Path -LiteralPath (Join-Path $Src $ExeName))) {{ $Src = $Stage }}
  $SrcExe = Join-Path $Src $ExeName
  if (-not (Test-Path -LiteralPath $SrcExe)) {{ throw ('update package missing ' + $ExeName) }}
  $DestExe = Join-Path $Dest $ExeName
  $Bak = $DestExe + '.preupdate'
  if (Test-Path -LiteralPath $DestExe) {{
    Write-Log 'backing up current executable'
    Copy-Item -LiteralPath $DestExe -Destination $Bak -Force
  }}
  Write-Log ('installing into ' + $Dest)
  # /E copy tree; /IS /IT overwrite same/tweaked files; treat 0-7 as success.
  & robocopy $Src $Dest /E /IS /IT /R:2 /W:1 /NFL /NDL /NJH /NJS /XD '_scr_update_stage' | Out-Null
  $rc = $LASTEXITCODE
  if ($rc -ge 8) {{
    if (Test-Path -LiteralPath $Bak) {{
      Copy-Item -LiteralPath $Bak -Destination $DestExe -Force
      Write-Log 'restored executable from backup after robocopy failure'
    }}
    throw ('robocopy failed with code ' + $rc)
  }}
  if (-not (Test-Path -LiteralPath $DestExe)) {{
    if (Test-Path -LiteralPath $Bak) {{ Copy-Item -LiteralPath $Bak -Destination $DestExe -Force }}
    throw 'executable missing after install'
  }}
  Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $Bak) {{ Remove-Item -LiteralPath $Bak -Force -ErrorAction SilentlyContinue }}
  Write-Log 'install ok; relaunching'
  Start-Process -FilePath $DestExe
}} catch {{
  $msg = $_.Exception.Message
  Write-Log ('FAILED: ' + $msg)
  try {{
    Start-Process -FilePath 'notepad.exe' -ArgumentList $Log
  }} catch {{}}
  exit 1
}}
)PS",
                       UpdateApply_PsSingleQuote(zipUtf8), UpdateApply_PsSingleQuote(destUtf8),
                       UpdateApply_PsSingleQuote(exeName), waitPid, UpdateApply_PsSingleQuote(stageDirUtf8),
                       UpdateApply_PsSingleQuote(logPathUtf8));
}

bool UpdateApply_LooksLikeZip(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char mag[4]{};
    in.read(mag, 4);
    if (in.gcount() < 4) return false;
    // Local file header or empty zip end-of-central-directory.
    return (mag[0] == 'P' && mag[1] == 'K' && (mag[2] == 3 || mag[2] == 5 || mag[2] == 7));
}

bool UpdateApply_LooksLikeAppImage(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char mag[4]{};
    in.read(reinterpret_cast<char*>(mag), 4);
    if (in.gcount() < 4) return false;
    // ELF magic — AppImages are ELF runtimes with a appended filesystem.
    return mag[0] == 0x7f && mag[1] == 'E' && mag[2] == 'L' && mag[3] == 'F';
}

bool UpdateApply_FileSizeOk(const fs::path& path, uint64_t expected, std::string& err)
{
    err.clear();
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
    {
        err = "downloaded package is missing";
        return false;
    }
    const auto sz = fs::file_size(path, ec);
    if (ec || sz == 0)
    {
        err = "downloaded package is empty or unreadable";
        return false;
    }
    if (expected && sz != expected)
    {
        err = std::format("downloaded size {} does not match release asset size {}", sz, expected);
        return false;
    }
    return true;
}

namespace
{
std::string PathUtf8(const fs::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string EntryPathUtf8(archive_entry* e)
{
#ifdef _WIN32
    if (const wchar_t* w = archive_entry_pathname_w(e)) return PathUtf8(fs::path(w));
#endif
    if (const char* u = archive_entry_pathname_utf8(e)) return u;
    if (const char* n = archive_entry_pathname(e)) return n;
    return {};
}

bool PathHasDotDot(std::string_view rel)
{
    size_t i = 0;
    while (i < rel.size())
    {
        size_t j = rel.find_first_of("/\\", i);
        if (j == std::string_view::npos) j = rel.size();
        const auto part = rel.substr(i, j - i);
        if (part == "..") return true;
        i = j + (j < rel.size() ? 1 : 0);
    }
    return false;
}

std::string Basename(std::string_view rel)
{
    const auto pos = rel.find_last_of("/\\");
    if (pos == std::string_view::npos) return std::string(rel);
    return std::string(rel.substr(pos + 1));
}
}   // namespace

bool UpdateApply_ZipContainsExe(const fs::path& zip, const std::string& exeName, std::string& err)
{
    err.clear();
    archive* a = archive_read_new();
    archive_read_support_format_zip(a);
#ifdef _WIN32
    const int openRc = archive_read_open_filename_w(a, zip.c_str(), 1 << 16);
#else
    const int openRc = archive_read_open_filename(a, zip.c_str(), 1 << 16);
#endif
    if (openRc != ARCHIVE_OK)
    {
        err = std::format("cannot open update zip: {}",
                          archive_error_string(a) ? archive_error_string(a) : "unknown format");
        archive_read_free(a);
        return false;
    }
    bool found = false;
    bool badPath = false;
    archive_entry* e = nullptr;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN)
    {
        const std::string rel = EntryPathUtf8(e);
        if (rel.empty()) continue;
        if (PathHasDotDot(rel))
        {
            badPath = true;
            break;
        }
        if (archive_entry_filetype(e) == AE_IFREG && Basename(rel) == exeName) found = true;
        archive_read_data_skip(a);
    }
    if (!badPath && r != ARCHIVE_EOF && !found)
    {
        err = std::format("cannot read update zip: {}",
                          archive_error_string(a) ? archive_error_string(a) : "error");
        archive_read_free(a);
        return false;
    }
    archive_read_free(a);
    if (badPath)
    {
        err = "update zip contains unsafe paths";
        return false;
    }
    if (!found)
    {
        err = std::format("update zip does not contain {}", exeName);
        return false;
    }
    return true;
}
