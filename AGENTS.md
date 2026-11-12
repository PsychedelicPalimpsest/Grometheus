# grom: project direction and agent instructions

## Purpose and current state

grom is an Android multiboot platform with independently encrypted OS slots
selected by a credential entered before Android boots. Its deniability goal is
to conceal additional independently credentialed installations, not to conceal
the presence of grom itself.

The repository contains `gromui`: a C11 display library, an LVGL adapter, and
an emulator init harness, plus a pinned Magisk v30.4 source fork and a grom
preboot hook compiled into `magiskinit`. The harness is not production init.
`grombake.py` automates the static combined build, image copy patching, and
verification. It does not flash devices. Patched images still stop in preboot.
The combined binary is an integration scaffold that starts a supervised LVGL
display test and waits in preboot; it does not boot Android yet. Touch reaches
the displayed password field, but the inert backend rejects authentication.
Credential processing, charger policy, encryption,
and provisioning are not implemented. Device driver profiles live in
`grom/devices`; `grombake.py` can bundle ordered early modules and firmware
from a supplied source tree. The preboot loader unlinks staged module files
after attempting to load them. On the Moto G Fast Android 10 test boot,
the Himax touchscreen registers. A protocol-B decoder fix for coordinates
reported before a tracking ID was tested on the device: stationary keyboard
taps now reach LVGL, and the user confirmed physical typing works on a
temporary debug boot using firmware extracted from the phone's vendor
partition. The opt-in USB ADB image was booted on the device and `adb shell id`
returned root.
Read `gromui/README.md` before working on
that component; preserve its documented library and executable boundaries.

This file records agreed product intent and engineering constraints. Do not
treat the exploratory Android/Magisk conversation as an authoritative technical
specification. Clearly distinguish implemented behavior, verified facts,
proposals, unresolved decisions, and horizon goals.

## Boot architecture

- Compile grom and a minimally modified MagiskInit fork into the same binary.
  A separate wrapper executable is not the selected architecture.
- Keep grom logic modular and the Magisk fork changes limited to explicit
  integration hooks. Avoid duplicating Magisk's boot machinery unnecessarily.
- grom owns initial interaction, including charger mode and credential entry.
  Once a slot is selected and its required storage setup is ready, return
  control to MagiskInit. Hook later stages where necessary.
- Establish hook ordering against the actual target's boot path. Intervene
  before stock init consumes any partition or metadata that must be redirected.
- Preserve PID 1 responsibilities, resource ownership, and reliable handoff.
  Display rendering failures must not kill init or trigger unsafe boot fallback.
- Do not assume integrating MagiskInit automatically supplies every Magisk
  daemon, module, Zygisk, or root-management behavior.

## Delivery scope

1. Android SDK emulator: prove a complete boot path through the combined binary
   into two independently encrypted environments, each with its own `/data` and
   `/metadata`, sharing one compatible system image. Include charger behavior
   and safe failed-unlock handling.
2. Moto G Fast: validate on the user's physical test device. The supplied fstab
   is a reference from that device, not proof of its complete boot behavior or
   kernel capabilities.
3. Horizon: support every device Magisk supports. This is an aspiration, not a
   compatibility guarantee or a reason to claim untested devices work.

Mixed ROMs and optional Magisk runtime profiles follow the initial milestone.
Their precise behavior remains open. No custom kernel or unsigned-module
availability may be silently assumed; capability gaps must be explicit.

## Security model and limits

- Unlocking one independently credentialed slot must not reveal a global slot
  inventory or other slots' allocation metadata, keys, or credentials.
- No master account or credential decrypts or enumerates every slot.
- Initial targets are a single offline storage image and examination of a
  disclosed running OS under a bounded privilege model, including active RAM
  dumps. This is a requirement to validate, not an already established guarantee.
- After OS handoff, RAM must retain only the selected slot's required secrets;
  it must not retain other slots' decrypted metadata, keys, or credentials.
- Privileged Magisk modules are trusted in the initial scope. Malicious modules,
  kernel compromise, and replacement of the trusted boot software are outside
  the initial live-isolation guarantee. Cryptography alone cannot stop malicious
  privileged code capturing future credentials or destroying ciphertext.
- Constrained privileges remain the intended supported isolation model. Do not
  claim unrestricted developer root is compatible with enforced live storage
  isolation. Any future unrestricted mode needs explicit weaker guarantees.
