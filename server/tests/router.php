<?php
declare(strict_types=1);
// A loopback-only CLI test harness. Never deploy tests/ as a document root.
if (PHP_SAPI !== 'cli-server' || getenv('HYPERLINK_TEST_ROUTER') !== '1' ||
    ($_SERVER['REMOTE_ADDR'] ?? '') !== '127.0.0.1') { http_response_code(404); exit; }
if (($_SERVER['HTTP_X_TEST_PLAIN_HTTP'] ?? '') !== '1') $_SERVER['HTTPS'] = 'on';
require dirname(__DIR__).'/public/index.php';
