# Almux-Cybernet

A dual-processor System-on-Board (SoB) designed for high-performance embedded applications, combining compute, graphics, and flexible I/O control in a compact PCB.

---

## 🚀 Overview

**Almux Cybernet** is built around **two RP2350B processors**, enabling separation of workloads for improved performance and modularity. The board integrates networking, graphics output, USB capabilities, and an onboard I/O manager for streamlined development and programming.

[RP2350B Datasheet](https://pip-assets.raspberrypi.com/categories/1214-rp2350/documents/RP-008373-DS-2-rp2350-datasheet.pdf?disposition=inline)

---

## 🧠 Architecture

### 🔹 Dual-Core Processing Design

* **Primary Core (Main RP2350B)**

  * General-purpose compute
  * GPIO control
  * USB Host functionality
  * Interfaces with peripherals and expansion headers

* **Secondary Core (Graphics RP2350B)**

  * Dedicated to **HDMI output**
  * Handles **graphics processing and display pipelines**

* **I/O Manager / Programmer**

  * Based on an **Arduino ATmega chip**
  * Handles:

    * Programming interface
    * Auxiliary I/O control
    * Coordination between subsystems

---

## 🧩 Memory & Storage

Each RP2350B includes:

* **16 Mbit external SRAM**
* **1 Gbit external flash storage**

---

## 🔌 Connectivity & Interfaces

### 🧷 Headers & Expansion

* **SPI Connector**

  * Exposes **SPI1**
  * Mixed I/O lines from:

    * Main processor
    * I/O manager (ATmega)

* **GPIO Header**

  * Direct access to **main core GPIO pins**

* **TX/RX Header**

  * UART interface for:

    * Programming
    * Debugging

* **Battery / Power Header**

  * **5V input**
  * **GND**

---

### 🔗 Communication & I/O

* **USB-C Input**

  * Primary power and/or data interface

* **USB Host (Main Core)**

  * Enables connection to external USB devices

* **Ethernet**

  * Powered by **USR-K6 module**
  * Provides network connectivity

---

### 🖥️ Display

* **HDMI Output**

  * Driven by the **secondary RP2350B**
  * Dedicated graphics pipeline for display tasks

---

## ⚙️ Design Philosophy

Almux Cybernet separates responsibilities across processors:

* 🧮 **Main Core** → logic, control, and external interfacing
* 🎮 **Secondary Core** → graphics and HDMI
* 🔧 **ATmega** → programming + I/O management

This modular approach improves:

* Performance isolation
* Flexibility
* Expandability

---

## 🛠️ Use Cases

* Embedded systems development
* Custom computing platforms
* Edge devices with display + networking
* Robotics and control systems
* Experimental multi-processor architectures

---

## 📌 Status

> 🚧 Active development
> Hardware and firmware are evolving. Expect changes.

---

## 📷 Board Layout

*(Insert PCB renders or photos here)*

---

## 🤝 Contributing

Contributions, ideas, and feedback are welcome.
Feel free to open issues or submit pull requests.

---

## 📄 License

Is found in the LISENCE file

---
