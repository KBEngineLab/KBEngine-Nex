using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

internal static class Program
{
    private const string UdpHello = "62a559f3fa7748bc22f8e0766019d498";
    private const string UdpHelloAck = "1432ad7c829170a76dd31982c3501eca";

    private static async Task<int> Main(string[] args)
    {
        try
        {
            ProbeOptions options = ProbeOptions.Parse(args);
            IPAddress address = await ResolveAddressAsync(options.Host).ConfigureAwait(false);
            var endpoint = new IPEndPoint(address, options.Port);
            ProbeReport report = await RunAsync(endpoint, options).ConfigureAwait(false);

            PrintReport(report);
            if (options.OutputJson is not null)
            {
                string? directory = Path.GetDirectoryName(Path.GetFullPath(options.OutputJson));
                if (!string.IsNullOrEmpty(directory))
                {
                    Directory.CreateDirectory(directory);
                }

                await File.WriteAllTextAsync(options.OutputJson,
                    JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }))
                    .ConfigureAwait(false);
            }

            return report.SuccessfulSamples > 0 ? 0 : 2;
        }
        catch (ArgumentException exception)
        {
            Console.Error.WriteLine(exception.Message);
            PrintUsage();
            return 64;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static async Task<ProbeReport> RunAsync(IPEndPoint endpoint, ProbeOptions options)
    {
        byte[] request = Encoding.ASCII.GetBytes(UdpHello);
        byte[] expectedPrefix = Encoding.ASCII.GetBytes(UdpHelloAck + "\0");
        byte[] response = new byte[2048];
        var measured = new List<double>();
        int timedOut = 0;
        int invalid = 0;
        int warmupRemaining = options.WarmupSamples;
        long finishAt = Stopwatch.GetTimestamp() +
            (long)(options.DurationSeconds * (double)Stopwatch.Frequency);
        Socket socket = CreateSocket(endpoint);

        try
        {
            while (Stopwatch.GetTimestamp() < finishAt)
            {
                long startedAt = Stopwatch.GetTimestamp();
                await socket.SendAsync(request, SocketFlags.None).ConfigureAwait(false);

                try
                {
                    using var timeout = new CancellationTokenSource(options.TimeoutMilliseconds);
                    int length = await socket.ReceiveAsync(response, SocketFlags.None, timeout.Token)
                        .ConfigureAwait(false);
                    double elapsedMs = Stopwatch.GetElapsedTime(startedAt).TotalMilliseconds;

                    if (!response.AsSpan(0, length).StartsWith(expectedPrefix))
                    {
                        ++invalid;
                    }
                    else if (warmupRemaining > 0)
                    {
                        --warmupRemaining;
                    }
                    else
                    {
                        measured.Add(elapsedMs);
                    }
                }
                catch (Exception exception) when (IsUdpTimeout(exception))
                {
                    if (warmupRemaining > 0)
                    {
                        --warmupRemaining;
                    }
                    else
                    {
                        ++timedOut;
                    }

                    // 不更换本地端口：服务端会将每个新端口保留到 inactivity 超时，持续丢包时反复
                    // 建立 socket 会放大 Channel 数并耗尽系统 UDP 资源。短暂冷却后清空迟到 ACK，
                    // 下一样本继续复用同一条探测 Channel。
                    // Keep the local port because the server retains every replacement until its inactivity timeout.
                    // Recreating sockets during sustained loss amplifies Channel count and can exhaust UDP resources.
                    // A short cooldown followed by draining late ACKs keeps the next sample on the same probe Channel.
                    await Task.Delay(TimeSpan.FromMilliseconds(250)).ConfigureAwait(false);
                    DrainAvailable(socket, response);
                }

                long remainingDelay = options.IntervalMilliseconds -
                    (long)Stopwatch.GetElapsedTime(startedAt).TotalMilliseconds;
                if (remainingDelay > 0)
                {
                    await Task.Delay(TimeSpan.FromMilliseconds(remainingDelay)).ConfigureAwait(false);
                }
            }
        }
        finally
        {
            socket.Dispose();
        }

        measured.Sort();
        int attempted = measured.Count + timedOut + invalid;
        return new ProbeReport(
            endpoint.ToString(),
            DateTimeOffset.UtcNow,
            options.DurationSeconds,
            options.IntervalMilliseconds,
            options.TimeoutMilliseconds,
            attempted,
            measured.Count,
            timedOut,
            invalid,
            attempted == 0 ? 0 : measured.Count * 100.0 / attempted,
            measured.Count == 0 ? 0 : measured[0],
            measured.Count == 0 ? 0 : measured.Average(),
            Percentile(measured, 0.50),
            Percentile(measured, 0.95),
            Percentile(measured, 0.99),
            Percentile(measured, 0.999),
            measured.Count == 0 ? 0 : measured[^1]);
    }

    private static Socket CreateSocket(EndPoint endpoint)
    {
        var socket = new Socket(endpoint.AddressFamily, SocketType.Dgram, ProtocolType.Udp);
        socket.Connect(endpoint);
        return socket;
    }

    private static bool IsUdpTimeout(Exception exception)
    {
        // A connected UDP socket on Windows reports an ICMP Port Unreachable as
        // ConnectionReset. It is an unanswered probe sample, not a process-fatal
        // transport failure, and must remain comparable with cancellation timeouts.
        // Windows 的 connected UDP 会把 ICMP Port Unreachable 映射为 ConnectionReset；
        // 它表示本次探测无响应，不应中止整个报告，应与取消超时使用相同统计口径。
        return exception is OperationCanceledException ||
            exception is SocketException
            {
                SocketErrorCode: SocketError.ConnectionReset or SocketError.ConnectionRefused
            };
    }

    private static void DrainAvailable(Socket socket, byte[] buffer)
    {
        while (socket.Available > 0)
        {
            socket.Receive(buffer, SocketFlags.None);
        }
    }

    private static async Task<IPAddress> ResolveAddressAsync(string host)
    {
        if (IPAddress.TryParse(host, out IPAddress? address))
        {
            return address;
        }

        IPAddress[] addresses = await Dns.GetHostAddressesAsync(host).ConfigureAwait(false);
        return addresses.FirstOrDefault(candidate => candidate.AddressFamily == AddressFamily.InterNetwork)
            ?? addresses.FirstOrDefault()
            ?? throw new ArgumentException($"Host did not resolve to an address: {host}");
    }

    private static double Percentile(IReadOnlyList<double> sorted, double percentile)
    {
        if (sorted.Count == 0)
        {
            return 0;
        }

        // 采用 nearest-rank，结果能与压测报告按实际观测样本直接对应，不制造插值样本。
        // Nearest-rank keeps every reported percentile tied to an observed sample instead of an interpolated value.
        int rank = Math.Max(1, (int)Math.Ceiling(percentile * sorted.Count));
        return sorted[rank - 1];
    }

    private static void PrintReport(ProbeReport report)
    {
        Console.WriteLine($"endpoint={report.Endpoint} attempted={report.AttemptedSamples} " +
            $"success={report.SuccessfulSamples} timeout={report.TimeoutSamples} invalid={report.InvalidSamples} " +
            $"successRate={report.SuccessRatePercent:F2}%");
        Console.WriteLine("metric    milliseconds");
        Console.WriteLine($"min       {report.MinMilliseconds,12:F3}");
        Console.WriteLine($"mean      {report.MeanMilliseconds,12:F3}");
        Console.WriteLine($"p50       {report.P50Milliseconds,12:F3}");
        Console.WriteLine($"p95       {report.P95Milliseconds,12:F3}");
        Console.WriteLine($"p99       {report.P99Milliseconds,12:F3}");
        Console.WriteLine($"p99.9     {report.P999Milliseconds,12:F3}");
        Console.WriteLine($"max       {report.MaxMilliseconds,12:F3}");
    }

    private static void PrintUsage()
    {
        Console.Error.WriteLine("Usage: dotnet run --project KcpLatencyProbe.csproj -- --port <port> " +
            "[--host 127.0.0.1] [--duration 60] [--interval-ms 100] [--timeout-ms 5000] " +
            "[--warmup 10] [--output-json report.json]");
    }
}

