using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Channels;
using QRCoder;
using SteamKit2;
using SteamKit2.Authentication;

// A private, length-prefixed pipe is the only interface. Never write diagnostics,
// credentials, or tickets to a terminal, a command line, or a file.
if (args.Length != 0 || !Console.IsInputRedirected || !Console.IsOutputRedirected) return 2;
using var stop = new CancellationTokenSource();
using var pipe = new Pipe(stop);
using var steam = new Session(pipe, stop);
try
{
    await steam.Run();
    return 0;
}
catch (OperationCanceledException) { return 0; }
catch (Exception error)
{
    try
    {
        pipe.Send(5, error switch
        {
            AuthenticationException auth => $"Steam sign-in failed ({auth.Result}). Try signing in again.",
            LoginFailure login => login.Message,
            TimeoutException => "Steam did not respond in time. Check your connection and try again.",
            _ => "The Steam session could not continue. Check your connection and sign in again."
        }, (error is LoginFailure failure ? failure.Code : error is AuthenticationException ? 1013 : 1010).ToString());
    }
    catch (IOException) { }
    return 1;
}

sealed class LoginFailure(string message, int code = 1010) : Exception(message)
{
    public int Code { get; } = code;
}
sealed record Message(uint Kind, string[] Fields);

sealed class Pipe : IDisposable
{
    const int Limit = 65536;
    readonly Stream input = Console.OpenStandardInput(), output = Console.OpenStandardOutput();
    readonly CancellationTokenSource stop;
    readonly Channel<Message> messages = Channel.CreateBounded<Message>(4);
    readonly object writeLock = new();
    readonly Task reader;
    public CancellationToken Token => stop.Token;
    public Pipe(CancellationTokenSource stop)
    {
        this.stop = stop;
        reader = Read();
    }
    async Task Read()
    {
        try
        {
            byte[] header = new byte[4];
            while (!Token.IsCancellationRequested)
            {
                await input.ReadExactlyAsync(header, Token);
                int size = BinaryPrimitives.ReadInt32LittleEndian(header);
                if (size < 4 || size > Limit) throw new InvalidDataException();
                byte[] bytes = new byte[size];
                try
                {
                    await input.ReadExactlyAsync(bytes, Token);
                    uint kind = BinaryPrimitives.ReadUInt32LittleEndian(bytes);
                    List<string> fields = [];
                    for (int offset = 4; offset < size;)
                    {
                        if (size - offset < 4 || fields.Count == 4) throw new InvalidDataException();
                        int length = BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(offset));
                        offset += 4;
                        if (length < 0 || length > size - offset) throw new InvalidDataException();
                        fields.Add(new UTF8Encoding(false, true).GetString(bytes, offset, length));
                        offset += length;
                    }
                    if (!messages.Writer.TryWrite(new(kind, [.. fields]))) throw new InvalidDataException();
                }
                finally { CryptographicOperations.ZeroMemory(bytes); }
            }
        }
        catch (Exception error) when (error is IOException or OperationCanceledException or ArgumentException)
        {
            stop.Cancel();
        }
        finally { messages.Writer.TryComplete(); }
    }
    public async Task<Message> Receive() => await messages.Reader.ReadAsync(Token);
    public void Send(uint kind, params string[] fields)
    {
        using var bytes = new MemoryStream();
        using var writer = new BinaryWriter(bytes, Encoding.UTF8, true);
        writer.Write(kind);
        foreach (string field in fields)
        {
            byte[] encoded = Encoding.UTF8.GetBytes(field);
            writer.Write(encoded.Length);
            writer.Write(encoded);
            CryptographicOperations.ZeroMemory(encoded);
        }
        if (bytes.Length > Limit) throw new InvalidDataException();
        lock (writeLock)
        {
            Span<byte> header = stackalloc byte[4];
            BinaryPrimitives.WriteInt32LittleEndian(header, (int)bytes.Length);
            try
            {
                output.Write(header);
                output.Write(bytes.GetBuffer(), 0, (int)bytes.Length);
                output.Flush();
            }
            finally { CryptographicOperations.ZeroMemory(bytes.GetBuffer()); }
        }
    }
    public void Dispose()
    {
        stop.Cancel();
        input.Dispose();
        output.Dispose();
        // The OS closes a blocked anonymous-pipe read when this short-lived
        // helper exits. Never join a reader that depends on the parent exiting.
        _ = reader.Exception;
    }
}

