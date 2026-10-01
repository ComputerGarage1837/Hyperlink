using System;
using System.IO;
using System.Net;
using System.Threading;
namespace Hyperlink {
static class AutoUpdateTests {
internal static string Run(string folder) {
using(var store=new Store(folder)) using(var host=new Host(store,delegate {return 0;},delegate {},true)) {
if(!store.Data.AutoCheckUpdates||!store.Data.AutoInstallUpdates)throw new Exception("New installs should enable automatic updates.");
host.Start(IPAddress.Loopback,0);
using(var pending=Wire.Connect("127.0.0.1",host.Port,store.Fingerprint)) {
for(int n=0;n<50&&!host.UpdateBusy;n++)Thread.Sleep(20);
bool started=false;
if(!host.UpdateBusy||host.BeginAutomaticUpdate(delegate {started=true;})||started||!host.Running)throw new Exception("Automatic update interrupted pending admission.");
}
for(int n=0;n<100&&host.UpdateBusy;n++)Thread.Sleep(20);
bool idle=false;if(!host.BeginAutomaticUpdate(delegate {idle=true;})||!idle||host.Running)throw new Exception("Idle automatic update could not stop admission.");
}
return "Automatic-update admission checks passed: defaults enabled, pending pairing protected and idle shutdown allowed.";
}
}
}
