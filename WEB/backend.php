
<?php
header("Access-Control-Allow-Origin: *");
$uploadDirectory = '/home/u916974978/domains/exercices.nprata.de/public_html/STM32_FW/'; 
$targetFile = $uploadDirectory . 'application.bin';

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_FILES['fw_file'])) {
    $file = $_FILES['fw_file'];
    $fileName = basename($file['name']);
    
    // Size check & force file name to enforce your standard
    if ($file['size'] >= 100 && $file['size'] <= 65536 && $fileName == "application.bin") {        
        $destination = $uploadDirectory . $fileName;        
        
        if (move_uploaded_file($file['tmp_name'], $destination)) {
            
            // Clear cache and check specifically for this file
            clearstatcache();
            $searchPattern = $uploadDirectory . $fileName;
            $result = glob($searchPattern);
            
            if (!empty($result) && file_exists($result[0])) {
                
                // 1. Read the raw binary data (Equiv to Python: open(..., 'rb').read())
                $rawData = file_get_contents($destination);
                $totalSize = strlen($rawData);
                
                // 2. Handle 4-byte padding alignment constraints for the STM32 Hardware CRC unit
                $remainder = $totalSize % 4;
                if ($remainder != 0) {
                    $paddingNeeded = 4 - $remainder;
                    
                    // Append 0xFF padding bytes (Equiv to Python: + b'\xFF' * padding_needed)
                    $rawData .= str_repeat("\xFF", $paddingNeeded);
                    $totalSize = strlen($rawData);
                    
                    // Overwrite the file on disk with the padded version
                    file_put_contents($destination, $rawData);
                }
                
                // 3. Compute the CRC32 - PHP's crc32() matches standard zlib/ethernet polynomial calculation
                // sprintf converts the checksum into a perfect, positive unsigned integer string.            
                $totalCrc = (float)sprintf('%u', crc32($rawData));
                
                // 4. Construct your manifest data
                $manifest = [
                    "version_major" => (int)$_POST['major'], 
                    "version_minor" => (int)$_POST['minor'],
                    "total_size"    => $totalSize, // Send the newly padded size
                    "total_crc"     => $totalCrc   // Optional, but nice to have in the JSON
                ];
                
                // Write the manifest next to the binary
                file_put_contents($uploadDirectory . "firmware.json", json_encode($manifest));
                
                http_response_code(200);
                echo "VERIFIED_ON_SERVER";

            } else {
                http_response_code(500);
                echo "ERROR: File moved but could not be found by glob.";
            }
            
        } else {
            http_response_code(500);
            echo "ERROR: Failed to move uploaded file.";
        }
    } else {
        http_response_code(400);
        echo "ERROR: Invalid file size or bad name matching constraint.";
    }
    exit();
}

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['action'])) {
    
    if ($_POST['action'] === 'delete_fw') {
        
        // 1. Check if the file even exists before trying to delete it
        if (!file_exists($targetFile)) {
            echo "ERROR: File does not exist on the server.";
            exit();
        }
        
        // 2. Attempt to delete (unlink) the file
        if (unlink($targetFile)) {
            
            // --- THE ACTIVE VERIFICATION ---
            // Clear the stat cache to ensure PHP reads the true disk state
            clearstatcache(); 
            
            if (!file_exists($targetFile)) {
                // Double checked: The file is physically gone
                echo "DELETION_VERIFIED";
            } else {
                echo "ERROR: Unlink returned true, but the file still exists on disk.";
            }
            
        } else {
            echo "ERROR: Server failed to delete the file (Check folder permissions).";
        }
        exit();
    }
}


?>



<!-- 

[ PHP Server ] 
   |  Hosts a standard, padded binary file.
   v
[ ESP32-S3 ] <-- The "Translator"
   |  Downloads standard bytes from HTTP buffer.
   |  Slices them into 512-byte blocks in RAM.
   |  Wraps them with: SOF + Packet ID + CRC.
   v
[ STM32F469 Bootloader ]
      Receives the exact frame layout it expects.


1. The PHP Server
Your PHP script continues to do what we just set up: it takes the file, pads it to a 4-byte boundary for the STM32 hardware CRC, 
saves it as a standard .bin file, and updates the firmware.json. It stays a dumb, fast web server.

2. The ESP32-S3 (The Translator)
Since the ESP32 is a powerful dual-core processor, it easily takes over the job your Python script used to do:
    It requests the standard .bin file from PHP.
    Instead of downloading all of it, it reads exactly 512 bytes from the network stream into a local RAM array.
    The ESP32 increments a local variable packet_id++.
    The ESP32 calculates the 32-bit CRC of that 512-byte chunk.
    The ESP32 packs the header (>IHHI) using standard C structure bit-shifting or string copying.
    It blasts the full constructed packet over UART to the STM32, waits for your ACK, and then reads the next 512 bytes from the web stream.

Why this Architecture is Superior
By shifting the packet framing to the ESP32 instead of PHP:
    No Server Overhead: Your website can serve hundreds of devices simultaneously because it's just hosting static files. 
    It doesn't have to run active, loop-heavy PHP threads tracking download states for individual chips.
    Network Independence: If you ever decide to update your STM32 via an SD card plugged directly into the ESP32 (instead of Wi-Fi), 
    your exact same ESP32 chunking code works perfectly. The data source changes, but your protocol engine remains identical.
The ESP32 is the perfect bridge to translate the raw web world into your strict, structured embedded protocol! 

-->