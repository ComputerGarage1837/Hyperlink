<?php
declare(strict_types=1);
// Run with the site offline BEFORE serving a restored database.
if (PHP_SAPI !== 'cli' || count($argv) !== 2) exit(1);
$config = require $argv[1];
$db = new PDO($config['dsn'], $config['db_user'] ?? null, $config['db_password'] ?? null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->beginTransaction();
$db->exec('UPDATE users SET epoch = epoch + 1');
$db->exec('UPDATE sessions SET revoked = 1');
$db->exec('DELETE FROM leases');
$db->commit();
echo "Restored logins revoked. All host and viewer trust requires deliberate renewal.\n";
