# Shared helpers for the test-VM scripts (dot-source this file).

<#
  Copies a file into the guest as raw bytes and checks its SHA-256.
  Copy-Item -ToSession copies the content but then fails ("The parameter is incorrect") when the source is on an
  exFAT drive, apparently while applying file metadata; sending the bytes avoids that.
#>
function Send-VmFile([System.Management.Automation.Runspaces.PSSession]$Session, [string]$Source, [string]$Destination) {
    $bytes = [IO.File]::ReadAllBytes($Source)
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Source).Hash
    $remote = Invoke-Command -Session $Session -ArgumentList $Destination, $bytes {
        param($path, $data)
        New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
        [IO.File]::WriteAllBytes($path, $data)
        (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
    }
    if ($remote -ne $hash) { throw "copy of $Source to the guest is corrupt ($remote vs $hash)" }
}
