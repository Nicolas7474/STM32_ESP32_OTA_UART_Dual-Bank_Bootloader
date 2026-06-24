
<!DOCTYPE html>
<html lang="de">

<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
   <!-- <link rel="shortcut icon" href="/img/favicon.ico">  -->
  <title>Test IoT</title>  
  <!-- MQTT.js library for MQTT communication in JavaScript    -->
  <script src="https://unpkg.com/mqtt/dist/mqtt.min.js"></script>
  <!-- <link href="style_rma.css" rel="stylesheet">
  <link rel="stylesheet" href="flags.css"> 
  <script src="common.js" defer></script>
  <script src="script_daheim.js" defer></script> -->
</head>

<body> 

<div> 

  <?php 
  //header("Refresh:180"); // Refresh the page every 20 seconds to get the latest data from the database

  ini_set('display_errors', 1);
  ini_set('display_startup_errors', 1);
  error_reporting(E_ALL); 

  $mysqli = new mysqli("localhost","u916974978_nini","Lobisomem_74","u916974978_iot");

  if ($mysqli -> connect_errno) {
    echo "<span class= 'error_title '> Failed to connect to MySQL 😮  </span> ";
    exit(); 
  } else {
    echo "<span class= 'success_title '> Connected to MySQL successfully! 😎 </span> ";
  }

  // 1. Read the raw input stream (this is where JSON lives)
  $json = file_get_contents('php://input');

  // 2. Decode it into a PHP associative array
  $data = json_decode($json, true);

  // 3. Check if 'value' exists in the decoded JSON
  if (isset($data['value1']) && isset($data['value2'])) {
      
      $v1 = $data['value1'];
      $v2 = $data['value2'];
      $time = $data['time'];
    
      $s = "SELECT sensor1 FROM table_Rx WHERE ID = 1"; 
      $l = ($mysqli->execute_query($s)->fetch_column()) + $v1;

      $stmt = $mysqli->prepare("UPDATE table_Rx SET sensor1 = ?, sensor2 = ?, `time` = ? WHERE ID = 1");
      $stmt->bind_param("ids", $l, $v2, $time); // "i" = int, "d" = double (float)

      if ($stmt->execute()) {
          //echo "Data updated successfully! Sensor1: $l, Sensor2: $v2";           
      } else {
          echo "Error: " . $mysqli->error;
      }
      
      $stmt->close();
  } 

  if (isset($data['sensor_temp'])) {
      $temp = $data['sensor_temp'];    
      
      $stmt = $mysqli->prepare("UPDATE table_Rx SET internal_temp = ? WHERE ID = 1");
      $stmt->bind_param("d", $temp); // "d" = double (float)

      if ($stmt->execute()) {
          //echo "Temperature updated successfully! Sensor1: $temp";           
      } else {
          echo "Error: " . $mysqli->error;
      }
      
      $stmt->close();
  }

  $se = "SELECT sensor1, sensor2, `time`, internal_temp FROM table_Rx WHERE ID = 1"; 
  $le = ($mysqli->execute_query($se)->fetch_assoc());
  echo "<br><br>  Valeur sensor1: ".$le['sensor1']."<br> 
                  Valeur sensor2: ".$le['sensor2']."<br> 
                  Internal Temperature: ".$le['internal_temp'];
  ?>

</div> 
<div id="display" style="margin-top: 20px; font-size: 24px; color: blue;"></div>



<div style="margin-top: 20px;" >
  <input type="text" id="input_cmd" placeholder="Type a command" value="">
  <button id="btn_send">Send</button>
</div>

<div id="status" style="margin-top: 50px; font-size: 24px;"></div>



<div style="margin:50px 100px">
  <hr/>
  <div style="color:darkblue;margin-top:20px"> STM32 update: upload your application.bin file </div>
  <span>   
    <input type="file" id ="fw_box" name = "fw_file" style="margin:20px 10px" accept=".bin" />
    <input type="submit"  id="store_file" value="Click to upload the file to the server" style="margin-right:10px">
    <input type="submit"  id="delete_file" value="Delete the file on the server">
  </span>
<div>
    <div>
        <span >Version:</span>  <span style="margin:10px 20px">Major  &nbsp; Minor</span>
    </div>
     <div style="margin-left:75px; margin-top:10px">
        <input type="number" name="version_major" min="0" required size = 1 value = 2> 
        .   
        <input type="number" name="version_minor" min="0" required size = 1 value = 4>
    </div>
</div>

</div>


