<?php
declare(strict_types=1);

namespace Hyperlink;

use Firebase\JWT\{JWT, JWK, Key};
use OTPHP\TOTP;
use PDO;
use RuntimeException;

final class Denied extends RuntimeException {}

/** The controller stores authorization metadata only, never desktop content. */
final class Controller
{
    public const RIGHTS = ['view', 'control', 'file_read', 'file_write', 'clipboard_to_host',
        'clipboard_from_host', 'audio', 'restart', 'printing', 'recording', 'unattended'];
    private PDO $db;
    private string $secret;
    private string $signingKey;
    private string $publicKey;
    private string $baseUrl;
    private int $now;

    public function __construct(array $config, ?int $now = null)
    {
        $this->secret = file_get_contents($config['secret_file']) ?: throw new RuntimeException('Missing server secret');
        if (strlen($this->secret) !== 32) throw new RuntimeException('Invalid server secret');
        $this->signingKey = file_get_contents($config['signing_key_file']) ?: throw new RuntimeException('Missing signing key');
        $key = openssl_pkey_get_private($this->signingKey);
        $this->publicKey = openssl_pkey_get_details($key)['key'];
        $this->baseUrl = rtrim($config['base_url'], '/');
        if (!str_starts_with($this->baseUrl, 'https://')) throw new RuntimeException('HTTPS base URL required');
        $this->now = $now ?? time();
        $this->db = new PDO($config['dsn'], $config['db_user'] ?? null, $config['db_password'] ?? null,
            [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC]);
        if ($this->db->getAttribute(PDO::ATTR_DRIVER_NAME) === 'sqlite') {
            $this->db->exec('PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000; PRAGMA journal_mode = WAL;');
        }
        $this->schema();
    }

    private function schema(): void
    {
        // Portable SQL: SQLite locally, MariaDB on hosting when configured.
        foreach ([
            'users (id VARCHAR(64) PRIMARY KEY, name VARCHAR(64) UNIQUE NOT NULL, password VARCHAR(255) NOT NULL, mfa TEXT NOT NULL, last_otp BIGINT NOT NULL DEFAULT -1, active INTEGER NOT NULL DEFAULT 0, pending_until BIGINT NOT NULL, admin INTEGER NOT NULL DEFAULT 0, epoch INTEGER NOT NULL DEFAULT 1)',
            'invites (token VARCHAR(64) PRIMARY KEY, name VARCHAR(64) NOT NULL, expires BIGINT NOT NULL, actor VARCHAR(64) NOT NULL)',
            'recovery (user_id VARCHAR(64) NOT NULL, hash VARCHAR(64) PRIMARY KEY)',
            'viewers (id VARCHAR(64) PRIMARY KEY, user_id VARCHAR(64) NOT NULL, jkt VARCHAR(64) NOT NULL, label VARCHAR(100) NOT NULL, epoch INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0)',
            'sessions (id VARCHAR(64) PRIMARY KEY, user_id VARCHAR(64) NOT NULL, jkt VARCHAR(64) NOT NULL, access_hash VARCHAR(64) UNIQUE NOT NULL, access_until BIGINT NOT NULL, refresh_hash VARCHAR(64) UNIQUE NOT NULL, expires BIGINT NOT NULL, revoked INTEGER NOT NULL DEFAULT 0, elevated_until BIGINT NOT NULL, epoch INTEGER NOT NULL)',
            'refresh_history (hash VARCHAR(64) PRIMARY KEY, session_id VARCHAR(64) NOT NULL, expires BIGINT NOT NULL)',
            'proofs (id VARCHAR(128) PRIMARY KEY, expires BIGINT NOT NULL)',
            'attempts (id VARCHAR(64) PRIMARY KEY, failures INTEGER NOT NULL, until_time BIGINT NOT NULL)',
            'devices (id VARCHAR(64) PRIMARY KEY, owner VARCHAR(64) NOT NULL, name VARCHAR(100) NOT NULL, public_key TEXT NOT NULL, fingerprint VARCHAR(64) NOT NULL, epoch INTEGER NOT NULL DEFAULT 1, revoked INTEGER NOT NULL DEFAULT 0, owner_epoch INTEGER NOT NULL)',
            'grants (id VARCHAR(64) PRIMARY KEY, device_id VARCHAR(64) NOT NULL, viewer VARCHAR(64) NOT NULL, rights TEXT NOT NULL, expires BIGINT NOT NULL, revoked INTEGER NOT NULL DEFAULT 0, viewer_epoch INTEGER NOT NULL, UNIQUE (device_id, viewer))',
            'leases (id VARCHAR(64) PRIMARY KEY, device_id VARCHAR(64) NOT NULL, viewer_session VARCHAR(64) NOT NULL, host_session VARCHAR(64) NOT NULL, grant_id VARCHAR(64), rights TEXT NOT NULL, expires BIGINT NOT NULL, redeemed INTEGER NOT NULL DEFAULT 0, device_epoch INTEGER NOT NULL)',
            'audit (id VARCHAR(64) PRIMARY KEY, at_time BIGINT NOT NULL, actor VARCHAR(64) NOT NULL, target VARCHAR(64) NOT NULL, event VARCHAR(64) NOT NULL, result VARCHAR(32) NOT NULL)',
        ] as $definition) $this->db->exec('CREATE TABLE IF NOT EXISTS '.$definition);
    }

