# Applications
This repository contains following applications targeting Amazon Web Services (AWS F1) FPGA architecture that have been revised to run on top of [InAccel Coral FPGA resource manager](https://inaccel.com/coral-fpga-resource-manager).

1. **GZip**
2. WebP
3. data_compression/xil_lz4
4. data_compression/xil_snappy

InAccel Coral *FPGA resource manager* allows:

- Instant scaling to multiple FPGA boards (f1.2x, f1.4x, f1.16x) through command line
- Secure sharing of the FPGA resources among different users and multiple processes or threads
- Simple accelerator invocation making it easy to build and accelerate parallel apps

That means that users can select on how many FPGAs they wish to deploy their applications and at the same time they are free to invoke their accelerators from multiple threads/processes simultaneously without worrying about the contection/serialization of the resources.

At the same time, accelerator access from the host code becomes much simpler. No need to modify your application to use an unfamiliar parallel programming language (like OpenCL). As an example, for the OpenCL host code of GZip **370 lines were deleted** and only **38 lines were added**.

### Documentation
For detailed usage instructions visit: [docs.inaccel.com](https://docs.inaccel.com)

### Support
For more product information contact: info@inaccel.com
