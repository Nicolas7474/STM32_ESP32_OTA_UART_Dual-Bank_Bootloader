### <!DOCTYPE html>

### <html>

### <head>

### &#x09;<meta http-equiv="content-type" content="text/html; charset=utf-8"/>

### &#x09;<title></title>

### &#x09;<meta name="generator" content="LibreOffice 26.2.4.2 (Windows)"/>

### &#x09;<meta name="created" content="2026-06-24T03:17:22"/>

### &#x09;<meta name="changed" content="2026-06-24T22:25:28"/>

### &#x09;<meta name="AppVersion" content="15.0000"/>

### &#x09;<style type="text/css">

### &#x09;	@page { size: 29.7cm 42cm; margin: 2cm }

### &#x09;	p { orphans: 2; color: #000000; direction: ltr; line-height: 115%; text-align: left; widows: 2; margin-bottom: 0.25cm; background: transparent }

### &#x09;	p.western { font-family: "Liberation Serif", serif; font-size: 12pt; so-language: de-DE }

### &#x09;	p.cjk { font-size: 12pt; so-language: zh-CN; font-family: "NSimSun" }

### &#x09;	p.ctl { font-family: "Arial"; font-size: 12pt; so-language: hi-IN }

### &#x09;	h3 { orphans: 2; color: #000000; text-align: left; widows: 2; margin-top: 0.25cm; margin-bottom: 0.21cm; direction: ltr; background: transparent; page-break-after: avoid }

### &#x09;	h3.western { font-family: "Liberation Serif", serif; font-weight: bold; font-size: 14pt; so-language: de-DE }

### &#x09;	h3.cjk { font-size: 14pt; so-language: zh-CN; font-weight: bold; font-family: "NSimSun" }

### &#x09;	h3.ctl { font-family: "Arial"; font-size: 14pt; so-language: hi-IN; font-weight: bold }

### &#x09;	h4 { orphans: 2; color: #000000; text-align: left; widows: 2; margin-top: 0.21cm; margin-bottom: 0.21cm; direction: ltr; background: transparent; page-break-after: avoid }

### &#x09;	h4.western { font-family: "Liberation Serif", serif; font-weight: bold; font-size: 12pt; so-language: de-DE }

### &#x09;	h4.cjk { font-size: 12pt; so-language: zh-CN; font-weight: bold; font-family: "NSimSun" }

### &#x09;	h4.ctl { font-family: "Arial"; font-size: 12pt; so-language: hi-IN; font-weight: bold }

### &#x09;</style>

### </head>

### <body lang="de-DE" text="#000000" link="#000080" vlink="#800000" dir="ltr"><h3 class="western" align="left">

### Project Architectural Overview: Dual-Bank Secure OTA Firmware Update

### System</h3>

### <p class="western" align="left"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left">This project implements a custom,

### bare-metal <b>Dual-Bank Over-The-Air (OTA) Firmware Update System</b>

### distributed across two microcontrollers: an <b>ESP32</b> (acting as

### the network gateway) and an <b>STM32F469</b> (acting as the main

### host/motherboard running the core application).</p>

### <p class="western" align="left">The philosophy of this architecture

### is <span style="font-weight: normal">robustness and anti-brick

### safety</span>. By leveraging the hardware dual-bank memory topology

### of the STM32F469, the system guarantees that a valid, running

### application always exists on the chip. Even if power cuts out halfway

### through an OTA download, or if a user accidentally uploads a

### completely broken or corrupted binary, the system automatically falls

### back to the previous working version.</p>

### <p class="western" align="left"><br/>

### <br/>

### 

### </p>

### <h3 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### 1\. The Global Hardware Architecture</h3>

### <p class="western" align="left">The system is divided into two clear

### processing layers connected via a physical asynchronous serial

### connection (UART):</p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+-----------------------------------+

### &#x20;           +-----------------------------------+</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;            ESP32                |             |            

### STM32F469             |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;       (Network Gateway)         |   UART6     |        (Main

### Motherboard)         |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;- Connects to Wi-Fi Secure AP    |============\&gt;|  - Runs Custom

