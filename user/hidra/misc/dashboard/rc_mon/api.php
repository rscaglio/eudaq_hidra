<?php
declare(strict_types=1);

header('Content-Type: application/json');
header('Cache-Control: no-store');


$EUDAQHIDRA = getenv("EUDAQHIDRA");
$WATCH_DIR = rtrim($EUDAQHIDRA, "/") . "/run/monitoring/";
$GLOB = "*.jsonl";

function fail_json(string $msg, int $code = 500): never {
    http_response_code($code);
    echo json_encode(["ok" => false, "error" => $msg]);
    exit;
}

function first_valid_json_line(string $file): ?array {
    $fh = fopen($file, "rb");
    if (!$fh) return null;

    while (($line = fgets($fh)) !== false) {
        $line = trim($line);
        if ($line === "") continue;

        $decoded = json_decode($line, true);
        if (json_last_error() === JSON_ERROR_NONE && is_array($decoded)) {
            fclose($fh);
            return $decoded;
        }
    }

    fclose($fh);
    return null;
}

function last_valid_json_line(string $file, int $maxBytes = 524288): ?array {
    $size = filesize($file);
    $fh = fopen($file, "rb");
    if (!$fh) return null;

    $offset = max(0, $size - $maxBytes);
    fseek($fh, $offset);
    $data = stream_get_contents($fh);
    fclose($fh);

    if ($offset > 0) {
        $firstNl = strpos($data, "\n");
        if ($firstNl !== false) {
            $data = substr($data, $firstNl + 1);
        }
    }

    $lines = preg_split("/\r\n|\n|\r/", trim($data));

    for ($i = count($lines) - 1; $i >= 0; $i--) {
        $line = trim($lines[$i]);
        if ($line === "") continue;

        $decoded = json_decode($line, true);
        if (json_last_error() === JSON_ERROR_NONE && is_array($decoded)) {
            return $decoded;
        }
    }

    return null;
}

if (!is_dir($WATCH_DIR) || !is_readable($WATCH_DIR)) {
    fail_json("Directory not readable: $WATCH_DIR");
}

$files = glob(rtrim($WATCH_DIR, "/") . "/" . $GLOB);
$files = array_filter($files, fn($f) => is_file($f) && is_readable($f));

if (!$files) {
    echo json_encode(["ok" => true, "file" => null, "entry" => null]);
    exit;
}

usort($files, fn($a, $b) => filemtime($b) <=> filemtime($a));
$file = $files[0];

$firstValid = first_valid_json_line($file);
$lastValid  = last_valid_json_line($file);

$run_start_unix_ns = $firstValid["time_unix_ns"] ?? null;

echo json_encode([
    "ok" => true,
    "file" => basename($file),
    "file_mtime" => filemtime($file),
    "file_size" => filesize($file),
    "run_start_unix_ns" => $run_start_unix_ns,
    "entry" => $lastValid,
    "server_time" => time()
]);