internal sealed record ProbeOptions(
    string Host,
    int Port,
    int DurationSeconds,
    int IntervalMilliseconds,
    int TimeoutMilliseconds,
    int WarmupSamples,
    string? OutputJson)
{
    public static ProbeOptions Parse(string[] args)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < args.Length; index += 2)
        {
            if (!args[index].StartsWith("--", StringComparison.Ordinal) || index + 1 >= args.Length)
            {
                throw new ArgumentException($"Invalid argument near '{args[index]}'. Options require a value.");
            }

            values[args[index]] = args[index + 1];
        }

        int port = PositiveInt(values, "--port", required: true);
        int duration = PositiveInt(values, "--duration", fallback: 60);
        int interval = PositiveInt(values, "--interval-ms", fallback: 100);
        int timeout = PositiveInt(values, "--timeout-ms", fallback: 5000);
        int warmup = NonNegativeInt(values, "--warmup", fallback: 10);
        string host = values.GetValueOrDefault("--host", "127.0.0.1");
        values.TryGetValue("--output-json", out string? outputJson);

        string[] supported = ["--host", "--port", "--duration", "--interval-ms", "--timeout-ms", "--warmup", "--output-json"];
        string? unknown = values.Keys.FirstOrDefault(key => !supported.Contains(key, StringComparer.OrdinalIgnoreCase));
        if (unknown is not null)
        {
            throw new ArgumentException($"Unknown option: {unknown}");
        }

        if (port > IPEndPoint.MaxPort)
        {
            throw new ArgumentException($"--port must be at most {IPEndPoint.MaxPort}.");
        }

        return new ProbeOptions(host, port, duration, interval, timeout, warmup, outputJson);
    }

    private static int PositiveInt(IReadOnlyDictionary<string, string> values, string name,
        int fallback = 0, bool required = false)
    {
        if (!values.TryGetValue(name, out string? text))
        {
            if (required)
            {
                throw new ArgumentException($"Missing required option: {name}");
            }

            return fallback;
        }

        if (!int.TryParse(text, out int value) || value <= 0)
        {
            throw new ArgumentException($"{name} must be a positive integer.");
        }

        return value;
    }

    private static int NonNegativeInt(IReadOnlyDictionary<string, string> values, string name, int fallback)
    {
        if (!values.TryGetValue(name, out string? text))
        {
            return fallback;
        }

        if (!int.TryParse(text, out int value) || value < 0)
        {
            throw new ArgumentException($"{name} must be a non-negative integer.");
        }

        return value;
    }
}

internal sealed record ProbeReport(
    string Endpoint,
    DateTimeOffset FinishedAtUtc,
    int DurationSeconds,
    int IntervalMilliseconds,
    int TimeoutMilliseconds,
    int AttemptedSamples,
    int SuccessfulSamples,
    int TimeoutSamples,
    int InvalidSamples,
    double SuccessRatePercent,
    double MinMilliseconds,
    double MeanMilliseconds,
    double P50Milliseconds,
    double P95Milliseconds,
    double P99Milliseconds,
    double P999Milliseconds,
    double MaxMilliseconds);