### Bare-Metal Boot    |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;- Queries Version JSON Manifest  |  (Custom    |  - Manages Hardware

### Dual-Banks    |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;- Downloads App Binary via HTTPS |  Protocol)  |  - Dynamically

### Erases \&amp; Flashes  |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;- Segments Binary into 512B Chunks|             |  - Validates MD

### Anchors \&amp; CRCs    |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+-----------------------------------+

### &#x20;           +-----------------------------------+</font></font></font></p>

### <ol>

### &#x09;<li><p class="western" align="left"><b>The ESP32 :</b> It does not

### &#x09;know or care about the underlying physical hardware layout of the

### &#x09;STM32 (such as flash sectors or memory addresses). Its job is

### &#x09;entirely administrative: it connects to a secure Wi-Fi access point,

### &#x09;securely pulls a JSON manifest and a firmware binary via HTTPS from

### &#x09;a remote server, ensures packet delivery through a custom

### &#x09;chunk-based packetizing protocol, and reports system health status.</p></li>

### &#x09;<li><p class="western" align="left"><b>The STM32F469 :</b> It runs a

### &#x09;highly optimized, custom bare-metal bootloader written in type-safe

### &#x09;modern C++. It completely controls the non-volatile memory

### &#x09;controller, manages safe page/sector erasures on the fly, enforces

### &#x09;structural safeguards (such as protecting its own bootloader space

### &#x09;and EEPROM config sector), and physically switches memory execution

### &#x09;lanes via hardware bank swapping.<br/>

### Hardware recommendation for

### &#x09;UART:  Pull-up mode for PC6 (Tx) in GPIO register doesn't solve the

### &#x09;issue of parasitic/ultra-sensitive behavior (spurious bytes at

### &#x09;startup). PC6 must be pulled high with an external resistor (22K -\&gt;

### &#x09;100K).</p></li>

### </ol>

### <p class="western" align="left" style="margin-left: 0.75cm"><br/>

### <br/>

### 

### </p>

### <h3 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### 2\. Detailed Technical Breakthroughs</h3>

### <h4 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### A. The Stream Parser \&amp; Chunk-Based Communication Protocol</h4>

### <p class="western" align="left">To communicate safely over UART

### without dropping bytes or requiring massive, RAM-heavy local buffers,

### the system implements a strict <b>Finite State Machine (FSM) Stream

### Parser</b> (<font face="Liberation Mono, serif">ParseIncomingStream</font>)

### using a custom 16-byte fixed framing header coupled with a variable

### payload block up to 512 bytes.</p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+-----------------------------------------------------------------------------------------------------+

### &#x20;                               						16-BYTE FIXED

### HEADER<br/>

### +----------------------+--------------------+-------------------+-------------------------------------+</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;Total Size (4B)     |   Packet ID (2B)   | Payload Length(2B)|

### Version (M/m)(4B)| CRC32 Total(4B)  | </font></font></font>

### </p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+----------------------+--------------------+-------------------+-------------------------------------+

### &#x20;                            					  	VARIABLE PAYLOAD

### +-----------------------------------------------------------------------------------------------------+</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                               DATA BYTES (Max 512B)                

### &#x20;            			  |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+-----------------------------------------------------------------------------------------------------+</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                               4-BYTE TRAILING CRC32                

### &#x20;        				  |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+-----------------------------------------------------------------------------------------------------+</font></font></font></p>

### <p class="western" align="left">The parser processes data

### byte-by-byte through sequential states:</p>

### <ul>

### &#x09;<li><p class="western" align="left"><font face="Liberation Mono, serif">IDLE\_START</font>:

### &#x09;Listens exclusively for a unique synchronization constant

### &#x09;(<font face="Liberation Mono, serif">PACKET\_START\_BYTE</font>).</p></li>

### &#x09;<li><p class="western" align="left"><font face="Liberation Mono, serif">READ\_HEADER</font>:

### &#x09;Accumulates exactly 16 bytes and decodes fields from Big-Endian (MSB

### &#x09;first) networks into the processor's native architecture. It

### &#x09;includes a <i>defensive hardware safeguard</i> that caps any inbound

