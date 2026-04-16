# 🍈 OS Jackfruit – Container Runtime System

## 📌 Overview
**OS Jackfruit** is a lightweight container runtime system implemented in **C**, designed to demonstrate core **Operating System concepts** such as process isolation, namespace usage, and kernel–user space interaction.

This project simulates the fundamental working principles of modern container engines by creating isolated execution environments using low-level Linux features. It provides hands-on experience with system-level programming and OS internals.

---

## 🎯 Objectives
- Understand Linux process creation and lifecycle
- Implement process isolation techniques
- Explore kernel–user space communication
- Learn Linux kernel module development
- Analyze CPU, memory, and I/O behavior
- Simulate a container runtime environment

---

## ⚙️ Features
- 🧩 Process isolation using Linux mechanisms  
- 📦 Lightweight container-like execution  
- 🧠 Kernel module for monitoring processes  
- 📊 Resource usage tracking (CPU, memory, I/O)  
- 💻 Command-line interface for runtime control  
- 🧪 Stress testing using custom workload programs  

---

## 🛠️ Technologies Used
- **C Programming Language**
- **Linux System Calls**
- **Linux Kernel Modules (LKM)**
- **GCC Compiler**
- **Makefile**
- **Shell Scripting**

---



## 🚀 Setup & Installation

### 1️⃣ Prerequisites
- Linux OS (Ubuntu recommended)
- GCC Compiler
- Kernel headers installed
- Root privileges

Install dependencies:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
2️⃣ Build the Project
cd boilerplate
make



