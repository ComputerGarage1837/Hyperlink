<?php
declare(strict_types=1);
if (PHP_SAPI !== 'cli') { http_response_code(404); exit; }
require dirname(__DIR__).'/vendor/autoload.php';
require dirname(__DIR__).'/src/Controller.php';
if (count($argv) !== 4 || !str_starts_with($argv[2], 'https://')) {
    fwrite(STDERR, "Usage: php bin/setup.php PRIVATE_DIRECTORY HTTPS_BASE_URL ADMIN_USERNAME\n"); exit(1);
}
$dir = $argv[1];
if (file_exists($dir)) throw new RuntimeException('Use a new private directory outside the document root');
if (!mkdir($dir, 0700, true)) throw new RuntimeException('Cannot create private directory');
file_put_contents($dir.'/secret.key', random_bytes(32)); chmod($dir.'/secret.key', 0600);
$key = openssl_pkey_new(['private_key_type' => OPENSSL_KEYTYPE_RSA, 'private_key_bits' => 3072]);
if (!$key || !openssl_pkey_export($key, $pem)) throw new RuntimeException('Cannot generate signing key');
file_put_contents($dir.'/signing.key', $pem); chmod($dir.'/signing.key', 0600);
$config = ['base_url' => rtrim($argv[2], '/'), 'dsn' => 'sqlite:'.$dir.'/controller.sqlite',
    'secret_file' => $dir.'/secret.key', 'signing_key_file' => $dir.'/signing.key'];
file_put_contents($dir.'/config.php', "<?php\nreturn ".var_export($config, true).";\n"); chmod($dir.'/config.php', 0600);
$app = new \Hyperlink\Controller($config);
echo "Initial single-use invitation (expires in 24 hours):\n".$app->bootstrap($argv[3])."\n";
$app->close(); chmod($dir.'/controller.sqlite', 0600);
