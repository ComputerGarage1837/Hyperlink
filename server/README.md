# Hyperlink family controller

PHP 8.4 API for the existing family hosting account. This service holds account,
device and authorization metadata. It does not relay desktop media.

Implemented: username-bound expiring invitations, mandatory TOTP enrollment,
single-use recovery codes, Argon2id password hashing, RSA DPoP login binding,
one-use request proofs, rotating refresh tokens with reuse revocation, named
device grants with independent rights and expiry, owner-only sharing, signed
single-use session tickets, 30-second leases with a five-second renewal interval,
account/login/grant/host revocation, and 30-day metadata audit retention.

Recovery revokes existing logins and invalidates viewer trust. Existing owned
hosts require explicit MFA and proof of their existing key before renewed trust.
An account administrator cannot grant access to somebody else's host.

## Development checks

```sh
composer install
composer audit
php tests/controller.php
php tests/api.php
```

The HTTP suite uses a loopback-only CLI router to simulate HTTPS termination.
The production entry point rejects plain HTTP. Never deploy `tests/` publicly.
The optional Windows-generated proof fixture exercises interoperability between
the native .NET RSA signing code and the maintained PHP JWT verifier.

## Deployment when the domain is ready

1. Install the locked production dependencies with `composer install --no-dev`.
2. Keep `src/`, `vendor/`, `bin/`, configuration, keys and the database outside
   the document root. Only `public/` is the web document root. Preserve the
   relative placement of `public/`, `src/` and `vendor/`.
3. Run `php bin/setup.php /home/perfectcomputer/hyperlink-private
   https://hyperlink.myfamilyapps.ca ADMIN_USERNAME` over authenticated SSH.
   Use a new private directory. The initial invitation is shown only to the
   operator and expires after 24 hours. Do not put it in GitHub or deployment logs.
4. Set Apache `HYPERLINK_CONFIG` to that private directory's `config.php`.
   Enable the hosting provider's valid domain TLS certificate. This is separate
   from Windows code signing, which the owner has waived for personal use.
5. Schedule `php bin/cleanup.php /PRIVATE/config.php` hourly. Back up private
   policy data and its encryption key together using restricted access.
6. Test the live HTTPS endpoints, account isolation and native sign-in before
   enabling remote engine admission.

SQLite is the tested first deployment database. PDO MariaDB configuration is
supported by the SQL layout but has not been exercised against a live MariaDB
instance. No MariaDB installation is required for this draft.

## Restore and signing-key changes

Keep the site offline during restore. Before serving a restored database run
`php bin/invalidate-restored-policy.php /PRIVATE/config.php`. This invalidates
old logins and trust epochs so a backup cannot automatically revive revoked
access. Host and viewer trust must be deliberately renewed. Test the restore
procedure before relying on unattended access.

Signing-key rotation must be explicitly accepted by enrolled hosts. The API
provides an RSA public JWK over validated HTTPS. Automatic acceptance of a
replacement key by the native transport is not implemented or permitted.

## Integration boundary

The native account page works with these API contracts but the domain has not
been deployed yet. The high-performance remote engine, host-side ticket and
lease enforcement, local consent policy, opaque relay, unattended Windows
service and Android viewer remain in development. A server ticket alone is
insufficient authorization: the host must verify the ticket, both endpoint
keys, current policy and its independently maintained owner-approved grants.

For this reason, enrolling the current Windows draft disables its temporary
direct-pairing host route. It cannot open an alternate account bypass while
hosted transport is unfinished. The current media draft is capped at 30 fps;
there is no 120 fps qualification claim.

No screen, input, clipboard, file contents, password, authenticator seed or
recovery code belongs in audit logs. TOTP seeds are encrypted at rest under a
separate private server key; login tokens and recovery codes are stored as
keyed hashes. Request body and authorization-header logging must be disabled
in hosting diagnostics.