### &#x09;payload length strictly to 512 bytes to completely neutralize buffer

### &#x09;overflow security attacks.</p></li>

### &#x09;<li><p class="western" align="left"><font face="Liberation Mono, serif">READ\_DATA</font>:

### &#x09;Seamlessly routes incoming data directly into a local buffer until

### &#x09;the exact <font face="Liberation Mono, serif">payload\_length</font>

### &#x09;condition is satisfied.</p></li>

### &#x09;<li><p class="western" align="left"><font face="Liberation Mono, serif">READ\_CRC</font>:

### &#x09;Gathers a 4-byte frame CRC, immediately invoking flash execution

### &#x09;routines (<font face="Liberation Mono, serif">execute\_flash\_and\_respond</font>)

### &#x09;if the packet passes evaluation.</p></li>

### </ul>

### <h4 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### <br/>

### <br/>

### 

### </h4>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <h4 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### B. The Dynamic Sector Erase Engine</h4>

### <p class="western" align="left">Instead of blindly erasing the entire

### target application bank at the start of a download—which introduces

### an extended window of vulnerability if a network crash happens

### immediately—the STM32 bootloader features a specialized <span style="font-weight: normal">Dynamic

### Sector Erase Engine</span>.</p>

### <p class="western" align="left">As each payload chunk arrives, the

### bootloader dynamically calculates the absolute physical boundary

### address in Flash memory where that specific block will terminate

### (<font face="Liberation Mono, serif">packet\_end\_address</font>). It

### then references internal, compiled lookup tables (<font face="Liberation Mono, serif">bank1\_sectors</font>

### and <font face="Liberation Mono, serif">bank2\_sectors</font>) in

### reverse order to deduce exactly which hardware sector corresponds to

### that end-boundary.</p>

### <p class="western" align="left">Physical flash erasure is executed

### <i>only</i> when the streaming address crosses a boundary into a new,

### un-erased physical sector block. This significantly shortens flash

### erase windows and keeps as much historical code intact as possible

### during live transitions.</p>

### <p class="western" align="left">To inform the ESP32 there will be a

### delay (minimum 350ms) due to the Flash erase time and to avoid

### unexpected behavior, the STM32 sends a WAIT\_BYTE allowing the ESP32

### to enter a condition where an extended timeout on the uart\_read\_bytes

### function is set.</p>

### <p class="western" align="left"><br/>

### <br/>

### 

### </p>

### <h4 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### C. Comprehensive Multi-Layer Security \&amp; Structural Safeguards</h4>

### <p class="western" align="left">The project implements four defensive

### perimeter gates before any newly downloaded code is permitted to run:</p>

### <ol>

### &#x09;<li><p class="western" align="left"><b>Per-Packet CRC Check:</b>

### &#x09;Every incoming 512-byte block is subjected to a local software CRC32

### &#x09;check. If a bit flips during the UART transmission, a <font face="Liberation Mono, serif">NAK\_CRC\_ERROR</font>

### &#x09;is shot back to the ESP32, which immediately executes a seamless

### &#x09;retry loop.</p></li>

### &#x09;<li><p class="western" align="left"><b>Peripheral Register Locking:</b>

### &#x09;The internal Flash controller registers are securely locked down at

### &#x09;all times. They are explicitly opened (<font face="Liberation Mono, serif">flash\_unlock</font>)

### &#x09;micro-moments before an erase/write sequence and immediately clamped

### &#x09;shut (<font face="Liberation Mono, serif">flash\_lock</font>) the

### &#x09;instant a operation completes.</p></li>

### &#x09;<li><p class="western" align="left"><b>Hard Sector Exclusion Zones:</b>

### &#x09;The erase engine contains strict programmatic boundaries. For Bank

### &#x09;1, it refuses to erase anything at or below Sector 1, completely

### &#x09;shielding the bootloader itself. For Bank 2, it blocks modifications

### &#x09;to Sectors 12 and 13, ensuring that the critical system EEPROM

### &#x09;configuration sector can never be corrupted by an inbound

### &#x09;application payload.</p></li>

