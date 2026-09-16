[CmdletBinding()]
param(
    [string] $Commit = 'HEAD',
    [string] $RemoteMain = 'origin/main'
)

$ErrorActionPreference = 'Stop'

if ($RemoteMain -notmatch '^(?<remote>[^/]+)/(?<branch>.+)$') {
    throw "Remote main reference '$RemoteMain' must use the form '<remote>/<branch>'."
}
$remote = $Matches.remote
$branch = $Matches.branch
$remoteRef = "refs/remotes/$RemoteMain"
& git fetch --no-tags --depth=1 $remote "+refs/heads/${branch}:$remoteRef"
if ($LASTEXITCODE -ne 0) {
    throw "Unable to fetch '$RemoteMain' for release identity validation."
}

$releaseCommit = (& git rev-parse "$Commit^{commit}").Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to resolve release commit '$Commit'."
}
$mainCommit = (& git rev-parse "$RemoteMain^{commit}").Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to resolve remote main commit '$RemoteMain'."
}
if ($releaseCommit -cne $mainCommit) {
    throw "Release commit '$releaseCommit' does not match '$RemoteMain' at '$mainCommit'."
}
Write-Host "Verified release commit '$releaseCommit' matches '$RemoteMain'."
