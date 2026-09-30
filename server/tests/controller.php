<?php
declare(strict_types=1);
require dirname(__DIR__).'/vendor/autoload.php';
require dirname(__DIR__).'/src/Controller.php';

use Hyperlink\{Controller, Denied};
use Firebase\JWT\JWT;
use OTPHP\TOTP;

$dir = sys_get_temp_dir().'/hyperlink-test-'.bin2hex(random_bytes(8)); mkdir($dir, 0700);
$key = openssl_pkey_new(['private_key_bits' => 2048]); openssl_pkey_export($key, $pem);
file_put_contents($dir.'/secret', random_bytes(32)); file_put_contents($dir.'/key', $pem);
$config = ['secret_file' => $dir.'/secret', 'signing_key_file' => $dir.'/key', 'dsn' => 'sqlite:'.$dir.'/db', 'base_url' => 'https://hyperlink.test'];
$now = time(); $passed = 0; $instances = [];
function app(int $offset = 0): Controller {
    global $config, $now, $instances; JWT::$timestamp = $now + $offset;
    $a = new Controller($config, $now + $offset); $instances[] = $a; return $a;
}
function check(bool $ok, string $label): void {
    global $passed; if (!$ok) throw new RuntimeException('FAILED: '.$label); $passed++; echo 'PASS '.$label."\n";
}
function denied(callable $fn, string $label): void {
    try { $fn(); } catch (Denied|UnexpectedValueException $e) { check(true, $label); return; }
    throw new RuntimeException('FAILED: allowed '.$label);
}
function identity(): array {
    $key = openssl_pkey_new(['private_key_bits' => 2048]); openssl_pkey_export($key, $pem);
    $d = openssl_pkey_get_details($key); $b64 = fn($s) => rtrim(strtr(base64_encode($s), '+/', '-_'), '=');
    $jwk = ['e' => $b64($d['rsa']['e']), 'kty' => 'RSA', 'n' => $b64($d['rsa']['n'])];
    return ['key' => $pem, 'public' => $d['key'], 'jwk' => $jwk, 'jkt' => $b64(hash('sha256', json_encode($jwk, JSON_UNESCAPED_SLASHES), true))];
}
function proof(array $key, string $method, string $path, int $offset = 0, ?string $access = null, array $extra = []): string {
    global $now;
    $p = ['iat' => $now + $offset, 'jti' => bin2hex(random_bytes(16)), 'htm' => $method, 'htu' => 'https://hyperlink.test'.$path];
    if ($access) $p['ath'] = rtrim(strtr(base64_encode(hash('sha256', $access, true)), '+/', '-_'), '=');
    return JWT::encode(array_replace($p, $extra), $key['key'], 'RS256', null, ['typ' => 'dpop+jwt', 'jwk' => $key['jwk']]);
}
function otp(string $secret, int $offset): string { global $now; return TOTP::createFromSecret($secret)->at($now + $offset); }

