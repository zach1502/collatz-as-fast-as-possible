# PSScriptAnalyzer settings for scripts/windows. Run on Windows PowerShell 5.1
# or pwsh 7:
#   Install-Module PSScriptAnalyzer -Scope CurrentUser
#   Invoke-ScriptAnalyzer -Path scripts/windows -Recurse -Settings tools/lint/PSScriptAnalyzerSettings.psd1
@{
    Severity     = @('Error', 'Warning')
    # The setup scripts talk to the console on purpose.
    ExcludeRules = @('PSAvoidUsingWriteHost')
    Rules        = @{
        PSUseCompatibleSyntax = @{
            Enable         = $true
            TargetVersions = @('5.1', '7.4')
        }
    }
}