### &#x09;<li><p class="western" align="left"><b>Compile-Time Metadata

### &#x09;Verification:</b> Even if 100% of the bytes arrive correctly, the

### &#x09;bootloader will refuse to boot the code unless it discovers a

### &#x09;precise <font face="Liberation Mono, serif">magic\_anchor</font>

### &#x09;token (<font face="Liberation Mono, serif">\&quot;VERS\&quot;</font> /

### &#x09;<font face="Liberation Mono, serif">0x56455253</font>) compiled into

### &#x09;a fixed offset (<font face="Liberation Mono, serif">0x200</font>)

### &#x09;explicitly set aside by the application's linker script. It then

### &#x09;matches the compiled version numbers against the JSON cloud manifest

### &#x09;attributes to stop corrupt or incompatible binary flashes.</p></li>

### </ol>

### <h4 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### <br/>

### <br/>

### 

### </h4>

### <h4 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### D. The Volatile Restoration \&amp; Dual-Bank Boot Swap</h4>

### <p class="western" align="left">The STM32F469 operates via a hardware

### steering map managed by the <font face="Liberation Mono, serif">SYSCFG-\&gt;MEMRMP</font>

### register. When the <font face="Liberation Mono, serif">UFB\_MODE</font>

### configuration bit is cleared (<font face="Liberation Mono, serif">0</font>),

### Bank 1 maps to the boot address space <font face="Liberation Mono, serif">0x08000000</font>.

### When <font face="Liberation Mono, serif">UFB\_MODE</font> is set (<font face="Liberation Mono, serif">1</font>),

### the internal memory controller completely crosses the internal lanes,

### mapping Bank 2 to <font face="Liberation Mono, serif">0x08000000</font>.</p>

### <p class="western" align="left">Because <font face="Liberation Mono, serif">SYSCFG-\&gt;MEMRMP</font>

### is entirely volatile, its state disappears the moment the MCU loses

### power or resets. The system brilliantly treats this as a feature:</p>

### <ul>

### &#x09;<li><p class="western" align="left"><b>The Boot Handoff Strategy:</b>

### &#x09;On every single power cycle, the STM32 natively wakes up in its true

### &#x09;physical Bank 1 configuration. The bootloader boots for a fraction

### &#x09;of a microsecond and reads the non-volatile state stored safely in

### &#x09;physical Sector 13.</p></li>

### &#x09;<li><p class="western" align="left">If Sector 13 states that Bank 1

### &#x09;contains the active app, the bootloader leaves <font face="Liberation Mono, serif">UFB\_MODE</font>

### &#x09;at <font face="Liberation Mono, serif">0</font> and immediately

### &#x09;jumps to the execution vector.</p></li>

### &#x09;<li><p class="western" align="left">If Sector 13 states that Bank 2

### &#x09;contains the active app, the bootloader engages <font face="Liberation Mono, serif">SYSCFG-\&gt;MEMRMP

### &#x09;|= SYSCFG\_MEMRMP\_UFB\_MODE</font>, safely mirroring Bank 2 over to

### &#x09;the primary execution address space before executing the application

### &#x09;jump.</p></li>

### </ul>

### <p class="western" align="left">This architecture ensures that if a

### catastrophic application error occurs, a hard hardware reset will

### always safely route control back through the un-brickable bootloader

### first.</p>

### <h3 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### <br/>

### <br/>

### 

### </h3>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p class="western" align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <h3 class="western" align="left" style="line-height: 115%; margin-top: 0cm; margin-bottom: 0.25cm">

### 3\. Step-by-Step Update Execution Flow</h3>

### <p class="western" align="left">The full lifecycle of an update

### follows a strict, highly coordinated dance between the two

### microcontrollers:</p>

### <p class="western" align="left"><br/>

### <br/>

### 

### </p>

### <p align="left" style="line-height: 115%"> <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">ESP32

### (Gateway)                                                   

### STM32F469 (Host)</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">================

### &#x20;                                                 ==================</font></font></font></p>

### <p align="left" style="line-height: 115%"><br/>

### <br/>

### 

### </p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">(Connects

