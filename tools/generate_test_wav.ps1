param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\assets\vocal_mono.wav')
)

$sampleRate = 48000
$secondsPerNote = 1
$frequencies = @(261.6256, 293.6648, 329.6276, 349.2282,
                 391.9954, 440.0000, 493.8833, 523.2511)
$samplesPerNote = $sampleRate * $secondsPerNote
$sampleCount = $samplesPerNote * $frequencies.Count
$dataBytes = $sampleCount * 2
$fadeSamples = [int]($sampleRate * 0.02)

$resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [IO.Path]::GetDirectoryName($resolvedOutput)
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

$stream = [IO.File]::Open($resolvedOutput, [IO.FileMode]::Create)
$writer = [IO.BinaryWriter]::new($stream)

try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([int](36 + $dataBytes))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
    $writer.Write([int]16)
    $writer.Write([int16]1)
    $writer.Write([int16]1)
    $writer.Write([int]$sampleRate)
    $writer.Write([int]($sampleRate * 2))
    $writer.Write([int16]2)
    $writer.Write([int16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([int]$dataBytes)

    foreach ($frequency in $frequencies) {
        for ($i = 0; $i -lt $samplesPerNote; $i++) {
            $envelope = 1.0
            if ($i -lt $fadeSamples) {
                $envelope = $i / [double]$fadeSamples
            } elseif ($i -ge ($samplesPerNote - $fadeSamples)) {
                $envelope = ($samplesPerNote - 1 - $i) / [double]$fadeSamples
            }

            $phase = 2.0 * [Math]::PI * $frequency * $i / $sampleRate
            $sample = [int16]([Math]::Round(0.32 * 32767 * $envelope * [Math]::Sin($phase)))
            $writer.Write($sample)
        }
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

Write-Host "Generated $resolvedOutput (48 kHz, mono, 16-bit, 8 seconds)"
