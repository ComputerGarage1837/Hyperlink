<?php
declare(strict_types=1);

// Set this Apache environment variable to a config file OUTSIDE the document root.
$configPath = getenv('HYPERLINK_CONFIG');
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
header('X-Content-Type-Options: nosniff');
header('Strict-Transport-Security: max-age=31536000');
header("Content-Security-Policy: default-src 'none'; frame-ancestors 'none'");
try {
    if (!$configPath || !is_file($configPath)) throw new RuntimeException('Service not configured');
    $config = require $configPath;
    require dirname(__DIR__).'/vendor/autoload.php';
    require dirname(__DIR__).'/src/Controller.php';
    $app = new \Hyperlink\Controller($config);
    // cPanel terminates HTTPS in Apache. No forwarded-header trust is enabled.
    if (($_SERVER['HTTPS'] ?? '') !== 'on' && ($_SERVER['HTTPS'] ?? '') !== '1') throw new \Hyperlink\Denied('HTTPS required');
    $path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
    $method = $_SERVER['REQUEST_METHOD'];
    if (str_contains($_SERVER['REQUEST_URI'], '?') || !is_string($path)) throw new \Hyperlink\Denied('Invalid endpoint');
    if ((int)($_SERVER['CONTENT_LENGTH'] ?? 0) > 16384) { http_response_code(413); echo '{"error":"Request too large"}'; exit; }
    $raw = file_get_contents('php://input', false, null, 0, 16385);
    if (strlen($raw) > 16384) { http_response_code(413); echo '{"error":"Request too large"}'; exit; }
    $body = $raw === '' ? [] : json_decode($raw, true, 8, JSON_THROW_ON_ERROR);
    if (!is_array($body) || ($raw !== '' && !str_starts_with(ltrim($raw), '{'))) throw new \Hyperlink\Denied('JSON object required');
    if ($method === 'GET' && $path === '/v1/health') $result = ['service' => 'Hyperlink', 'api' => 1];
    elseif ($method === 'GET' && $path === '/v1/signing-key') $result = ['algorithm' => 'RS256', 'public_key' => $app->signingPublicKey(), 'jwk' => $app->signingJwk()];
    else {
        if ($method !== 'POST' && $method !== 'GET') throw new \Hyperlink\Denied('Unsupported method');
        $auth = $_SERVER['HTTP_AUTHORIZATION'] ?? '';
        $public = in_array($path, ['/v1/register', '/v1/activate', '/v1/login', '/v1/refresh', '/v1/recover'], true);
        $token = null;
        if (!$public) {
            if (!preg_match('/^DPoP ([A-Za-z0-9_-]{43})$/D', $auth, $match)) throw new \Hyperlink\Denied('Authentication required');
            $token = $match[1];
        }
        $jkt = $app->proof($_SERVER['HTTP_DPOP'] ?? '', $method, $path, $token);
        $session = $public ? null : $app->authenticate($token, $jkt);
        $str = static function (string $key) use ($body): string {
            if (!isset($body[$key]) || !is_string($body[$key])) throw new \Hyperlink\Denied('Missing field: '.$key);
            return $body[$key];
        };
        $rights = static function () use ($body): array {
            if (!isset($body['rights']) || !is_array($body['rights']) || !array_is_list($body['rights']) || count($body['rights']) > 11) throw new \Hyperlink\Denied('Invalid rights');
            foreach ($body['rights'] as $v) if (!is_string($v)) throw new \Hyperlink\Denied('Invalid rights');
            return $body['rights'];
        };
        $result = match ($method.' '.$path) {
            'POST /v1/register' => $app->register($str('invite'), $str('username'), $str('password')),
            'POST /v1/activate' => $app->activate($str('enrollment_id'), $str('code')),
            'POST /v1/login' => $app->login($str('username'), $str('password'), $str('code'), $jkt, $_SERVER['REMOTE_ADDR'], ($body['trust_new_viewer'] ?? false) === true, $str('viewer_label')),
            'POST /v1/refresh' => $app->refresh($str('refresh_token'), $jkt),
            'POST /v1/recover' => $app->recover($str('username'), $str('password'), $str('recovery_code')),
            'POST /v1/elevate' => $app->elevate($session, $str('code')),
            'POST /v1/invites' => ['invite' => $app->inviteAccount($session, $str('username'))],
            'GET /v1/devices' => ['devices' => $app->devices($session)],
            'POST /v1/devices' => $app->enrollJwk($session, $str('name'), $body['public_jwk'] ?? [], $str('fingerprint')),
            'POST /v1/devices/revoke' => $app->revokeDevice($session, $str('device_id')),
            'POST /v1/devices/reauthorize' => $app->reauthorizeDevice($session, $str('device_id')),
            'POST /v1/grants' => $app->grant($session, $str('device_id'), $str('username'), $rights(), is_int($body['expires'] ?? null) ? $body['expires'] : 0),
            'POST /v1/grants/revoke' => $app->revokeGrant($session, $str('grant_id')),
            'POST /v1/grants/list' => ['grants' => $app->grants($session, $str('device_id'))],
            'GET /v1/logins' => ['logins' => $app->sessions($session)],
            'GET /v1/viewers' => ['viewers' => $app->viewers($session)],
            'POST /v1/viewers/revoke' => $app->revokeViewer($session, $str('viewer_id')),
            'POST /v1/logins/revoke' => $app->revokeSession($session, $str('session_id')),
            'POST /v1/tickets' => $app->ticket($session, $str('device_id'), isset($body['grant_id']) ? $str('grant_id') : null, $rights(), $str('host_session')),
            'POST /v1/leases/redeem' => $app->redeem($session, $str('ticket')),
            'POST /v1/leases/renew' => $app->renew($session, $str('lease_id')),
            'POST /v1/leases/end' => $app->end($session, $str('lease_id')),
            default => throw new \Hyperlink\Denied('Unknown endpoint'),
        };
    }
    echo json_encode($result ?? ['ok' => true], JSON_THROW_ON_ERROR | JSON_UNESCAPED_SLASHES);
} catch (\Hyperlink\Denied|\UnexpectedValueException|\JsonException $e) {
    http_response_code(403); echo '{"error":"Request denied"}';
} catch (\Throwable $e) {
    // No credentials, proofs, passwords or stack traces are returned or logged.
    http_response_code(503); echo '{"error":"Service unavailable"}';
} finally { if (isset($app)) $app->close(); }