    private function query(string $sql, array $args = []): \PDOStatement
    {
        $s = $this->db->prepare($sql); $s->execute($args); return $s;
    }
    private function one(string $sql, array $args = []): array
    {
        return $this->query($sql, $args)->fetch() ?: throw new Denied('Access denied');
    }
    private function transaction(callable $fn): mixed
    {
        $this->db->beginTransaction();
        try { $result = $fn(); $this->db->commit(); return $result; }
        catch (\Throwable $e) { $this->db->rollBack(); throw $e; }
    }
    private static function token(): string { return self::b64(random_bytes(32)); }
    private static function b64(string $bytes): string { return rtrim(strtr(base64_encode($bytes), '+/', '-_'), '='); }
    private static function publicJwk(string $pem): array
    {
        $key = openssl_pkey_get_public($pem);
        if (!$key) throw new Denied('Invalid public key');
        $d = openssl_pkey_get_details($key);
        if ($d['type'] !== OPENSSL_KEYTYPE_RSA || $d['bits'] < 2048 || $d['bits'] > 4096) throw new Denied('Unsupported public key');
        return ['e' => self::b64($d['rsa']['e']), 'kty' => 'RSA', 'n' => self::b64($d['rsa']['n'])];
    }
    private static function thumbprint(array $jwk): string
    {
        return self::b64(hash('sha256', json_encode(['e' => $jwk['e'], 'kty' => 'RSA', 'n' => $jwk['n']], JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR), true));
    }
    private function hash(string $token): string { return hash_hmac('sha256', $token, $this->secret); }
    private function audit(string $actor, string $target, string $event, string $result = 'ok'): void
    {
        $this->query('INSERT INTO audit VALUES (?, ?, ?, ?, ?, ?)', [self::token(), $this->now, $actor, $target, $event, $result]);
    }
    private static function name(string $name): string
    {
        $name = strtolower(trim($name));
        if (!preg_match('/^[a-z0-9][a-z0-9._-]{2,63}$/D', $name)) throw new Denied('Use a username of 3–64 letters, digits, dots, underscores or hyphens');
        return $name;
    }
    private function seal(string $text): string
    {
        $iv = random_bytes(12); $tag = '';
        $encrypted = openssl_encrypt($text, 'aes-256-gcm', $this->secret, OPENSSL_RAW_DATA, $iv, $tag, 'Hyperlink MFA v1');
        if ($encrypted === false) throw new RuntimeException('Encryption failed');
        return base64_encode($iv.$tag.$encrypted);
    }
    private function unseal(string $text): string
    {
        $bytes = base64_decode($text, true);
        if ($bytes === false || strlen($bytes) < 29) throw new RuntimeException('Invalid encrypted secret');
        return openssl_decrypt(substr($bytes, 28), 'aes-256-gcm', $this->secret, OPENSSL_RAW_DATA,
            substr($bytes, 0, 12), substr($bytes, 12, 16), 'Hyperlink MFA v1') ?: throw new RuntimeException('Invalid encrypted secret');
    }