sealed class Session : IDisposable, IAuthenticator
{
    readonly SteamClient client = new();
    readonly CallbackManager callbacks;
    readonly SteamUser user;
    readonly Pipe pipe;
    readonly CancellationTokenSource stop;
    readonly TaskCompletionSource<bool> connected = new(TaskCreationOptions.RunContinuationsAsynchronously);
    readonly TaskCompletionSource<SteamUser.LoggedOnCallback> loggedOn = new(TaskCreationOptions.RunContinuationsAsynchronously);
    readonly TaskCompletionSource<EResult> disconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
    SteamAuthTicket.TicketInfo? ticket;
    Task? pump;

    public Session(Pipe pipe, CancellationTokenSource stop)
    {
        this.pipe = pipe;
        this.stop = stop;
        callbacks = new(client);
        user = client.GetHandler<SteamUser>()!;
        callbacks.Subscribe<SteamClient.ConnectedCallback>(_ => connected.TrySetResult(true));
        callbacks.Subscribe<SteamClient.DisconnectedCallback>(_ => disconnected.TrySetResult(EResult.NoConnection));
        callbacks.Subscribe<SteamUser.LoggedOnCallback>(value => loggedOn.TrySetResult(value));
        callbacks.Subscribe<SteamUser.LoggedOffCallback>(value => disconnected.TrySetResult(value.Result));
    }
    async Task<T> Bounded<T>(Task<T> task, int seconds)
    {
        var outcome = await Task.WhenAny(task, disconnected.Task).WaitAsync(
            seconds == 0 ? Timeout.InfiniteTimeSpan : TimeSpan.FromSeconds(seconds), pipe.Token);
        if (outcome == disconnected.Task)
            throw new LoginFailure($"Steam disconnected ({await disconnected.Task}). Sign in again.");
        return await task;
    }
    async Task Connect()
    {
        client.Connect();
        pump = Task.Run(() =>
        {
            while (!pipe.Token.IsCancellationRequested)
                callbacks.RunWaitCallbacks(TimeSpan.FromMilliseconds(100));
        });
        await Bounded(connected.Task, 30);
    }
    void ShowQr(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || uri.Scheme != "https" || uri.Host != "s.team")
            throw new InvalidDataException();
        using var data = QRCodeGenerator.GenerateQrCode(url, QRCodeGenerator.ECCLevel.M);
        var bits = new StringBuilder();
        foreach (var row in data.ModuleMatrix)
            foreach (bool bit in row) bits.Append(bit ? '1' : '0');
        pipe.Send(1, bits.ToString());
    }
    async Task<AuthSession> SignIn(Message request)
    {
        AuthSessionDetails details = new()
        {
            DeviceFriendlyName = "GW2 Multi Launcher",
            IsPersistentSession = true,
            Authenticator = this
        };
        if (request.Kind == 1)
        {
            var qr = await Bounded(client.Authentication.BeginAuthSessionViaQRAsync(details), 30);
            qr.ChallengeURLChanged = () => ShowQr(qr.ChallengeURL);
            ShowQr(qr.ChallengeURL);
            return qr;
        }
        (details.Username, details.Password) = (request.Fields[0], request.Fields[1]);
        if (details.Username.Length is < 1 or > 64 || details.Password.Length is < 1 or > 1024)
            throw new InvalidDataException();
        try { return await Bounded(client.Authentication.BeginAuthSessionViaCredentialsAsync(details), 30); }
        finally { details.Password = null; }
    }
    async Task<(string Name, string Token, string Expected)> Credentials(Message request)
    {
        if (request.Kind == 3)
        {
            var (expected, name, token) = (request.Fields[0], request.Fields[1], request.Fields[2]);
            if (!ulong.TryParse(expected, out var id) || !new SteamID(id).IsValid ||
                name.Length is < 1 or > 64 || token.Length is < 1 or > 8192)
                throw new InvalidDataException();
            return (name, token, expected);
        }
        var session = await SignIn(request);
        request = new(0, []);
        var result = await Bounded(session.PollingWaitForResultAsync(pipe.Token), 300);
        return (result.AccountName, result.RefreshToken, "");
    }
    async Task LogOn(string name, string token, string expected)
    {
        user.LogOn(new SteamUser.LogOnDetails
        {
            Username = name, AccessToken = token, ShouldRememberPassword = true,
            LoginID = (uint)RandomNumberGenerator.GetInt32(1, int.MaxValue)
        });
        var login = await Bounded(loggedOn.Task, 30);
        if (login.Result != EResult.OK)
            throw new LoginFailure($"Steam sign-in failed ({login.Result}). Sign in again.");
        string identity = client.SteamID?.ConvertToUInt64().ToString() ?? "";
        if (identity.Length == 0 || (expected.Length != 0 && identity != expected))
            throw new LoginFailure("Steam signed in to a different account. Reconnect this saved account.", 1014);
        // Publish only after the authenticated CM session proves identity. The
        // parent encrypts this credential; we never trust an unverified JWT ID.
        pipe.Send(3, identity, name, token);
        token = "";
    }
    async Task Tickets()
    {
        while (!pipe.Token.IsCancellationRequested)
        {
            var command = await Bounded(pipe.Receive(), ticket is null ? 600 : 0);
            if (command.Kind != 5 || command.Fields.Length != 0)
                throw new InvalidDataException();
            if (ticket is not null)
            {
                ticket.Dispose();
                CryptographicOperations.ZeroMemory(ticket.Ticket);
                ticket = null;
            }
            // SteamKit checks ownership as part of this request.
            ticket = await Bounded(client.GetHandler<SteamAuthTicket>()!.GetAuthSessionTicket(1284210), 30);
            if (ticket.Ticket.Length is < 1 or > 299)
                throw new LoginFailure("Steam returned a ticket larger than GW2's native login accepts.", 1012);
            pipe.Send(4, Convert.ToHexStringLower(ticket.Ticket));
        }
    }
    public async Task Run()
    {
        Message request = await pipe.Receive().WaitAsync(TimeSpan.FromSeconds(15), pipe.Token);
        if ((request.Kind == 1 && request.Fields.Length != 0) ||
            (request.Kind == 2 && request.Fields.Length != 2) ||
            (request.Kind == 3 && request.Fields.Length != 3) || request.Kind is < 1 or > 3)
            throw new InvalidDataException();
        await Connect();
        var (name, token, expected) = await Credentials(request);
        request = new(0, []);
        await LogOn(name, token, expected);
        token = "";
        await Tickets();
    }
    async Task<string> Code(string prompt)
    {
        pipe.Send(2, prompt);
        var code = await Bounded(pipe.Receive(), 300);
        if (code.Kind != 4 || code.Fields.Length != 1 || code.Fields[0].Length != 5 ||
            code.Fields[0].Any(c => !char.IsAsciiLetterOrDigit(c))) throw new InvalidDataException();
        return code.Fields[0];
    }
    public Task<string> GetDeviceCodeAsync(bool retry) =>
        Code(retry ? "That code was incorrect. Enter a new Steam Guard code." : "Enter your Steam Guard code.");
    public Task<string> GetEmailCodeAsync(string email, bool retry) =>
        Code(retry ? "That code was incorrect. Enter the new code from Steam's email." : "Enter the code from Steam's email.");
    public Task<bool> AcceptDeviceConfirmationAsync()
    {
        pipe.Send(6, "Approve sign-in in your Steam mobile app.");
        return Task.FromResult(true);
    }
    public void Dispose()
    {
        ticket?.Dispose();
        if (ticket is not null) CryptographicOperations.ZeroMemory(ticket.Ticket);
        user.LogOff();
        client.Disconnect();
        stop.Cancel();
        pump?.GetAwaiter().GetResult();
    }
}
