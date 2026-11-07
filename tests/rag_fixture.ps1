param([int]$Port = 18887, [switch]$Redirect)
$ErrorActionPreference = 'Stop'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Output "LISTENING: $Port"
try {
    $client = $listener.AcceptTcpClient()
    try {
        $client.ReceiveTimeout = 5000
        $client.SendTimeout = 5000
        $stream = $client.GetStream()
        $request = New-Object byte[] 4096
        $n = $stream.Read($request, 0, $request.Length)
        if ($n -le 0) { throw 'No request received' }
        $first = [Text.Encoding]::ASCII.GetString($request, 0, $n).Split("`r`n")[0]
        Write-Output "REQUEST: $first"
        if ($Redirect) {
            $body = 'blocked'
            $status = '302 Found'
            $extra = "Location: https://example.org/`r`n"
        }
        else {
            $body = '{"results":[{"title":"Local Casper Source","url":"https://example.test/source","content":"Casper source-linked result from an isolated local fixture"}]}'
            $status = '200 OK'
            $extra = ''
        }
        $nbytes = [Text.Encoding]::UTF8.GetByteCount($body)
        $response = "HTTP/1.1 $status`r`nContent-Type: application/json`r`nContent-Length: $nbytes`r`nConnection: close`r`n$extra`r`n$body"
        $bytes = [Text.Encoding]::UTF8.GetBytes($response)
        $stream.Write($bytes, 0, $bytes.Length)
    }
    finally { $client.Close() }
}
finally { $listener.Stop() }
