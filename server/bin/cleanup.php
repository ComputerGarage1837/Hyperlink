<?php
declare(strict_types=1);
if (PHP_SAPI !== 'cli' || count($argv) !== 2) exit(1);
require dirname(__DIR__).'/vendor/autoload.php';
require dirname(__DIR__).'/src/Controller.php';
$app = new \Hyperlink\Controller(require $argv[1]); $app->cleanup(); $app->close();
