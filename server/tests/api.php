<?php
declare(strict_types=1);
require dirname(__DIR__).'/vendor/autoload.php';
require dirname(__DIR__).'/src/Controller.php';
use Hyperlink\Controller;
use Firebase\JWT\JWT;
use OTPHP\TOTP;

$dir = sys_get_temp_dir().'/hyperlink-api-'.bin2hex(random_bytes(8)); mkdir($dir, 0700);
$key = openssl_pkey_new(['private_key_bits' => 2048]); openssl_pkey_export($key, $pem);
$details = openssl_pkey_get_details($key);
$b64 = fn($s) => rtrim(strtr(base64_encode($s), '+/', '-_'), '=');
$jwk = ['e' => $b64($details['rsa']['e']), 'kty' => 'RSA', 'n' => $b64($details['rsa']['n'])];
$jkt = $b64(hash('sha256', json_encode($jwk, JSON_UNESCAPED_SLASHES), true));
file_put_contents($dir.'/secret', random_bytes(32)); file_put_contents($dir.'/key', $pem);
$config = ['secret_file' => $dir.'/secret', 'signing_key_file' => $dir.'/key', 'dsn' => 'sqlite:'.$dir.'/db', 'base_url' => 'https://hyperlink.test'];
file_put_contents($dir.'/config.php', '<?php return '.var_export($config, true).';');
$fixtureTime = time() - 31;
$app = new Controller($config, $fixtureTime);
$invite = $app->bootstrap('api-owner');
$enrollment = $app->register($invite, 'api-owner', 'a very long test password');
$app->activate($enrollment['enrollment_id'], TOTP::createFromSecret($enrollment['secret'])->at($fixtureTime)); $app->close();
$socket = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
if (!$socket) throw new RuntimeException($error);
$address = stream_socket_get_name($socket, false); fclose($socket);
$environment = getenv(); $environment['HYPERLINK_CONFIG'] = $dir.'/config.php'; $environment['HYPERLINK_TEST_ROUTER'] = '1';
$process = proc_open([PHP_BINARY, '-S', $address, __DIR__.'/router.php'],
    [0 => ['pipe', 'r'], 1 => ['file', $dir.'/stdout', 'a'], 2 => ['file', $dir.'/stderr', 'a']], $pipes, __DIR__, $environment, ['bypass_shell' => true]);
if (!is_resource($process)) throw new RuntimeException('Cannot start API fixture');
fclose($pipes[0]); $passed = 0;
function request(string $method, string $path, ?array $body = null, ?string $token = null, ?string $proof = null, bool $plain = false): array {
    global $address, $pem, $jwk, $b64;
    if ($proof === null) {
        $claims = ['iat' => time(), 'jti' => bin2hex(random_bytes(16)), 'htm' => $method, 'htu' => 'https://hyperlink.test'.$path];
        if ($token) $claims['ath'] = $b64(hash('sha256', $token, true));
        $proof = JWT::encode($claims, $pem, 'RS256', null, ['typ' => 'dpop+jwt', 'jwk' => $jwk]);
    }
    $headers = ['Content-Type: application/json', 'DPoP: '.$proof];
    if ($token) $headers[] = 'Authorization: DPoP '.$token;
    if ($plain) $headers[] = 'X-Test-Plain-HTTP: 1';
    $curl = curl_init('http://'.$address.$path);
    curl_setopt_array($curl, [CURLOPT_CUSTOMREQUEST => $method, CURLOPT_HTTPHEADER => $headers,
        CURLOPT_RETURNTRANSFER => true, CURLOPT_TIMEOUT => 5]);
    if ($body !== null) curl_setopt($curl, CURLOPT_POSTFIELDS, json_encode($body, JSON_THROW_ON_ERROR));
    $raw = curl_exec($curl); $status = curl_getinfo($curl, CURLINFO_RESPONSE_CODE); $error = curl_error($curl); curl_close($curl);
    if ($raw === false) throw new RuntimeException($error);
    return [$status, json_decode($raw, true, 12, JSON_THROW_ON_ERROR)];
}
function check(bool $ok, string $label): void {
    global $passed; if (!$ok) throw new RuntimeException('FAILED '.$label); $passed++; echo 'PASS '.$label."\n";
}
try {
    $ready = false;
    for ($i = 0; $i < 30; $i++) { usleep(100000); try { $health = request('GET', '/v1/health'); $ready = true; break; } catch (RuntimeException $e) {} }
    if (!$ready) throw new RuntimeException('API fixture failed to start');
    check($health[0] === 200 && $health[1]['service'] === 'Hyperlink', 'HTTP health endpoint');
    check(request('GET', '/v1/health', null, null, null, true)[0] === 403, 'production endpoint rejects plain HTTP');
    check(request('GET', '/v1/devices')[0] === 403, 'HTTP device list requires account authentication');
    $login = request('POST', '/v1/login', ['username' => 'api-owner', 'password' => 'a very long test password', 'code' => TOTP::createFromSecret($enrollment['secret'])->now(), 'trust_new_viewer' => true, 'viewer_label' => 'API viewer']);
    check($login[0] === 200 && $login[1]['token_type'] === 'DPoP', 'HTTP password and MFA login');
    $access = $login[1]['access_token'];
    $devices = request('GET', '/v1/devices', null, $access);
    check($devices[0] === 200 && count($devices[1]['devices']) === 0, 'HTTP authenticated object list');
    $enroll = request('POST', '/v1/devices', ['name' => 'API host', 'public_jwk' => $jwk, 'fingerprint' => str_repeat('a', 64)], $access);
    check($enroll[0] === 200 && isset($enroll[1]['device_id']), 'HTTP host enrollment binds JWK identity (status '.$enroll[0].')');
    $listed = request('GET', '/v1/devices', null, $access);
    check($listed[0] === 200 && count($listed[1]['devices']) === 1, 'HTTP enrolled computer visible to owner');
    check(request('GET', '/v1/devices', null, str_repeat('a', 43))[0] === 403, 'HTTP forged account token denied');
    $publicKey = request('GET', '/v1/signing-key');
    check($publicKey[0] === 200 && $publicKey[1]['jwk']['kty'] === 'RSA', 'HTTP ticket verification key available over validated TLS');
    $refresh = request('POST', '/v1/refresh', ['refresh_token' => $login[1]['refresh_token']]);
    check($refresh[0] === 200, 'HTTP key-bound refresh rotation');
    check(request('GET', '/v1/devices', null, $access)[0] === 403, 'HTTP old access token invalidated');
    check(request('POST', '/v1/refresh', ['refresh_token' => $login[1]['refresh_token']])[0] === 403, 'HTTP reused refresh rejected');
    check(request('GET', '/v1/devices', null, $refresh[1]['access_token'])[0] === 403, 'HTTP refresh reuse revokes login');
    echo $passed." HTTP checks passed\n";
} finally {
    proc_terminate($process); proc_close($process);
    foreach (glob($dir.'/*') as $file) unlink($file); rmdir($dir);
}