    /** RFC 9449 proof validation. Request URLs come from trusted config, never a Host header. */
    public function proof(string $jwt, string $method, string $path, ?string $accessToken = null): string
    {
        if (strlen($jwt) > 8192 || !preg_match('/^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$/D', $jwt)) throw new Denied('Invalid proof');
        $header = json_decode(JWT::urlsafeB64Decode(explode('.', $jwt)[0]), true, 8, JSON_THROW_ON_ERROR);
        if (($header['typ'] ?? '') !== 'dpop+jwt' || ($header['alg'] ?? '') !== 'RS256') throw new Denied('Invalid proof algorithm');
        $jwk = $header['jwk'] ?? [];
        if (($jwk['kty'] ?? '') !== 'RSA' || isset($jwk['d']) || isset($jwk['p']) || isset($jwk['q'])) throw new Denied('Public RSA proof key required');
        $public = JWK::parseKey($jwk, 'RS256');
        $details = openssl_pkey_get_details(openssl_pkey_get_public($public->getKeyMaterial()));
        if ($details['bits'] < 2048 || $details['bits'] > 4096) throw new Denied('Unsupported proof key size');
        $canonicalJwk = self::publicJwk($details['key']);
        if (($jwk['e'] ?? '') !== $canonicalJwk['e'] || ($jwk['n'] ?? '') !== $canonicalJwk['n']) throw new Denied('Noncanonical proof key');
        $claims = (array) JWT::decode($jwt, $public);
        if (!is_int($claims['iat'] ?? null) || abs($this->now - $claims['iat']) > 60 ||
            ($claims['htm'] ?? '') !== strtoupper($method) || ($claims['htu'] ?? '') !== $this->baseUrl.$path ||
            !is_string($claims['jti'] ?? null) || !preg_match('/^[A-Za-z0-9_-]{16,100}$/D', $claims['jti'])) throw new Denied('Invalid proof claims');
        if ($accessToken !== null && !hash_equals(self::b64(hash('sha256', $accessToken, true)), $claims['ath'] ?? '')) throw new Denied('Token proof mismatch');
        $jkt = self::thumbprint($canonicalJwk);
        try { $this->query('INSERT INTO proofs VALUES (?, ?)', [hash('sha256', $jkt.$claims['jti']), $this->now + 120]); }
        catch (\PDOException $e) { if ($e->getCode() !== '23000') throw $e; throw new Denied('Replayed proof'); }
        return $jkt;
    }

