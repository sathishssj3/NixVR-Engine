param (
    [string]$Token,
    [string]$Tag = "v0.1.0",
    [string]$Repo = "sathishssj3/NexVR-Engine-Releases",
    [string]$ArtifactsDir = "launcher\dist-electron"
)

$ErrorActionPreference = "Stop"

if (-not $Token) {
    # Attempt local git credential fill if running locally
    $cred = "protocol=https`nhost=github.com`n" | git credential fill
    foreach ($line in $cred) {
        if ($line -match "^password=(.+)$") {
            $Token = $matches[1].Trim()
        }
    }
}

if (-not $Token) {
    Write-Error "No GitHub token provided or found."
    exit 1
}

$headers = @{
    "Authorization" = "Bearer $Token"
    "Accept"        = "application/vnd.github.v3+json"
    "User-Agent"    = "NexVR-Release-Uploader"
}

Write-Host ">>> Querying releases for $Repo..."
$releases = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers -Method Get

$targetRelease = $releases | Where-Object { $_.tag_name -eq $Tag }
if (-not $targetRelease) {
    Write-Host ">>> Release with tag '$Tag' not found. Creating it..."
    $createBody = @{
        tag_name         = $Tag
        name             = "NexVR Engine $Tag"
        body             = "Automated release build of NexVR Engine ($Tag)."
        draft            = $false
        prerelease       = $false
    } | ConvertTo-Json

    $targetRelease = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers -Method Post -Body $createBody -ContentType "application/json"
    Write-Host ">>> Created release ID: $($targetRelease.id)"
} else {
    Write-Host ">>> Found existing release ID: $($targetRelease.id) (Tag: $($targetRelease.tag_name))"
}

$uploadBase = "https://uploads.github.com/repos/$Repo/releases/$($targetRelease.id)/assets"

# Locate setup.exe files to upload
$filesToUpload = Get-ChildItem -Path $ArtifactsDir -Include "*Setup*.exe", "NexVR-Engine-Setup.exe", "*Portable*.exe", "*.zip" -File -Recurse

if ($filesToUpload.Count -eq 0) {
    Write-Warning "No installer files found in $ArtifactsDir to upload."
    exit 0
}

# Fetch currently attached assets to replace if already present
$existingAssets = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/$($targetRelease.id)/assets" -Headers $headers -Method Get

foreach ($file in $filesToUpload) {
    $fileName = $file.Name
    $fileSizeMB = [math]::Round($file.Length / 1MB, 2)

    $existing = $existingAssets | Where-Object { $_.name -eq $fileName }
    if ($existing) {
        Write-Host ">>> Deleting existing asset '$fileName' (ID: $($existing.id))..."
        Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/assets/$($existing.id)" -Headers $headers -Method Delete
    }

    Write-Host ">>> Uploading $fileName ($fileSizeMB MB)..."
    $escapedName = [Uri]::EscapeDataString($fileName)
    $uploadUri = "${uploadBase}?name=${escapedName}"

    $curlArgs = @(
        "-s",
        "-X", "POST",
        "-H", "Authorization: Bearer $Token",
        "-H", "Content-Type: application/octet-stream",
        "--data-binary", "@$($file.FullName)",
        $uploadUri
    )

    & curl.exe @curlArgs | Out-Null
    Write-Host ">>> Upload complete for $fileName"
}

Write-Host ">>> All release assets uploaded successfully to https://github.com/$Repo/releases/tag/$Tag"