<script>

  // ------------update file bootlader/app stm32-ESP32 ---------------

  document.getElementById('store_file').addEventListener('click', function() {
    const fileInput = document.getElementById('fw_box');
    
    // 1. Check if a file is actually selected
    if (fileInput.files.length === 0) {
        alert('Please select a file first.');
        return;
    }

    const file = fileInput.files[0];

    // 2. Validate file size - from 100 bytes to 64KB
    if (file.size < 100) {
        alert(`File is too small! Must be at least ${minSize} bytes.`);
        return;
    }    
    if (file.size > 64 * 1024) {
        alert(`File is too large! Must be under 64 KB (Current size: ${(file.size / 1024).toFixed(2)} KB).`);
        return;
    }

    // 3. Prepare the data for upload
    const major = document.getElementsByName('version_major')[0].value;
    const minor = document.getElementsByName('version_minor')[0].value;
    const formData = new FormData();
    formData.append('fw_file', file);
    formData.append('major', major);
    formData.append('minor', minor);

    // 4. Send the file to your server via Fetch API
    // Replace 'upload.php' with your actual PHP backend script name (e.g., the current file name)
    fetch('backend.php', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        if (!response.ok) {
            throw new Error('Network response was not stable');
        }
        return response.text(); // Parse the custom server message
    })
    .then(serverText => { 
        // Strict check: Only trigger success if the server's glob check passed
        if (serverText.trim() === "VERIFIED_ON_SERVER") {          
            alert('🎉 Success! The file safely landed and was verified on the server.');
        } else {
            alert('⚠️ Upload anomaly: ' + serverText);
        }
    })
    .catch(error => {
        console.error('Upload Error:', error);
        alert('Failed to communicate with the server.');
    });
});


document.getElementById('delete_file').addEventListener('click', function() {
    // Confirm with the user before deleting
    if (!confirm('Are you sure you want to delete application.bin?')) {
        return;
    }

    // Prepare the payload specifying the action
    const formData = new FormData();
    formData.append('action', 'delete_fw');

    // Send the request to your PHP backend
    fetch('backend.php', {
        method: 'POST',
        body: formData
    })
    .then(response => {
        if (!response.ok) {
            throw new Error('Network response was not stable');
        }
        return response.text();
    })
    .then(serverText => {
        // Check for the explicit deletion confirmation string from the server
        if (serverText.trim() === "DELETION_VERIFIED") {
            alert('🗑️ Success! application.bin has been permanently deleted from the server.');
        } else {
            alert('⚠️ Deletion issue: ' + serverText);
        }
    })
    .catch(error => {
        console.error('Error:', error);
        alert('Failed to communicate with the server.');
    });
});


//    MQTT
//     On a VPS, you can use the native MQTT protocol (mqtt:// or mqtts://)
//     Efficiency: Native MQTT over TCP is even lighter than WebSockets because it removes the HTTP upgrade overhead.
//     Port: You would use the standard MQTT port 1883 (or 8883 for TLS) instead of the WebSocket port 8884.
// Here we use WebSockets; data is lost if the tab is closed ! HiveMQ Cloud WebSocket connection settings:
const host = 'wss://810119234d2e4d7ab1dd24f0d6e2cb0d.s1.eu.hivemq.cloud:8884/mqtt';

const options = {
  keepalive: 60,
  clientId: 'browser_client_' + Math.random().toString(16).substr(2, 8),
  protocolId: 'MQTT',
  protocolVersion: 4,
  clean: true,
  reconnectPeriod: 1000,
  connectTimeout: 30 * 1000,
  username: 'nico7474',
  password: 'Suisse74'
};

console.log('Connecting...');
const client = mqtt.connect(host, options);

// client.on('connect', () => {        // client.on method is an event listener.
//     console.log('Connected to HiveMQ!');
//     // Subscribe to your ESP32 topic
//     client.subscribe('sensor/esp32');
//     document.getElementById('status').innerText = "ONLINE";
//     document.getElementById('status').style.color = "green";
// });

client.on('message', (topic, message) => {
  // message is a buffer, convert to string
  console.log(message.toString());
  const data = JSON.parse(message.toString());
  console.log(`Received: ${message} from ${topic}`); 
  console.log(data);
  document.getElementById('display').innerText = "";
  for(key in data) {
    document.getElementById('display').innerText += `${key}: ${data[key]} \n\r`;
  }

  //document.getElementById('display').innerText = message.toString();
});

client.on('error', (err) => {
  console.error('Connection error: ', err);
  client.end();
});

client.on('close', () => {
    document.getElementById('status').innerText = "OFFLINE";
    document.getElementById('status').style.color = "red";
});


// Publish a command to broker -> ESP32
document.getElementById('btn_send').addEventListener('click', () => {

  let cmd = document.getElementById('input_cmd').value;
  const payload = JSON.stringify({ status: cmd, timestamp: Date.now() });

  // 3. Publish to the topic your ESP32 is SUBSCRIBED to
  client.publish('test/demo', payload, { qos: 1 }, (err) => {
    if (err) {
      console.error('Publish error:', err);
    } else {
      console.log('Command ' + payload + ' sent successfully');
    }
  });
});


</script>

</body> 