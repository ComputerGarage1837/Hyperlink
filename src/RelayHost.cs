using System;
using System.IO;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace Hyperlink
{
    sealed class RelayHost : IDisposable
    {
        readonly Store store; readonly Host host; readonly Action<string> status; readonly RelayProvisioning provisioning;
        readonly CancellationTokenSource stop = new CancellationTokenSource();
        RelaySocket current;
        internal volatile bool Online;
        internal RelayHost(Store state, Host desktop, Action<string> changed)
        {
            store = state; host = desktop; status = changed;
            provisioning = RelayProvisioning.Load(store);
            lock (store.Sync) { store.Data.RelayOwnerToken = provisioning.Token; store.Data.RelayComputerCode = provisioning.Code; store.Save(); }
            Task.Run((Func<Task>)Run);
        }
        async Task Run()
        {
            while (!stop.IsCancellationRequested)
            {
                RelaySocket socket = null; CancellationTokenSource heartbeatStop = null; Task heartbeat = null;
                try
                {
                    if (!host.Running || !store.Data.Unattended) return;
                    socket = RelaySocket.Open("control"); current = socket;
                    if (stop.IsCancellationRequested) break;
                    socket.SendJson(new { operation = "register", id = store.Id, fingerprint = store.Fingerprint, name = store.Data.Name, token = provisioning.Token, enrollment = provisioning.Enrollment, attestation = provisioning.Attestation });
                    var registered = socket.ReadJson(); string code = Wire.Text(registered, "code");
                    if (Wire.Text(registered, "kind") != "registered" || !System.Text.RegularExpressions.Regex.IsMatch(code, "^[1-9][0-9]{7}$")) throw new IOException("Invalid computer registration.");
                    lock (store.Sync) { if (store.Data.RelayComputerCode != code) { store.Data.RelayComputerCode = code; store.Save(); } }
                    status("Ready · Computer ID " + code);
                    Online = true;
                    heartbeatStop = CancellationTokenSource.CreateLinkedTokenSource(stop.Token); var alive = socket; var heartbeatToken = heartbeatStop.Token;
                    heartbeat = Task.Run(async delegate {
                        try { while (true) { await Task.Delay(10000, heartbeatToken); alive.SendJson(new { operation = "heartbeat" }); } }
                        catch { alive.Dispose(); }
                    });
                    while (!stop.IsCancellationRequested && host.Running && store.Data.Unattended)
                    {
                        var message = socket.ReadJson(); string kind = Wire.Text(message, "kind");
                        if (kind == "heartbeat") continue;
                        if (kind != "session") throw new IOException("Invalid relay notification.");
                        var details = message;
                        QueueTunnel(details);
                    }
                }
                catch (Exception)
                {
                    if (!stop.IsCancellationRequested) status("Waiting for the server · check your internet connection.");
                }
                finally
                {
                    if (heartbeatStop != null) heartbeatStop.Cancel(); if (socket != null) socket.Dispose(); current = null;
                    Online = false;
                }
                if (heartbeat != null) try { await heartbeat; } catch { }
                if (heartbeatStop != null) heartbeatStop.Dispose();
                if (!stop.IsCancellationRequested) try { await Task.Delay(5000, stop.Token); } catch (OperationCanceledException) { }
            }
        }
        void QueueTunnel(System.Collections.Generic.Dictionary<string, object> details)
        {
            Task.Run(delegate {
                            RelaySocket tunnel = null;
                            try { tunnel = RelaySocket.Tunnel(details); if (stop.IsCancellationRequested) return; host.AcceptRelay(tunnel); tunnel = null; }
                            catch { }
                            finally { if (tunnel != null) tunnel.Dispose(); }
            });
        }
        public void Dispose() { stop.Cancel(); var socket = current; if (socket != null) socket.Dispose(); }
    }
    public sealed partial class Remote
    {
        internal Device PairPin(string code, string pin)
        {
            if (!PinGate.Valid(pin)) throw new ArgumentException("Enter your Windows PIN (6–12 digits).");
            var target = RelaySocket.Lookup(code); string fingerprint = Wire.Text(target, "fingerprint");
            using (var connection = RelaySocket.Secure(RelaySocket.Tunnel(target), fingerprint))
            {
                Authenticate(connection, new { version = 1, operation = "pair-pin", id = store.Id, publicKey = store.PublicKey, name = store.Data.Name, pin = pin }, fingerprint);
                var reply = connection.ReadJson(); if (Wire.Text(reply, "kind") != "paired") throw new UnauthorizedAccessException("PIN declined or unattended access is disabled.");
                connection.SendJson(new { kind = "paired-received" });
                var device = new Device { Id = Wire.Text(target, "id"), Name = Util.Name(Wire.Text(target, "name")), RelayCode = code, Fingerprint = fingerprint, Control = (bool)reply["control"] };
                lock (store.Sync) { store.Data.Devices.RemoveAll(d => d.Id == device.Id); store.Data.Devices.Add(device); store.Save(); }
                return device;
            }
        }
    }
}