try {
    $a = app(); $alice = identity(); $bob = identity(); $eve = identity();
    if (isset($argv[1])) {
        $native = json_decode(file_get_contents($argv[1]), true, 8, JSON_THROW_ON_ERROR);
        check($a->proof($native['proof'], 'POST', '/v1/devices', str_repeat('a', 43)) === $native['jkt'], 'Windows RSA proof verified by PHP JWT library');
        denied(fn() => $a->proof($native['proof'], 'POST', '/v1/devices', str_repeat('a', 43)), 'Windows proof replay rejected');
    }
    $invite = $a->bootstrap('alice');
    denied(fn() => $a->bootstrap('other'), 'bootstrap cannot repeat');
    denied(fn() => $a->register($invite, 'mallory', 'a correct long password'), 'invitation bound to intended username');
    $enroll = $a->register($invite, 'alice', 'a correct long password');
    denied(fn() => $a->register($invite, 'alice', 'a correct long password'), 'invitation single use');
    denied(fn() => $a->login('alice', 'a correct long password', otp($enroll['secret'], 0), $alice['jkt'], 'test'), 'MFA required before login');
    $recovery = $a->activate($enroll['enrollment_id'], otp($enroll['secret'], 0));
    check(count($recovery['recovery_codes']) === 10, 'recovery codes issued once');
    denied(fn() => $a->login('alice', 'a correct long password', otp($enroll['secret'], 0), $alice['jkt'], 'test', true), 'authenticator replay rejected');
    $a = app(31);
    denied(fn() => $a->login('alice', 'a correct long password', otp($enroll['secret'], 31), $alice['jkt'], 'test'), 'new viewer needs explicit approval');
    $login = $a->login('alice', 'a correct long password', otp($enroll['secret'], 31), $alice['jkt'], 'test', true, 'Alice PC');
    $s = $a->authenticate($login['access_token'], $alice['jkt']);
    denied(fn() => $a->authenticate($login['access_token'], $bob['jkt']), 'stolen token without enrolled key rejected');
    $p = proof($alice, 'POST', '/v1/devices', 31, $login['access_token']);
    check($a->proof($p, 'POST', '/v1/devices', $login['access_token']) === $alice['jkt'], 'DPoP signature and token binding');
    $forged = proof($alice, 'POST', '/v1/devices', 31, $login['access_token']);
    $signature = strrpos($forged, '.') + 1; $forged[$signature] = $forged[$signature] === 'A' ? 'B' : 'A';
    denied(fn() => $a->proof($forged, 'POST', '/v1/devices', $login['access_token']), 'forged proof signature rejected');
    denied(fn() => $a->proof($p, 'POST', '/v1/devices', $login['access_token']), 'DPoP replay rejected');
    denied(fn() => $a->proof(proof($alice, 'POST', '/v1/devices', 31, $login['access_token']), 'GET', '/v1/devices', $login['access_token']), 'DPoP method mismatch rejected');
    denied(fn() => $a->proof(proof($alice, 'POST', '/v1/devices', 31, $login['access_token']), 'POST', '/v1/grants', $login['access_token']), 'DPoP endpoint mismatch rejected');
    denied(fn() => $a->proof(proof($alice, 'POST', '/v1/devices', 31, 'wrong'), 'POST', '/v1/devices', $login['access_token']), 'DPoP access hash mismatch rejected');
    denied(fn() => $a->proof(proof($alice, 'POST', '/v1/devices', -100), 'POST', '/v1/devices'), 'old DPoP proof rejected');
    $bobInvite = $a->inviteAccount($s, 'bob'); $b = $a->register($bobInvite, 'bob', 'bob correct long password');
    $a->activate($b['enrollment_id'], otp($b['secret'], 31));
    $a = app(62); $bl = $a->login('bob', 'bob correct long password', otp($b['secret'], 62), $bob['jkt'], 'test', true, 'Bob phone');
    $bs = $a->authenticate($bl['access_token'], $bob['jkt']);
    $device = $a->enrollDevice($s, 'Family PC', $alice['public'], str_repeat('a', 64))['device_id'];
    check($a->enrollDevice($s, 'Family PC', $alice['public'], str_repeat('a', 64))['device_id'] === $device, 'host enrollment retry preserves identity');
    denied(fn() => $a->enrollDevice($s, 'Wrong host', $bob['public'], str_repeat('b', 64)), 'host enrollment requires matching private key proof');
    check(count($a->devices($bs)) === 0, 'unshared devices invisible across accounts');
    denied(fn() => $a->grant($bs, $device, 'bob', ['view'], $now + 200), 'nonowner cannot grant own access');
    denied(fn() => $a->inviteAccount($bs, 'eve'), 'nonadmin cannot invite accounts');
    $g = $a->grant($s, $device, 'bob', ['view'], $now + 200)['grant_id'];
    check($a->grant($s, $device, 'bob', ['view'], $now + 200)['grant_id'] === $g, 'share retry cannot create an unrevoked duplicate');
    denied(fn() => $a->grants($bs, $device), 'nonowner cannot enumerate another host shares');
    check(count($a->devices($bs)) === 1, 'named valid grant exposes only shared device');
    denied(fn() => $a->ticket($bs, $device, $g, ['view', 'control'], $s['id']), 'view grant cannot mint control ticket');
    $ticket = $a->ticket($bs, $device, $g, ['view'], $s['id']);
    denied(fn() => $a->redeem($bs, $ticket['ticket']), 'viewer cannot redeem host ticket');
    $lease = $a->redeem($s, $ticket['ticket']);
    check($lease['expires'] === $now + 92 && $lease['renew_after'] === 5, 'bounded renewable thirty second lease');
    denied(fn() => $a->redeem($s, $ticket['ticket']), 'session ticket single use');
    check($a->renew($s, $lease['lease_id'])['expires'] === $now + 92, 'host renews authorized session');
    $expired = app(95);
    denied(fn() => $expired->renew($s, $lease['lease_id']), 'expired lease cannot be renewed');
    denied(fn() => $expired->redeem($s, $ticket['ticket']), 'expired signed ticket denied');
    $a = app(62);
    $a->revokeGrant($s, $g);
    denied(fn() => $a->renew($s, $lease['lease_id']), 'grant revocation denies next heartbeat');
    check(count($a->devices($bs)) === 0, 'revoked share disappears');
    $g2 = $a->grant($s, $device, 'bob', ['view', 'control'], $now + 200)['grant_id'];
    denied(fn() => $a->renew($s, $lease['lease_id']), 'regrant cannot resurrect a revoked session lease');
    $lease2 = $a->redeem($s, $a->ticket($bs, $device, $g2, ['view'], $s['id'])['ticket']);
    $a->revokeSession($bs, $bs['id']);
    denied(fn() => $a->renew($s, $lease2['lease_id']), 'viewer login revocation denies active lease');
    denied(fn() => $a->authenticate($bl['access_token'], $bob['jkt']), 'revoked login rejected');
    $rotated = $a->refresh($login['refresh_token'], $alice['jkt']);
    denied(fn() => $a->authenticate($login['access_token'], $alice['jkt']), 'refresh rotation invalidates old access token');
    check($a->authenticate($rotated['access_token'], $alice['jkt'])['id'] === $s['id'], 'new rotated access token works');
    denied(fn() => $a->refresh($login['refresh_token'], $alice['jkt']), 'refresh reuse detected');
    denied(fn() => $a->authenticate($rotated['access_token'], $alice['jkt']), 'refresh reuse revokes whole login');
    $a = app(93); $new = $a->login('alice', 'a correct long password', otp($enroll['secret'], 93), $alice['jkt'], 'test');
    $ns = $a->authenticate($new['access_token'], $alice['jkt']);
    $a = app(124); $bl2 = $a->login('bob', 'bob correct long password', otp($b['secret'], 124), $bob['jkt'], 'test');
    $bs2 = $a->authenticate($bl2['access_token'], $bob['jkt']);
    denied(fn() => $a->recover('bob', 'bob correct long password', TOTP::createFromSecret($b['secret'])->getSecret()), 'authenticator secret is not a recovery code');
    // Generate a fresh member to exercise valid recovery independently.
    $inviteC = $a->inviteAccount($ns, 'charlie');
    $c = $a->register($inviteC, 'charlie', 'charlie correct long password');
    $cr = $a->activate($c['enrollment_id'], otp($c['secret'], 124));
    $a = app(155); $cl = $a->login('charlie', 'charlie correct long password', otp($c['secret'], 155), $eve['jkt'], 'test', true, 'Charlie phone');
    $cs = $a->authenticate($cl['access_token'], $eve['jkt']);
    $charlieDevice = $a->enrollDevice($cs, 'Charlie PC', $eve['public'], str_repeat('c', 64))['device_id'];
    denied(fn() => $a->grant($ns, $charlieDevice, 'alice', ['view', 'control'], $now + 1000), 'account administrator cannot grant access to another owner host');
    $cg = $a->grant($ns, $device, 'charlie', ['view', 'unattended'], $now + 1000)['grant_id'];
    $recovered = $a->recover('charlie', 'charlie correct long password', $cr['recovery_codes'][0]);
    denied(fn() => $a->authenticate($cl['access_token'], $eve['jkt']), 'recovery revokes all old logins');
    denied(fn() => $a->ticket($cs, $device, $cg, ['view'], $ns['id']), 'recovery invalidates prior sharing trust');
    denied(fn() => $a->recover('charlie', 'charlie correct long password', $cr['recovery_codes'][0]), 'recovery code single use');
    $a->activate($recovered['enrollment_id'], otp($recovered['secret'], 155));
    denied(fn() => $a->recover('charlie', 'charlie correct long password', $cr['recovery_codes'][0]), 'used recovery code rejected after reactivation');
    $a = app(186);
    $newCharlie = $a->login('charlie', 'charlie correct long password', otp($recovered['secret'], 186), $eve['jkt'], 'test', true, 'Charlie phone');
    $newCs = $a->authenticate($newCharlie['access_token'], $eve['jkt']);
    denied(fn() => $a->ticket($newCs, $charlieDevice, null, ['view', 'unattended'], $newCs['id']), 'account recovery does not silently restore owned unattended hosts');
    $a->reauthorizeDevice($newCs, $charlieDevice);
    check(isset($a->ticket($newCs, $charlieDevice, null, ['view'], $newCs['id'])['ticket']), 'explicit MFA and existing host key renew recovered host trust');
    $charlieViewers = $a->viewers($newCs);
    $a->revokeViewer($newCs, $charlieViewers[0]['id']);
    denied(fn() => $a->authenticate($newCharlie['access_token'], $eve['jkt']), 'viewer key revocation denies all key-bound logins');
    denied(fn() => $a->refresh($newCharlie['refresh_token'], $eve['jkt']), 'revoked viewer key cannot refresh');
    $later = app(217);
    denied(fn() => $later->login('charlie', 'charlie correct long password', otp($recovered['secret'], 217), $eve['jkt'], 'test', true), 'revoked viewer key cannot reenroll through ordinary login');
    $a = app(186);
    for ($n = 0; $n < 10; $n++) {
        try { $a->login('alice', 'incorrect password', '000000', $alice['jkt'], 'different-ip-'.$n); throw new RuntimeException('Bad password allowed'); }
        catch (Denied $e) {}
    }
    try { $a->login('alice', 'a correct long password', otp($enroll['secret'], 186), $alice['jkt'], 'another-ip'); throw new RuntimeException('Distributed login attempts were not limited'); }
    catch (Denied $e) { check($e->getMessage() === 'Try again later', 'account rate limit survives attacker IP rotation'); }
    $di = $a->inviteAccount($ns, 'delayed-code');
    $de = $a->register($di, 'delayed-code', 'delayed test correct password');
    $a->activate($de['enrollment_id'], otp($de['secret'], 186));
    $delayOffset = (intdiv($now + 186, 30) + 2) * 30 - $now;
    $delayApp = app($delayOffset);
    $delayedCode = otp($de['secret'], $delayOffset - 1);
    check(isset($delayApp->login('delayed-code', 'delayed test correct password', $delayedCode, $bob['jkt'], 'test', true)['access_token']), 'fresh previous-step authenticator code survives delivery boundary');
    denied(fn() => $delayApp->login('delayed-code', 'delayed test correct password', $delayedCode, $bob['jkt'], 'test'), 'previous-step authenticator code still single use');
    $a->revokeDevice($ns, $device);
    check(count($a->devices($ns)) === 0, 'host identity revocation hides device');
    $a->cleanup();
    echo $passed." checks passed\n";
} finally {
    foreach ($instances as $instance) $instance->close();
    $instances = []; $a = null; JWT::$timestamp = null;
    // All PDO statements and handles are gone before Windows cleanup.
    foreach (glob($dir.'/*') as $file) unlink($file); rmdir($dir);
}
