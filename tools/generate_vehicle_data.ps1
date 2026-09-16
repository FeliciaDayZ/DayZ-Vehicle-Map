param(
    [string]$ServerMissionRoot = 'C:\Program Files (x86)\Steam\steamapps\common\DayZServer\mpmissions',
    [string]$AdditionalSourceRoot = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $projectRoot 'data\spawns'
if (-not $AdditionalSourceRoot) {
    $AdditionalSourceRoot = Join-Path $projectRoot 'data\source'
}

$vehicleTypes = @(
    @{ Output = 'VehicleCivilianSedan'; Child = 'CivilianSedan' },
    @{ Output = 'VehicleHatchback02'; Child = 'Hatchback_02' },
    @{ Output = 'VehicleOffroad02'; Child = 'Offroad_02' },
    @{ Output = 'VehicleOffroadHatchback'; Child = 'OffroadHatchback' },
    @{ Output = 'VehicleSedan02'; Child = 'Sedan_02' },
    @{ Output = 'VehicleTruck01'; Child = 'Truck_01_Covered' },
    @{ Output = 'VehicleBoat'; Child = 'Boat_01_' },
    @{ Output = 'VehicleMotorbike01'; Child = 'Motorbike_01_' },
    @{ Output = 'VehicleMotorbike02'; Child = 'Motorbike_02_' }
)

$directEvents = @{}
foreach ($type in $vehicleTypes | Select-Object -First 7) {
    $directEvents[$type.Output] = @($type.Output)
}
$directEvents.VehicleMotorbike01 = @('VehicleMotorbike')
$directEvents.VehicleMotorbike02 = @('VehicleMotorbike')

$maps = @(
    @{
        Id = 'chernarusplus'; WorldSize = 15360
        Spawn = Join-Path $ServerMissionRoot 'dayzOffline.chernarusplus\cfgeventspawns.xml'
        Events = Join-Path $ServerMissionRoot 'dayzOffline.chernarusplus\db\events.xml'
        SourceEvents = $directEvents
    },
    @{
        Id = 'enoch'; WorldSize = 12800
        Spawn = Join-Path $ServerMissionRoot 'dayzOffline.enoch\cfgeventspawns.xml'
        Events = Join-Path $ServerMissionRoot 'dayzOffline.enoch\db\events.xml'
        SourceEvents = $directEvents
    },
    @{
        Id = 'sakhal'; WorldSize = 15360
        Spawn = Join-Path $ServerMissionRoot 'dayzOffline.sakhal\cfgeventspawns.xml'
        Events = Join-Path $ServerMissionRoot 'dayzOffline.sakhal\db\events.xml'
        SourceEvents = @{
            VehicleCivilianSedan = @('VehicleCivilianSedan')
            VehicleHatchback02 = @('VehicleHatchback02')
            VehicleOffroad02 = @()
            VehicleOffroadHatchback = @('VehicleOffroadHatchback')
            VehicleSedan02 = @('VehicleSedan02')
            VehicleTruck01 = @('VehicleTruck01')
            VehicleBoat = @('VehicleBoat', 'VehicleMilitaryBoat')
            VehicleMotorbike01 = @('VehicleMotorbike')
            VehicleMotorbike02 = @('VehicleMotorbike')
        }
    },
    @{
        Id = 'namalsk'; WorldSize = 12800
        Spawn = Join-Path $AdditionalSourceRoot 'namalsk\cfgeventspawns.xml'
        Events = Join-Path $AdditionalSourceRoot 'namalsk\events.xml'
        SourceEvents = @{
            VehicleCivilianSedan = @('VehicleCivilianCars')
            VehicleHatchback02 = @('VehicleCivilianCars')
            VehicleOffroad02 = @('VehicleMilitary')
            VehicleOffroadHatchback = @('VehicleCivilianCars')
            VehicleSedan02 = @('VehicleCivilianCars')
            VehicleTruck01 = @('VehicleTrucks')
            VehicleBoat = @('VehicleRubberBoats')
            VehicleMotorbike01 = @()
            VehicleMotorbike02 = @()
        }
    },
    @{
        Id = 'deerisle'; WorldSize = 16384
        Spawn = Join-Path $AdditionalSourceRoot 'deerisle\cfgeventspawns.xml'
        Events = Join-Path $AdditionalSourceRoot 'deerisle\events.xml'
        SourceEvents = @{
            VehicleCivilianSedan = @('VehicleCivilianSedan')
            VehicleHatchback02 = @('VehicleHatchback02')
            VehicleOffroad02 = @()
            VehicleOffroadHatchback = @('VehicleOffroadHatchback')
            VehicleSedan02 = @('VehicleSedan02')
            VehicleTruck01 = @('VehicleTruck01')
            VehicleBoat = @()
            VehicleMotorbike01 = @()
            VehicleMotorbike02 = @()
        }
    },
    @{
        Id = 'bitterroot'; WorldSize = 12288
        Spawn = Join-Path $AdditionalSourceRoot 'bitterroot\cfgeventspawns.xml'
        Events = Join-Path $AdditionalSourceRoot 'bitterroot\events.xml'
        SourceEvents = @{
            VehicleCivilianSedan = @('VehicleCivilianSedan')
            VehicleHatchback02 = @('VehicleHatchback02')
            VehicleOffroad02 = @('VehicleOffroad02')
            VehicleOffroadHatchback = @('VehicleOffroadHatchback')
            VehicleSedan02 = @('VehicleSedan02')
            VehicleTruck01 = @('VehicleTruck01')
            VehicleBoat = @()
            VehicleMotorbike01 = @()
            VehicleMotorbike02 = @()
        }
    },
    @{
        Id = 'banov'; WorldSize = 15360
        Spawn = Join-Path $AdditionalSourceRoot 'banov\cfgeventspawns.xml'
        Events = Join-Path $AdditionalSourceRoot 'banov\events.xml'
        SourceEvents = @{
            VehicleCivilianSedan = @('VehicleCivilianSedan')
            VehicleHatchback02 = @('VehicleHatchback02')
            VehicleOffroad02 = @('VehicleOffroad02')
            VehicleOffroadHatchback = @('VehicleOffroadHatchback')
            VehicleSedan02 = @('VehicleSedan02')
            VehicleTruck01 = @('VehicleTruck01')
            VehicleBoat = @('VehicleBoat')
            VehicleMotorbike01 = @()
            VehicleMotorbike02 = @()
        }
    }
)

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$culture = [Globalization.CultureInfo]::InvariantCulture

foreach ($map in $maps) {
    if (-not (Test-Path -LiteralPath $map.Spawn) -or
        -not (Test-Path -LiteralPath $map.Events)) {
        throw "Missing source mission XML for $($map.Id)."
    }

    [xml]$spawnXml = Get-Content -Raw -LiteralPath $map.Spawn
    [xml]$eventsXml = Get-Content -Raw -LiteralPath $map.Events
    $lines = New-Object System.Collections.Generic.List[string]
    $skippedOutOfRange = New-Object System.Collections.Generic.List[string]
    $lines.Add('# Generated from verified DayZ mission event data; do not edit by hand.')
    $lines.Add('# event,x,z')

    foreach ($type in $vehicleTypes) {
        $seen = @{}
        foreach ($sourceEventName in @($map.SourceEvents[$type.Output])) {
            $spawnEvent = @($spawnXml.eventposdef.event |
                Where-Object { [string]$_.name -eq $sourceEventName })
            $eventDefinition = @($eventsXml.events.event |
                Where-Object { [string]$_.name -eq $sourceEventName })
            if ($spawnEvent.Count -ne 1 -or $eventDefinition.Count -ne 1) {
                throw "Expected exactly one spawn and event definition for $sourceEventName in $($map.Id)."
            }
            $definition = $eventDefinition[0]
            $children = @($definition.children.child | ForEach-Object { [string]$_.type })
            if ([string]$definition.active -ne '1' -or
                [string]$definition.position -ne 'fixed' -or
                $children.Count -eq 0 -or
                -not @($children | Where-Object { $_.StartsWith($type.Child) }).Count) {
                throw "Event $sourceEventName is not an active fixed $($type.Output) source in $($map.Id)."
            }

            foreach ($position in @($spawnEvent[0].pos)) {
                $xText = ([string]$position.x).Trim()
                $zText = ([string]$position.z).Trim()
                $x = [double]::Parse($xText, $culture)
                $z = [double]::Parse($zText, $culture)
                if ($x -lt 0 -or $x -gt $map.WorldSize -or
                    $z -lt 0 -or $z -gt $map.WorldSize) {
                    $skippedOutOfRange.Add("$sourceEventName ($xText, $zText)")
                    continue
                }
                $key = $x.ToString('R', $culture) + '|' + $z.ToString('R', $culture)
                if (-not $seen.ContainsKey($key)) {
                    $seen[$key] = $true
                    $lines.Add("$($type.Output),$xText,$zText")
                }
            }
        }
    }

    $outputPath = Join-Path $outputRoot ($map.Id + '.csv')
    [System.IO.File]::WriteAllLines($outputPath, $lines, $utf8NoBom)
    Write-Output "$($map.Id): $($lines.Count - 2) positions -> $outputPath"
    if ($skippedOutOfRange.Count) {
        Write-Warning ("$($map.Id): excluded $($skippedOutOfRange.Count) out-of-world source row(s): " +
            ($skippedOutOfRange -join '; '))
    }
}