    /** Run once by the operator over SSH; no public bootstrap endpoint exists. */
    public function bootstrap(string $username): string
    {
        if ((int)$this->query('SELECT COUNT(*) FROM users')->fetchColumn() !== 0 ||
            (int)$this->query('SELECT COUNT(*) FROM invites')->fetchColumn() !== 0) throw new Denied('Bootstrap already completed');
        return $this->invite('operator', $username);
    }
    private function invite(string $actor, string $name): string
    {
        $token = self::token();
        $this->query('INSERT INTO invites VALUES (?, ?, ?, ?)', [$this->hash($token), self::name($name), $this->now + 86400, $actor]);
        $this->audit($actor, self::name($name), 'account_invited'); return $token;
    }
    public function inviteAccount(array $session, string $name): string
    {
        $this->elevated($session);
        $u = $this->one('SELECT * FROM users WHERE id = ?', [$session['user_id']]);
        if (!(int)$u['admin']) throw new Denied('Account administrator required');
        return $this->invite($u['id'], $name);
    }
    public function register(string $invite, string $name, string $password): array
    {
        if (strlen($password) < 12 || strlen($password) > 200) throw new Denied('Password must be 12–200 bytes');
        $name = self::name($name);
        return $this->transaction(function () use ($invite, $name, $password) {
            $i = $this->one('SELECT * FROM invites WHERE token = ? AND name = ? AND expires > ?', [$this->hash($invite), $name, $this->now]);
            if ($this->query('DELETE FROM invites WHERE token = ?', [$i['token']])->rowCount() !== 1) throw new Denied('Invitation already used');
            $otp = TOTP::generate(); $otp->setLabel($name); $otp->setIssuer('Hyperlink');
            $id = self::token();
            $this->query('INSERT INTO users (id, name, password, mfa, pending_until, admin) VALUES (?, ?, ?, ?, ?, ?)',
                [$id, $name, password_hash($password, PASSWORD_ARGON2ID), $this->seal($otp->getSecret()), $this->now + 600, $i['actor'] === 'operator' ? 1 : 0]);
            $this->audit($id, $id, 'mfa_enrollment_started');
            return ['enrollment_id' => $id, 'secret' => $otp->getSecret(), 'uri' => $otp->getProvisioningUri(), 'expires' => $this->now + 600];
        });
    }
    private function otp(array $user, string $code): void
    {
        $otp = TOTP::createFromSecret($this->unseal($user['mfa']));
        $step = intdiv($this->now, 30);
        if (!preg_match('/^[0-9]{6}$/D', $code)) throw new Denied('Invalid authenticator code');
        $accepted = null;
        // RFC 6238's bounded delay window; the library still computes/verifies TOTP.
        foreach ([$step, $step - 1] as $candidate) {
            if ($candidate > (int)$user['last_otp'] && $otp->verify($code, $candidate * 30)) { $accepted = $candidate; break; }
        }
        if ($accepted === null) throw new Denied('Invalid or reused authenticator code');
        if ($this->query('UPDATE users SET last_otp = ? WHERE id = ? AND last_otp < ?', [$accepted, $user['id'], $accepted])->rowCount() !== 1) throw new Denied('Reused authenticator code');
    }
    public function activate(string $id, string $code): array
    {
        return $this->transaction(function () use ($id, $code) {
            $u = $this->one('SELECT * FROM users WHERE id = ? AND active = 0 AND pending_until > ?', [$id, $this->now]);
            $this->otp($u, $code);
            $this->query('UPDATE users SET active = 1 WHERE id = ?', [$id]);
            $codes = [];
            for ($n = 0; $n < 10; $n++) {
                $code = self::b64(random_bytes(16)); $codes[] = $code;
                $this->query('INSERT INTO recovery VALUES (?, ?)', [$id, $this->hash($code)]);
            }
            $this->audit($id, $id, 'account_activated'); return ['recovery_codes' => $codes];
        });
    }
    public function login(string $name, string $password, string $code, string $jkt, string $ip, bool $trustNewViewer = false, string $viewerLabel = 'Personal viewer'): array
    {
        $name = self::name($name); $rate = $this->hash($name.'|'.$ip);
        $accountRate = $this->hash('account-login|'.$name);
        $a = $this->query('SELECT * FROM attempts WHERE id = ?', [$rate])->fetch();
        $global = $this->query('SELECT * FROM attempts WHERE id = ?', [$accountRate])->fetch();
        if ($a && (int)$a['failures'] >= 5 && (int)$a['until_time'] > $this->now) throw new Denied('Try again later');
        if ($global && (int)$global['failures'] >= 10 && (int)$global['until_time'] > $this->now) throw new Denied('Try again later');
        $u = $this->query('SELECT * FROM users WHERE name = ? AND active = 1', [$name])->fetch();
        try {
            if (!$u || !password_verify($password, $u['password'])) throw new Denied('Invalid login');
            $viewerId = hash('sha256', $u['id'].'|'.$jkt);
            $viewer = $this->query('SELECT * FROM viewers WHERE id = ?', [$viewerId])->fetch();
            if ($viewer && (int)$viewer['revoked']) throw new Denied('Viewer key revoked');
            if ((!$viewer || (int)$viewer['epoch'] !== (int)$u['epoch']) && !$trustNewViewer) throw new Denied('Explicit viewer approval required');
            if (mb_strlen($viewerLabel) < 1 || mb_strlen($viewerLabel) > 100 || preg_match('/[\x00-\x1f]/', $viewerLabel)) throw new Denied('Invalid viewer label');
            $this->otp($u, $code);
        } catch (Denied $e) {
            if (!$a || (int)$a['until_time'] <= $this->now) {
                $this->query('DELETE FROM attempts WHERE id = ?', [$rate]);
                $this->query('INSERT INTO attempts VALUES (?, 1, ?)', [$rate, $this->now + 900]);
            } else $this->query('UPDATE attempts SET failures = failures + 1 WHERE id = ?', [$rate]);
            if (!$global || (int)$global['until_time'] <= $this->now) {
                $this->query('DELETE FROM attempts WHERE id = ?', [$accountRate]);
                $this->query('INSERT INTO attempts VALUES (?, 1, ?)', [$accountRate, $this->now + 900]);
            } else $this->query('UPDATE attempts SET failures = failures + 1 WHERE id = ?', [$accountRate]);
            $this->audit($u['id'] ?? 'unknown', $name, 'login', 'denied'); throw new Denied('Invalid login');
        }
        $this->query('DELETE FROM attempts WHERE id = ?', [$rate]);
        $this->query('DELETE FROM attempts WHERE id = ?', [$accountRate]);
        if (!$viewer) {
            $this->query('INSERT INTO viewers (id, user_id, jkt, label, epoch) VALUES (?, ?, ?, ?, ?)', [$viewerId, $u['id'], $jkt, $viewerLabel, $u['epoch']]);
            $this->audit($u['id'], $viewerId, 'viewer_enrolled');
        } elseif ((int)$viewer['epoch'] !== (int)$u['epoch']) {
            $this->query('UPDATE viewers SET epoch = ?, label = ? WHERE id = ?', [$u['epoch'], $viewerLabel, $viewerId]);
            $this->audit($u['id'], $viewerId, 'viewer_trust_renewed');
        }
        $access = self::token(); $refresh = self::token(); $id = self::token();
        $this->query('INSERT INTO sessions VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?)',
            [$id, $u['id'], $jkt, $this->hash($access), $this->now + 600, $this->hash($refresh), $this->now + 2592000, $this->now + 300, $u['epoch']]);
        $this->audit($u['id'], $id, 'login');
        return ['access_token' => $access, 'refresh_token' => $refresh, 'token_type' => 'DPoP', 'expires_in' => 600, 'session_id' => $id];
    }
    public function authenticate(string $access, string $jkt): array
    {
        return $this->one('SELECT s.* FROM sessions s JOIN users u ON s.user_id = u.id JOIN viewers v ON v.user_id = u.id AND v.jkt = s.jkt WHERE s.access_hash = ? AND s.jkt = ? AND s.revoked = 0 AND s.access_until > ? AND s.expires > ? AND u.active = 1 AND s.epoch = u.epoch AND v.revoked = 0 AND v.epoch = u.epoch',
            [$this->hash($access), $jkt, $this->now, $this->now]);
    }
    private function elevated(array $s): void
    {
        if ((int)$s['elevated_until'] <= $this->now) throw new Denied('Fresh MFA verification required');
    }
    public function refresh(string $refresh, string $jkt): array
    {
        $hash = $this->hash($refresh);
        $old = $this->query('SELECT h.* FROM refresh_history h JOIN sessions s ON s.id = h.session_id WHERE h.hash = ? AND s.jkt = ?', [$hash, $jkt])->fetch();
        if ($old) {
            $this->query('UPDATE sessions SET revoked = 1 WHERE id = ?', [$old['session_id']]);
            throw new Denied('Refresh replay revoked this login');
        }
        return $this->transaction(function () use ($hash, $jkt) {
            $s = $this->one('SELECT s.* FROM sessions s JOIN users u ON u.id = s.user_id JOIN viewers v ON v.user_id = u.id AND v.jkt = s.jkt WHERE s.refresh_hash = ? AND s.jkt = ? AND s.revoked = 0 AND s.expires > ? AND u.active = 1 AND s.epoch = u.epoch AND v.revoked = 0 AND v.epoch = u.epoch', [$hash, $jkt, $this->now]);
            $access = self::token(); $refresh = self::token();
            $this->query('INSERT INTO refresh_history VALUES (?, ?, ?)', [$hash, $s['id'], $s['expires']]);
            if ($this->query('UPDATE sessions SET access_hash = ?, access_until = ?, refresh_hash = ? WHERE id = ? AND refresh_hash = ?',
                [$this->hash($access), $this->now + 600, $this->hash($refresh), $s['id'], $hash])->rowCount() !== 1) throw new Denied('Refresh already rotated');
            return ['access_token' => $access, 'refresh_token' => $refresh, 'token_type' => 'DPoP', 'expires_in' => 600, 'session_id' => $s['id']];
        });
    }
    public function revokeSession(array $s, string $id): void
    {
        if ($this->query('UPDATE sessions SET revoked = 1 WHERE id = ? AND user_id = ?', [$id, $s['user_id']])->rowCount() !== 1) throw new Denied('Access denied');
        $this->audit($s['user_id'], $id, 'login_revoked');
    }
    public function elevate(array $s, string $code): void
    {
        $this->otp($this->one('SELECT * FROM users WHERE id = ?', [$s['user_id']]), $code);
        $this->query('UPDATE sessions SET elevated_until = ? WHERE id = ?', [$this->now + 300, $s['id']]);
    }
    public function recover(string $name, string $password, string $recoveryCode): array
    {
        // Recovery resets MFA and all viewer trust. It never creates an authenticated login.
        return $this->transaction(function () use ($name, $password, $recoveryCode) {
            $u = $this->one('SELECT * FROM users WHERE name = ? AND active = 1', [self::name($name)]);
            if (!password_verify($password, $u['password']) || $this->query('DELETE FROM recovery WHERE user_id = ? AND hash = ?', [$u['id'], $this->hash($recoveryCode)])->rowCount() !== 1) throw new Denied('Invalid recovery');
            $otp = TOTP::generate(); $otp->setLabel($u['name']); $otp->setIssuer('Hyperlink');
            $this->query('UPDATE users SET active = 0, epoch = epoch + 1, mfa = ?, last_otp = -1, pending_until = ? WHERE id = ?', [$this->seal($otp->getSecret()), $this->now + 600, $u['id']]);
            $this->query('UPDATE sessions SET revoked = 1 WHERE user_id = ?', [$u['id']]);
            $this->query('DELETE FROM recovery WHERE user_id = ?', [$u['id']]);
            $this->audit($u['id'], $u['id'], 'account_recovered');
            return ['enrollment_id' => $u['id'], 'secret' => $otp->getSecret(), 'uri' => $otp->getProvisioningUri(), 'expires' => $this->now + 600];
        });
    }

