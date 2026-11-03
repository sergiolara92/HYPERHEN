# HyperHEN v0.9.5

HyperHEN 0.9.5 adds the confirmed 12.70 startup correction, manual PRX/SPRX
game-module loading and ShadowMountPlus 1.7 beta11. Hyper's 7B Q5_K_M model
has now completed a spoken reply, while remaining experimental with high
latency and significant memory use.

## 12.70 startup correction

- The exact 12.70 route checks Bypervisor completion before launching its
  dedicated HyperCore and retains the route's no-PPR startup contract.
- The dedicated core includes the upstream `store_rax_rdi` scalar-store
  correction from [kstuff-lite](https://github.com/EchoStretch/kstuff-lite/commit/fe7fef639aafbbdacc73cf9f4f49a45f6798106d).
  The R48 console test completed the HyperCore capability check, manual
  WebKit-close gate, Toolbox startup and HyperDeck registration on 12.70.
  Automount startup was observed; that capture does not validate every
  individual mount operation.
- **The standalone Bypervisor 12.70 ELF is unchanged.** The correction is
  in the dedicated HyperCore embedded in HyperHEN; no replacement Bypervisor
  download is needed for this fix.
- The 13.00/13.20/13.60 HyperCore path and corrected 13.60 native runtime
  were already included in 0.9.4. They are preserved, not new 0.9.5 fixes.
  The other Kstuff / HyperCore engines retain their previous inputs.

## Manual PRX / SPRX modules

1. Copy the compatible `.prx` or `.sprx` directly to
   `/data/HyperHEN/plugins/` or `/data/HyperHEN/sprx/`. These directories
   are prepared during startup.
2. Start the exact matching game and update, and enter the game.
3. Open **HyperHEN Toolbox → Plugins → SPRX**, select the file and choose
   **Load in current game**. Return to the game to complete loading.
4. For **RaiDougie 1.56.2**, use **L1 + □ (Square)** to open its menu.

**Confirmed:** RaiDougie1.56.2.prx in **GTA V CUSA00411 v1.56, FW 10.01**
with the R71 development build. The log confirms initializer completion and
the tester confirms the menu works. This does not validate other plugins,
regions, updates or firmware layouts. Game modules load manually; close the
game to unload them. Removing a catalog entry keeps the file and does not
unload a loaded module. Descriptor-bearing plugin ELFs retain their separate
Plugins workflow.

## Automount beta11

Both standard and restricted lite Automount now use **ShadowMountPlus
1.7 beta11**. HyperHEN's translated notifications, saved configuration,
single-owner startup handoff and exact-profile restrictions are retained.
The declared firmware table remains **67 exact profiles**; this update does
not establish testing on every profile.

## Hyper — voice progress and practical limits

- Hyper stays on the main Toolbox page directly below **Content Install &
  Management**. Hold **L1 + R1 + □** to speak through the DualSense
  microphone and hear the answer from the controller speaker.
- The built-in installer downloads models and voice data into `/data/Hyper`;
  no PC or online voice service is required after installation. Downloads
  need an internet connection. Existing models can be kept when updating.
- **Qwen2.5 0.5B Q4_K_M remains the recommended, console-confirmed starting
  point.** The remaining catalog entries stay beta; **7B Q5_K_M** has now
  completed an Italian spoken turn on FW 10.01 in R51, taking about
  **138 seconds** overall. That is one recorded turn, not a general latency
  guarantee.
- The retained R52 worker optimizes CPU callbacks and weight access, keeping
  the 512 MiB weight cache and conversation-prefix reuse. Local checks do
  not establish a new R52 console speed result. Inference uses the CPU;
  7B is still experimental and very slow. The cache is not the assistant's
  total RAM requirement.
- **Disable Hyper and let its model unload before launching demanding games
  or testing plugins.** In a reported test a resident 7B model was associated
  with a GTA V black screen with audio; the game opened with Hyper disabled.
  Concurrent 7B/game operation has not been validated.

See the [Hyper guide](https://github.com/Michele-M-Media/HYPERHEN#hyper--local-voice-assistant-beta) for model
sizes, installation, voice selection and the enable switch.

## Startup order and retained services

For **9.40, 12.70, 13.00, 13.20 and 13.60**, keep this order:

1. Open WebKit and launch the matching Bypervisor ELF.
2. Enter Rest Mode, then resume the console.
3. Open WebKit again and launch HyperHEN.
4. **Close WebKit manually**, keeping it closed through the **two-second
   quiet interval** and the remaining Automount/Toolbox initialization.

The unchanged standalone Bypervisor downloads are in the repository and
linked from the [README](https://github.com/Michele-M-Media/HYPERHEN#bypervisor-downloads-and-startup-procedure).
They are not additional 0.9.5 release assets. The 5.xx integrated selector
and 13.40/13.42 lite route retain their existing separate procedures.

HyperDeck, Advanced Debug tools, firmware selectors, existing translations
and the isolated native runtime with the 13.60 initializer are retained.

- **Product labels and translated version notifications updated to HyperHEN v0.9.5.**

## Credits and licenses

All existing developer and component credits remain in the
[README](https://github.com/Michele-M-Media/HYPERHEN#credits). Particular thanks for the plugin work to
[etaHEN](https://github.com/LightningMods/etaHEN),
[GronedWaffel's etaHEN community port](https://github.com/GronedWaffel/etahen-11.00-13.60),
[etaHEN-Plugins](https://github.com/etaHEN/etaHEN-Plugins) and the
[GoldHEN Plugins SDK authors](https://github.com/GoldHEN/GoldHEN_Plugins_SDK).
Adapted code and third-party components retain their licenses and notices;
see [THIRD_PARTY_NOTICES.md](https://github.com/Michele-M-Media/HYPERHEN/blob/main/THIRD_PARTY_NOTICES.md).

Special thanks to [**@oG_LuckyXmods**](https://x.com/oG_LuckyXmods) for the
extensive testing support, and to the [**PizzaClub testers**](https://t.me/+6SY-QLIzDkI1Mzg0)
for their time, feedback and firmware tests.
