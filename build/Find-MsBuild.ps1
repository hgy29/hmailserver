function Find-MsBuild {
    param(
        [string]$VsWhereMinVersion = '[17.0,18.0)'
    )

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

    $msbuild = $null
    if (Test-Path $vswhere) {
        try {
            $msbuild = & $vswhere -version $VsWhereMinVersion -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        } catch {
            $msbuild = $null
        }
    } else {
        Write-Verbose "vswhere not found at $vswhere"
    }

    if (-not $msbuild) {
        $msbuildCmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
        if ($msbuildCmd) { $msbuild = $msbuildCmd.Source }
    }

    return $msbuild
}
