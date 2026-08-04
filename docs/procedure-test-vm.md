# Testing in a VM

**Rule: never install a development APO on a working machine.** An APO with a bug
can crash `audiodg.exe` in a loop, which means no sound at all on that machine —
including during the debugging session that would fix it. Use a Windows 11 VM, or
a dedicated machine you are willing to lose.

## 1. Preparing the VM

1. A Windows 11 x64 VM (Hyper-V Quick Create, VMware, VirtualBox) with virtual
   audio enabled:
   - Hyper-V: enhanced session (`vmconnect` with audio redirection);
   - VMware: `sound.present = TRUE`; VirtualBox: audio controller + host mic.

   You need **at least one capture endpoint** visible in Settings > Sound.
2. Install the build tools (VS Build Tools C++ and CMake) **or** build on the host
   and copy the effect pack into the VM.
3. Take a **clean checkpoint/snapshot** of the VM.
4. Inside the VM, create a Windows restore point, then export the sensitive keys
   by hand — belt and braces, the installer takes its own backups too:

   ```powershell
   reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture" C:\backup-capture.reg /y
   reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio" C:\backup-audio.reg /y
   ```

## 2. Order of operations, the first time

> **Test the uninstaller before the first install.**

```powershell
cd installer\effectpack

# (a) On a clean VM: must report nothing to remove and exit 0.
.\pack.ps1 -Action uninstall

# (b) Install.
.\pack.ps1 -Action install -PackageDir package

# (c) Choose Kwiet in Settings > Sound > [microphone] > Audio enhancements.
#     Installing does not enable it; nothing in the audio stack says so.

# (d) Functional test (see §3).

# (e) Remove immediately, and check the machine returned to its initial state.
.\pack.ps1 -Action uninstall
reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture" C:\after-uninstall.reg /y
fc C:\backup-capture.reg C:\after-uninstall.reg
```

Only once that install → uninstall → clean diff cycle passes should real test
sessions begin.

## 3. Checking that the APO loads

1. Open Voice Recorder (or the microphone test in Settings) to create a capture
   stream.
2. Open the Kwiet panel. **A stream visible in the panel is the only reliable
   instrument.** Two tests that look convincing and prove nothing:
   - *"The DLL is loaded in audiodg"* — DLLs stay mapped long after they stop
     being used;
   - *"No new log lines"* — a stream already locked writes nothing while running.

   Both produced false negatives repeatedly during development.
3. With the effect bypassed, the recorded sound must be identical to the input.

If it will not load, check in order: is the pack selected in audio enhancements →
is the driver package in the store (`pnputil /enum-drivers`) → does the panel say
"effect pack missing" → Event Viewer (Application log, *Audio* and
*AudioEndpointBuilder* sources, and `audiodg.exe` crashes).

## 4. Stability matrix

The APO must survive all of the following **without crashing audiodg and without
losing audio**. Falling back to passthrough is fine; permanent silence is not.

| Test | Procedure |
|---|---|
| Sample-rate change | Device properties > Advanced: alternate 44.1 kHz / 48 kHz during a recording |
| Hot-plug | Unplug and replug the USB microphone, or disable and re-enable the device in Device Manager while a stream is active |
| Sleep/resume | Sleep → resume with an active stream (or save/restore the VM, depending on the hypervisor) |
| Several apps | Chrome (meet.google.com) and Discord simultaneously for at least an hour |
| Modes | One communications stream (Discord) and one default stream (Voice Recorder) in parallel |
| Soak | 48 h with an active capture stream; watch for audiodg crashes in Event Viewer and for memory growth (`Get-Process audiodg`) |

## 5. Emergency recovery

If the VM's audio is dead after an install:

1. `pack.ps1 -Action uninstall`, then restart the stack:
   `Restart-Service AudioEndpointBuilder -Force ; Start-Service Audiosrv`.
2. If audiodg crashes in a loop and blocks everything: rename the DLL in the
   driver store (safe mode if necessary), reboot, then `reg import` the backups.
3. Last resort: the Windows restore point, or the VM checkpoint.

Note: after several audiodg crashes Windows may disable the endpoint's audio
enhancements on its own. Re-enable them once the fix is in.

## 6. Reminders

- Shared mode only: exclusive and RAW streams bypass the APO, by design.
- Take a fresh clean checkpoint after every validated change to the install
  procedure.
