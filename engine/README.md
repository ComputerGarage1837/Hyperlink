# Native-engine evaluation

Candidate: RustDesk 1.4.9 at commit
`6c578292e8ebbbec708b76986ba8c4bc7c509747`.
Source: https://github.com/rustdesk/rustdesk/tree/6c578292e8ebbbec708b76986ba8c4bc7c509747

The engine is not bundled with the Windows draft. The patch in `patches/`
is an evaluation change, not a production security qualification.

## Audited finding and patch

`src/client.rs::secure_connection` has multiple success paths that do not set
an authenticated encryption key: absent/invalid rendezvous signature, bad peer
signature or identity, missing/wrong message type, malformed message, and a
public-key mismatch fallback. The direct and alternate-route callers inspected
propagate handshake errors; fixing only a warning in the UI would be insufficient.

The evaluation patch requires an explicit private rendezvous identity key,
authenticates both signed peer-identity messages, checks both peer IDs, rejects
every malformed or missing-key branch, and returns success only after installing
the negotiated session encryption key. It removes the empty-message and empty
public-key downgrade messages and the compiled public-key fallback in this
function.

The patch was verified against the pinned source with `git apply --reverse
--check`. Rust, Cargo and the native C/C++ build toolchain are not currently
available in this workspace; it has not been compiled or runtime-tested.
All other incoming/outgoing routes and data channels still need audit.

## Required before adoption

- Enforce Hyperlink ticket/key binding and independent local owner grants inside
  engine admission, with one-use redemption and renewable revocation leases.
- Audit direct IP, raw IDs, LAN discovery, saved passwords, compatibility modes,
  public fallback, host authentication and every data channel.
- Run missing/wrong-key, forged-ticket, replay, hostile-relay, downgrade,
  expired-lease and revocation tests over direct and forced-relay paths.
- Prove distinct 1080p120 presentation and input latency on qualifying Windows
  and Android hardware; an upstream fps setting does not qualify the product.
- Verify a compatible private rendezvous/relay deployment. Shared PHP hosting
  alone does not establish daemon support.
- Review upstream/dependency licensing and publish corresponding modified source
  and notices when distributing the engine.

Upstream RustDesk uses AGPL-3.0. Its license is preserved as
`LICENSE-RustDesk`. No RustDesk code is compiled into the current native draft.