- Repeated storage snapshots, storage-controller/FTL inspection, and TEE
  forensics are separate research goals. Disabling discard does not establish
  protection against all these observers.
- Strong passphrases underpin offline guessing resistance. Short PINs may be
  supported for convenience, but a costly KDF does not create missing entropy.
- Booting a different slot initially requires reboot. Residual memory across
  reboot must be investigated and tested, not assumed erased.

## Authentication and selection

- Before authentication, expose only the shared password/PIN prompt, plus the
  necessary charger behavior. Do not expose a slot list or provisioning entry.
- Evaluate both the admin verifier and slot authentication before deciding the
  destination. A first match must not silently suppress other matches.
- One slot match boots that slot. An admin-only match opens provisioning.
- Multiple matches display a selection menu, including Administration if the
  admin verifier matches. Show only matching destinations.
- Slot display labels live inside encrypted slot metadata. Never list labels
  for unmatched slots. No match produces a generic error.
- Credential reuse is supported: slots sharing a credential share an unlock
  boundary. Authentication may temporarily expose all matching slots. Do not
  promise independence between slots sharing a credential.
- Before OS handoff, erase the entered credential, derivation state, temporary
  plaintext, and all unselected slots' keys and metadata. Retain only secrets
  needed to operate the selected slot.
- Use deliberate secret-lifetime management and erasure mechanisms that cannot
  be optimized away. Account for copies, UI buffers, error paths, and helper
  processes. Do not log credentials, keys, decrypted headers, or allocation
  information, or persist them in diagnostic artifacts.

## Administration and provisioning

The admin PIN prevents casual unauthorized provisioning and accidental data
loss. It does not protect encrypted contents and unlocks no slot data.

- Store a salted, deliberately expensive admin verifier in grom configuration,
  separate from encrypted slot entries. Its discoverability is acceptable.
- Set it during installation. Provide an explicit external USB maintenance
  mechanism to replace it; fastboot flashing a dedicated configuration partition
  is a possible implementation, not an agreed universal device layout.
- Reset changes only the verifier and must preserve slot headers and the pool.
  It grants no decryption or inventory capability. Flash-capable adversaries
  are outside this casual safeguard's purpose.
- Provisioning never starts automatically because a slot failed to unlock.

During creation, the user temporarily supplies credentials for existing slots:

1. Use each supplied credential to authenticate matching slot metadata and
   retrieve its occupied extents.
2. Add those extents to an anonymous union of occupied ranges. Separately retain
   display labels for slots authenticated by credentials supplied in this
   provisioning session, and show them in an on-screen preservation list.
   Labels may remain in RAM for this session; do not retain their association
   with extent groups, credentials, keys, or full decrypted payloads. Never
   enumerate unmatched slots.
3. Erase the credential, derived keys, decrypted payloads, and temporary copies
   before displaying the next credential prompt, except for the explicitly
   retained display labels and accumulated occupied-range union.
4. Allocate around the accumulated ranges. The creation UI has no manual
   range-reservation screen.
   Require acknowledgement that omitted slots may be overwritten. Remaining
   space is not verified free space.
5. Erase accumulated ranges and retained labels when provisioning finishes or
   is cancelled. The completion flow offers reboot only, with no direct boot
   into an OS.

The accumulated occupied-range union and preservation-label list exist only in
RAM and must never be written to disk. The new slot's own allocation metadata
belongs inside its encrypted metadata; it must not contain an inventory of
other slots.

Provisioning is an explicit exception to the ordinary-boot RAM guarantee: a RAM
dump during it can reveal accumulated protected ranges, retained display labels,
and secrets currently being processed. Immediate secret erasure limits exposure;
it cannot undo a dump already taken.

## Storage and cryptography constraints

- Independently credentialed slots have independent secrets and allocation
  metadata. Random-looking storage must not be treated as proof of vacancy.
- Never introduce a plaintext/global allocation map or master slot registry to
  simplify provisioning. User-supplied allocation knowledge is intentional.
- Never perform speculative chaff writes to supposedly unused ranges: they
  could destroy undisclosed slots.
- Keep header geometry, entry count/size, cipher suites, KDF parameters, extent
  format, and mapping strategy open until designed and validated. The proposed
  128 entries, 512-byte entries, and Argon2id/HKDF/AEAD scheme are candidates,
  not frozen requirements.