    public function enrollDevice(array $s, string $name, string $publicKey, string $fingerprint): array
    {
        $this->elevated($s);
        $jwk = self::publicJwk($publicKey);
        $details = openssl_pkey_get_details(openssl_pkey_get_public($publicKey));
        if (self::thumbprint($jwk) !== $s['jkt'] ||
            !preg_match('/^[a-f0-9]{64}$/D', $fingerprint) || mb_strlen($name) < 1 || mb_strlen($name) > 100 || preg_match('/[\x00-\x1f]/', $name)) throw new Denied('Invalid device identity');
        $id = self::token();
        $existing = $this->query('SELECT id FROM devices WHERE owner = ? AND public_key = ? AND fingerprint = ? AND revoked = 0 AND owner_epoch = ?', [$s['user_id'], $details['key'], $fingerprint, $s['epoch']])->fetch();
        if ($existing) return ['device_id' => $existing['id']];
        $this->query('INSERT INTO devices (id, owner, name, public_key, fingerprint, owner_epoch) VALUES (?, ?, ?, ?, ?, ?)', [$id, $s['user_id'], $name, $details['key'], $fingerprint, $s['epoch']]);
        $this->audit($s['user_id'], $id, 'device_enrolled'); return ['device_id' => $id];
    }
    public function enrollJwk(array $s, string $name, array $jwk, string $fingerprint): array
    {
        if (($jwk['kty'] ?? '') !== 'RSA' || isset($jwk['d']) || isset($jwk['p']) || isset($jwk['q'])) throw new Denied('Public host key required');
        $key = JWK::parseKey($jwk, 'RS256')->getKeyMaterial();
        $details = openssl_pkey_get_details(openssl_pkey_get_public($key));
        return $this->enrollDevice($s, $name, $details['key'], $fingerprint);
    }
    private function owner(array $s, string $device): array
    {
        return $this->one('SELECT * FROM devices WHERE id = ? AND owner = ? AND revoked = 0', [$device, $s['user_id']]);
    }
    public function grant(array $s, string $device, string $viewerName, array $rights, int $expires): array
    {
        $this->elevated($s); $this->owner($s, $device);
        $viewer = $this->one('SELECT * FROM users WHERE name = ? AND active = 1', [self::name($viewerName)]);
        $rights = array_values(array_unique($rights)); sort($rights);
        if (!$rights || !in_array('view', $rights, true) || array_diff($rights, self::RIGHTS) ||
            $expires <= $this->now || $expires > $this->now + 31536000) throw new Denied('Invalid permission or expiration');
        $id = self::token();
        $existing = $this->query('SELECT id FROM grants WHERE device_id = ? AND viewer = ?', [$device, $viewer['id']])->fetch();
        if ($existing) {
            $id = $existing['id'];
            $this->query('UPDATE grants SET rights = ?, expires = ?, revoked = 0, viewer_epoch = ? WHERE id = ?', [json_encode($rights), $expires, $viewer['epoch'], $id]);
        } else $this->query('INSERT INTO grants (id, device_id, viewer, rights, expires, viewer_epoch) VALUES (?, ?, ?, ?, ?, ?)', [$id, $device, $viewer['id'], json_encode($rights), $expires, $viewer['epoch']]);
        $this->audit($s['user_id'], $id, 'grant_created'); return ['grant_id' => $id];
    }
    public function revokeGrant(array $s, string $id): void
    {
        $this->transaction(function () use ($s, $id) {
            $g = $this->one('SELECT * FROM grants WHERE id = ?', [$id]); $this->owner($s, $g['device_id']);
            $this->query('UPDATE grants SET revoked = 1 WHERE id = ?', [$id]);
            $this->query('DELETE FROM leases WHERE grant_id = ?', [$id]);
            $this->audit($s['user_id'], $id, 'grant_revoked');
        });
    }
    public function grants(array $s, string $device): array
    {
        $this->owner($s, $device);
        $grants = $this->query('SELECT g.id, u.name AS username, g.rights, g.expires, g.revoked FROM grants g JOIN users u ON u.id = g.viewer WHERE g.device_id = ?', [$device])->fetchAll();
        foreach ($grants as &$g) $g['rights'] = json_decode($g['rights'], true); unset($g);
        return $grants;
    }
    public function revokeDevice(array $s, string $id): void
    {
        $this->owner($s, $id);
        $this->query('UPDATE devices SET revoked = 1, epoch = epoch + 1 WHERE id = ?', [$id]); $this->audit($s['user_id'], $id, 'device_revoked');
    }
    public function devices(array $s): array
    {
        $owned = $this->query('SELECT id, name, owner, fingerprint, owner_epoch FROM devices WHERE revoked = 0 AND owner = ?', [$s['user_id']])->fetchAll();
        foreach ($owned as &$d) { $d['grant_id'] = null; $d['rights'] = self::RIGHTS; $d['needs_reauthorization'] = (int)$d['owner_epoch'] !== (int)$s['epoch']; unset($d['owner_epoch']); } unset($d);
        $shared = $this->query('SELECT d.id, d.name, d.owner, d.fingerprint, g.id AS grant_id, g.rights, g.expires FROM devices d JOIN grants g ON g.device_id = d.id JOIN users u ON u.id = g.viewer JOIN users o ON o.id = d.owner WHERE d.revoked = 0 AND o.active = 1 AND d.owner_epoch = o.epoch AND g.viewer = ? AND g.revoked = 0 AND g.expires > ? AND g.viewer_epoch = u.epoch AND d.owner <> ?', [$s['user_id'], $this->now, $s['user_id']])->fetchAll();
        foreach ($shared as &$d) $d['rights'] = json_decode($d['rights'], true); unset($d);
        return array_merge($owned, $shared);
    }
    private function authorization(array $s, string $device, ?string $grantId): array
    {
        $d = $this->one('SELECT d.* FROM devices d JOIN users u ON d.owner = u.id WHERE d.id = ? AND d.revoked = 0 AND u.active = 1 AND d.owner_epoch = u.epoch', [$device]);
        if ($d['owner'] === $s['user_id']) {
            // Owner permissions still require the host's independent local approval/enrollment policy.
            return [$d, self::RIGHTS];
        }
        $g = $this->one('SELECT g.* FROM grants g JOIN users u ON u.id = g.viewer WHERE g.id = ? AND g.device_id = ? AND g.viewer = ? AND g.revoked = 0 AND g.expires > ? AND g.viewer_epoch = u.epoch AND u.active = 1', [$grantId, $device, $s['user_id'], $this->now]);
        return [$d, json_decode($g['rights'], true, 8, JSON_THROW_ON_ERROR)];
    }
    public function ticket(array $s, string $device, ?string $grantId, array $requested, string $hostSession): array
    {
        [$d, $allowed] = $this->authorization($s, $device, $grantId);
        $host = $this->one('SELECT s.* FROM sessions s JOIN users u ON u.id = s.user_id JOIN viewers v ON v.user_id = u.id AND v.jkt = s.jkt WHERE s.id = ? AND s.user_id = ? AND s.revoked = 0 AND s.expires > ? AND u.active = 1 AND s.epoch = u.epoch AND v.revoked = 0 AND v.epoch = u.epoch', [$hostSession, $d['owner'], $this->now]);
        if ($host['jkt'] !== self::thumbprint(self::publicJwk($d['public_key']))) throw new Denied('Host key mismatch');
        $requested = array_values(array_unique($requested));
        if (!$requested || !in_array('view', $requested, true) || array_diff($requested, $allowed)) throw new Denied('Requested permission denied');
        $id = self::token(); $expires = $this->now + 30;
        $this->query('INSERT INTO leases VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?)', [$id, $device, $s['id'], $host['id'], $grantId, json_encode($requested), $expires, $d['epoch']]);
        $this->audit($s['user_id'], $device, 'ticket_issued');
        return ['ticket' => JWT::encode(['iss' => $this->baseUrl, 'aud' => 'hyperlink-host', 'sub' => $device, 'jti' => $id,
            'iat' => $this->now, 'exp' => $expires, 'viewer_jkt' => $s['jkt'], 'host_jkt' => $host['jkt'],
            'host_fingerprint' => $d['fingerprint'], 'device_epoch' => (int)$d['epoch'], 'rights' => $requested], $this->signingKey, 'RS256'), 'expires' => $expires];
    }
    public function redeem(array $host, string $ticket): array
    {
        $p = (array) JWT::decode($ticket, new Key($this->publicKey, 'RS256'));
        if (($p['iss'] ?? '') !== $this->baseUrl || ($p['aud'] ?? '') !== 'hyperlink-host' || (int)($p['exp'] ?? 0) <= $this->now ||
            ($p['host_jkt'] ?? '') !== $host['jkt']) throw new Denied('Invalid session ticket');
        return $this->transaction(function () use ($host, $p) {
            $l = $this->validLease($host, $p['jti'], false);
            if ($this->query('UPDATE leases SET redeemed = 1, expires = ? WHERE id = ? AND redeemed = 0', [$this->now + 30, $l['id']])->rowCount() !== 1) throw new Denied('Ticket already redeemed');
            $this->audit($host['user_id'], $l['device_id'], 'session_started');
            return ['lease_id' => $l['id'], 'expires' => $this->now + 30, 'renew_after' => 5, 'rights' => json_decode($l['rights'], true)];
        });
    }
    private function validLease(array $host, string $id, bool $redeemed): array
    {
        $l = $this->one('SELECT * FROM leases WHERE id = ? AND host_session = ? AND expires > ? AND redeemed = ?', [$id, $host['id'], $this->now, $redeemed ? 1 : 0]);
        $viewer = $this->one('SELECT s.* FROM sessions s JOIN users u ON u.id = s.user_id JOIN viewers v ON v.user_id = u.id AND v.jkt = s.jkt WHERE s.id = ? AND s.revoked = 0 AND s.expires > ? AND u.active = 1 AND s.epoch = u.epoch AND v.revoked = 0 AND v.epoch = u.epoch', [$l['viewer_session'], $this->now]);
        [$d, $rights] = $this->authorization($viewer, $l['device_id'], $l['grant_id']);
        if ($d['owner'] !== $host['user_id'] || (int)$d['epoch'] !== (int)$l['device_epoch'] || array_diff(json_decode($l['rights'], true), $rights)) throw new Denied('Session authorization revoked');
        return $l;
    }
    public function renew(array $host, string $id): array
    {
        $this->validLease($host, $id, true);
        $this->query('UPDATE leases SET expires = ? WHERE id = ?', [$this->now + 30, $id]);
        return ['expires' => $this->now + 30, 'renew_after' => 5];
    }
    public function end(array $s, string $id): void
    {
        $l = $this->one('SELECT * FROM leases WHERE id = ? AND (host_session = ? OR viewer_session = ?)', [$id, $s['id'], $s['id']]);
        $this->query('DELETE FROM leases WHERE id = ?', [$id]); $this->audit($s['user_id'], $l['device_id'], 'session_ended');
    }
    public function sessions(array $s): array
    {
        return $this->query('SELECT id, jkt, expires, revoked FROM sessions WHERE user_id = ?', [$s['user_id']])->fetchAll();
    }
    public function viewers(array $s): array
    {
        return $this->query('SELECT id, label, jkt, epoch, revoked FROM viewers WHERE user_id = ?', [$s['user_id']])->fetchAll();
    }
    public function revokeViewer(array $s, string $id): void
    {
        $v = $this->one('SELECT * FROM viewers WHERE id = ? AND user_id = ?', [$id, $s['user_id']]);
        $this->query('UPDATE viewers SET revoked = 1 WHERE id = ?', [$id]);
        $this->query('UPDATE sessions SET revoked = 1 WHERE user_id = ? AND jkt = ?', [$s['user_id'], $v['jkt']]);
        $this->audit($s['user_id'], $id, 'viewer_revoked');
    }
    public function signingPublicKey(): string { return $this->publicKey; }
    public function signingJwk(): array { return self::publicJwk($this->publicKey); }
    public function reauthorizeDevice(array $s, string $id): void
    {
        $this->elevated($s); $d = $this->owner($s, $id);
        if ($s['jkt'] !== self::thumbprint(self::publicJwk($d['public_key']))) throw new Denied('Existing host key proof required');
        $this->query('UPDATE devices SET owner_epoch = ?, epoch = epoch + 1 WHERE id = ?', [$s['epoch'], $id]);
        $this->audit($s['user_id'], $id, 'host_trust_renewed');
    }
    public function close(): void { unset($this->db); }
    public function cleanup(): void
    {
        foreach (['proofs' => 'expires', 'refresh_history' => 'expires', 'leases' => 'expires', 'invites' => 'expires', 'attempts' => 'until_time', 'sessions' => 'expires'] as $table => $column)
            $this->query('DELETE FROM '.$table.' WHERE '.$column.' <= ?', [$this->now]);
        $this->query('DELETE FROM audit WHERE at_time < ?', [$this->now - 2592000]);
    }
}