### Wi-Fi)    								     (Bootloader jumps to Application) </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \&lt;--- \[1. Request Version] -------------------------------- |

### (Running Application layer)    </font></font></font>

### </p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">(Queries

### Manifest JSON)                                                   </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \---- \[2. Returns Server Version (e.g., v1.2)] -----------\&gt; |

### (Application layer compares it against current </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |  version

### stored in Flash at section .fw\_metada </font></font></font>

### </p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">	

### |										   | (e.g. v1.0) and detects version minor is higher		 |  

### &#x20;                                                        |           

### &#x20;                                               </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \---- \[3. Wait for next reboot to perform the update] ----- | Writes

### 0x0A (new FW available on server) to Sect 13                         

### &#x20;                                   </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[System

### Reboots manually]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |

### \[Bootloader wakes up]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \&lt;--- \[4. Sends Request Firmware Byte (0x28)] ------------- | </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[Downloads

### App Binary via HTTPS]                                    | \[Enters

### Parse Stream Loop]</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[Buffers

### first 512B Block]                                          |</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |</font></font></font></p>

### <p align="left" style="line-height: 115%">  <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+----+

### &#x20;                                                          |</font></font></font></p>

### <p align="left" style="line-height: 115%">  <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### Loop for each chunk                                             |</font></font></font></p>

### <p align="left" style="line-height: 115%">  <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">v

### &#x20;  |                                                            |</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \---- \[5. Transmits Frame: Header + Data Payload + CRC] --\&gt; |</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Parses

### Stream FSM]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |

### \[Validates Packet CRC]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |

### \[Evaluates Target Sector]</font></font></font></p>

### <p align="left" style="line-height: 115%">   <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">	

### | \&lt;--- \[6. Transmits Packet WAIT\_BYTE (0x19)] -------------- |

### \[Erase Flash if crossing a new Sector]</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">(increased

### timeout for Erase time)						   | \[On-the-fly Flash Erase]           

### &#x20;                                                       </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |

### \[Programs Block to Flash]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \&lt;--- \[7. Transmits Packet ACK (0x06)] -------------------- |

### \[Advances Flash Pointers]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |</font></font></font></p>

### <p align="left" style="line-height: 115%">  <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">+----+

### &#x20;                                                          |</font></font></font></p>

### <p align="left" style="line-height: 115%">  <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### End Loop                                                        |</font></font></font></p>

### <p align="left" style="line-height: 115%">  <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">v

### &#x20;  |                                                            |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">https

### transaction ends, all full-packets (512B) are 		         | <br/>

### stored,

### checks for remaining bytes and append to buffer		   |</font></font></font></p>

### <p align="left" style="line-height: 115%"><font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">	

### | ---- \[8. Transmits Remaining Tail Bytes ] ---------------\&gt; |</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Total

### Bytes Received Match]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Checks

### Magic Anchor \&quot;VERS\&quot;]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          |

### \[Verifies Major/Minor Match]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Runs

### Comprehensive Global CRC]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Writes

### New Target Bank to Sec 13]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### \&lt;--- \[9. Transmits Final Success ACK] -------------------- |

### \[Clears Local States]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Flushes

### UART Pipelines]</font></font></font></p>

### <p align="left" style="line-height: 115%">       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                          | \[Invokes

### NVIC\_SystemReset()]</font></font></font></p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                       <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">|

### &#x20;                                                                    

### &#x20;   </font></font></font>

### </p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                    <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[BOOTLOADER

### REBOOTS]</font></font></font></p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                    <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[Reads

### New Bank Choice]</font></font></font></p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                    <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[Sets

### UFB\_MODE for Swap]</font></font></font></p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                    <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[Fetches

### App Linker MSP]</font></font></font></p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                    <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[Launches

### Reset\_Handler]</font></font></font></p>

### <p align="left" style="line-height: 115%">                           

### &#x20;                                    <font face="Liberation Mono, serif"><font size="2" style="font-size: 10pt"><font face="Liberation Mono, serif">\[New

### Application Boots Successfully!]</font></font></font></p>

### </body>

### </html>