- Use established cryptographic constructions and libraries. Specify nonce,
  key-separation, sector/IV, integrity, bounds, and crash-consistency behavior
  before treating an on-disk format as stable.
- Prevent discard from reaching protected backing storage through every
  supported path, and verify this. Do not equate this control with complete
  flash-forensics resistance.
- Probe actual kernel capabilities. Device mapper availability does not prove
  `dm-crypt` availability; `dm-default-key` is not interchangeable by assumption.
  Userspace block fallbacks likewise require explicit feasibility validation.
- Validate mapping ranges, overlap, arithmetic, alignment, and backing-device
  identity before writing. Never infer safe allocation from authentication
  failure or random-looking bytes.

## Android integration constraints

- Discover and verify the actual fstab sources and consumers for each boot
  stage, including ramdisk, device-tree, vendor/odm, and init-script paths as
  applicable. Patch before consumption; do not assume one file controls boot.
- Handle device-node recreation and coldboot. Unlinking nodes or replacing
  symlinks alone does not prevent underlying storage access.
- SELinux `neverallow` is a policy assertion, not a runtime overriding deny
  rule. Review effective permissions and policy modification authority when
  implementing access restrictions.
- Account for whole-disk aliases, existing descriptors, device creation, and
  device-mapper control when claiming raw-storage isolation.
- Do not assume removing Android FBE flags or substituting software KeyMint,
  Gatekeeper, or Weaver is universally safe. Validate per-slot state handling,
  hardware interaction, and framework requirements on supported targets.
- Mixed ROMs require validation of actual framework/vendor/kernel/module and
  SELinux compatibility. A shared Android version or Treble support is not
  sufficient evidence.
- Do not promise invisibility, Play Integrity outcomes, or stock-equivalent
  behavior merely because a manager app, `su` entry, or daemon is absent.

## Failure, charger, and update policy

- Charger mode charges without unlocking an OS slot.
- Normal boot waits for credentials. Authentication and initialization failures
  stay in preboot with appropriate retry or shutdown behavior.
- Never fall through to mounting, repairing, or formatting the raw encrypted
  pool after failure. Prevent stock recovery/format paths from being invoked as
  an automatic consequence of failed grom setup.
- Initially block ordinary in-OS OTA installation on supported configurations.
  Disabling a Settings UI alone is not sufficient evidence of this control.
- Updates require explicit maintenance preserving the encrypted pool and
  validating the combined boot chain and shared-image compatibility.
- Treat hardware A/B slots separately from encrypted OS slots. Account for
  Virtual A/B snapshot state and merge ownership before supporting updates.
- Automated OTA survival is future work. Never assume flashing new firmware
  leaves every encrypted environment compatible or bootable.

## Working and verification rules

- Keep changes scoped to the requested work; do not silently turn proposals
  into permanent format or security decisions.
- Consult the pinned Magisk/AOSP source and actual target artifacts when
  choosing hooks. Record versions and evidence for compatibility claims.
- Use emulator testing for boot sequencing and failure behavior; distinguish
  it from physical-device proof. Cross-compilation alone is not device testing.
- The opt-in `grombake.py --debug-adb` image carries an unauthenticated root
  USB shell for preboot diagnosis. It is an explicit debug exception to the
  intended pre-authentication interface and must never be treated as a secure
  deployment image. Normal images omit the ADB payload.
- Test security-relevant behavior directly: slot separation, ambiguous matches,
  malformed metadata, bounds, cleanup, provisioning allocation, interruption,
  and failure paths. Report what the tests establish and what remains unknown.
- Use disposable images for storage tests. Never write to real device storage
  merely to investigate behavior without explicit authorization for that work.
- For gromui changes, use its documented build/test workflow and required
  display evidence. Do not mistake a rendered demo button for implemented input.
- Keep this file aligned with agreed decisions as implementation develops.

## Technical references

These sources clarify mechanisms; verify against the version being integrated.

- Magisk boot and runtime internals:
  https://topjohnwu.github.io/Magisk/details.html
- Magisk tools and policy patch operations:
  https://topjohnwu.github.io/Magisk/tools.html
- Android SELinux model:
  https://source.android.com/docs/security/features/selinux
- Android metadata encryption:
  https://source.android.com/docs/security/features/encryption/metadata
- Android framework/vendor compatibility matching:
  https://source.android.com/docs/core/architecture/vintf/match-rules
