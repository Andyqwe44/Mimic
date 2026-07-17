# Patch nginx on aliyun: serve C:\mimic\cdn under /mimic/
$ErrorActionPreference = 'Stop'
$conf = 'C:\tools\nginx-1.31.1\conf\nginx.conf'
$bak = "C:\tools\nginx-1.31.1\conf\nginx.conf.bak_mimic_$(Get-Date -Format yyyyMMddHHmmss)"
Copy-Item $conf $bak -Force

$snippet = @'
        # Mimic CDN — client/server update payloads (static)
        location /mimic/ {
            alias C:/mimic/cdn/;
            autoindex off;
            charset utf-8;
            types {
                application/json              json;
                application/zip               zip;
                application/octet-stream      exe dll;
                text/plain                    txt md;
                text/html                     html;
                text/css                      css;
                application/javascript        js;
                image/svg+xml                 svg;
            }
            default_type application/octet-stream;
            add_header Access-Control-Allow-Origin "*" always;
            add_header Cache-Control "public, max-age=60" always;
        }

'@

$text = [IO.File]::ReadAllText($conf)
if ($text -match 'location /mimic/') {
    Write-Host 'nginx already has /mimic/ — skip insert'
} else {
    # Insert before first "location / {" inside each server (two servers: :80 and :443)
    $needle = "        location / {"
    $count = ([regex]::Matches($text, [regex]::Escape($needle))).Count
    if ($count -lt 1) { throw "could not find location / in nginx.conf" }
    $sb = New-Object System.Text.StringBuilder
    $idx = 0
    $inserted = 0
    while ($true) {
        $next = $text.IndexOf($needle, $idx)
        if ($next -lt 0) {
            [void]$sb.Append($text.Substring($idx))
            break
        }
        [void]$sb.Append($text.Substring($idx, $next - $idx))
        [void]$sb.Append($snippet)
        [void]$sb.Append($needle)
        $idx = $next + $needle.Length
        $inserted++
    }
    [IO.File]::WriteAllText($conf, $sb.ToString())
    Write-Host "inserted /mimic/ location $inserted time(s)"
}

& 'C:\tools\nginx-1.31.1\nginx.exe' -t -c $conf
if ($LASTEXITCODE -ne 0) { throw 'nginx -t failed' }
& 'C:\tools\nginx-1.31.1\nginx.exe' -s reload
Write-Host 'nginx reloaded OK'

# smoke placeholder
'{"ok":true,"service":"mimic-cdn"}' | Set-Content -Encoding utf8 'C:\mimic\cdn\health.json'
Write-Host 'wrote health.json'
