/**********
Copyright (c) 2018, Xilinx, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********/

#include "xil_gzip.h"
#define FORMAT_0 31
#define FORMAT_1 139
#define VARIANT 8
#define REAL_CODE 8 
#define OPCODE 3

extern unsigned long crc;
#define CHUNK_16K 16384

int batch_buf_release = 0;

void zip(std::string & inFile_name, std::ofstream & outFile, uint8_t *zip_out, uint32_t enbytes) {

    // 2 bytes of magic header
    outFile.put(FORMAT_0);
    outFile.put(FORMAT_1);
   
    // 1 byte Compression method
    outFile.put(VARIANT);
    
    // 1 byte flags
    uint8_t flags = 0;
    flags |= REAL_CODE;
    outFile.put(flags);

    // 4 bytes file modification time in unit format
    unsigned long time_stamp = 0;
    struct stat istat;
    stat(inFile_name.c_str(), &istat);
    time_stamp = istat.st_mtime;
    //put_long(time_stamp, outFile);
    uint8_t time_byte = 0;
    time_byte = time_stamp;
    outFile.put(time_byte);
    time_byte = time_stamp >> 8;
    outFile.put(time_byte);
    time_byte = time_stamp >> 16;
    outFile.put(time_byte);
    time_byte = time_stamp >> 24;
    outFile.put(time_byte);

    // 1 byte extra flag (depend on compression method)
    uint8_t deflate_flags = 0;
    outFile.put(deflate_flags);

    // 1 byte OPCODE - 0x03 for Unix
    outFile.put(OPCODE);
    
    // Dump file name
    for(int i = 0; inFile_name[i] != '\0'; i++){
        outFile.put(inFile_name[i]); 
    }
    outFile.put(0);

    outFile.write((char *)zip_out, enbytes);    
    
    unsigned long ifile_size = istat.st_size;
    uint8_t crc_byte = 0;
    long crc_val = 0;
    crc_byte = crc_val;
    outFile.put(crc_byte);
    crc_byte = crc_val >> 8;
    outFile.put(crc_byte);
    crc_byte = crc_val >> 16;
    outFile.put(crc_byte);
    crc_byte = crc_val >> 24;
    outFile.put(crc_byte);
    
    uint8_t len_byte = 0;
    len_byte = ifile_size;
    outFile.put(len_byte);
    len_byte = ifile_size >> 8;
    outFile.put(len_byte);
    len_byte = ifile_size >> 16;
    outFile.put(len_byte);
    len_byte = ifile_size >> 24;
    outFile.put(len_byte);
}

void xil_gzip::compress_file(std::vector<std::string> & inFile_batch,
                             std::vector<std::string> & outFile_batch,  
                             std::vector<int> & enbytes,
                             int chunk_size
                            ) {
    // Input file size holder
    std::vector<int> input_size_cntr; 

    // Allocate inaccel input and output buffers based on chunk size
    // These buffer pointers hold data related to 
    // Each input file 
    std::vector<inaccel::vector<uint8_t>> input_vectors(chunk_size);
    std::vector<inaccel::vector<uint8_t>> output_vectors(chunk_size);

    uint32_t local_enbytes[chunk_size];
    
    // Find out input sizes  
    for(int i = 0; i < chunk_size; i++) {
        
        std::ifstream inFile(inFile_batch[i].c_str(), std::ifstream::binary);
        if(!inFile) {
            std::cout << "Unable to open file" << std::endl;
            exit(1);
        }
        
        inFile.seekg(0, inFile.end);
        long input_size = inFile.tellg();
        inFile.seekg(0, inFile.beg);
        input_size_cntr.push_back(input_size);
   
        input_vectors[i].reserve(input_size_cntr[i]);
        output_vectors[i].resize(input_size_cntr[i]);
        inFile.read((char *)input_vectors[i].data(), input_size_cntr[i]);

        inFile.close();
    }

    // GZip batch mode compress
    compress(input_vectors, output_vectors, input_size_cntr, local_enbytes);

    
    // Fill the .gz buffer with encoded stream
    for(int i = 0; i < chunk_size; i++) {
        
        enbytes.push_back(local_enbytes[i]);

        std::ofstream outFile(outFile_batch[i].c_str(), std::ofstream::binary);
    
        // Dump compressed bytes to .gz file
        zip(inFile_batch[i], outFile, output_vectors[i].data(), local_enbytes[i]);        
        outFile.close();
    }   
}


uint32_t xil_gzip::compress_file(std::string & inFile_name, std::string & outFile_name) 
{
    std::ifstream inFile(inFile_name.c_str(), std::ifstream::binary);
    std::ofstream outFile(outFile_name.c_str(), std::ofstream::binary);
    
    if(!inFile) {
        std::cout << "Unable to open file";
        exit(1);
    }

    inFile.seekg(0, inFile.end);
    long input_size = inFile.tellg();
    inFile.seekg(0, inFile.beg);   
    
    inaccel::vector<uint8_t> gzip_in (input_size);
    inaccel::vector<uint8_t> gzip_out(input_size);
    
    inFile.read((char *)gzip_in.data(), input_size); 

    // GZip Compress 
    uint32_t enbytes = compress(gzip_in, gzip_out, input_size);

    // Pack GZip encoded stream .gz file
    zip(inFile_name, outFile, gzip_out.data(), enbytes);

    // Close file 
    inFile.close();
    outFile.close();
    return enbytes;
}

int validate(std::string & inFile_name, std::string & outFile_name) {


#ifdef VERBOSE 
   orig< "Validation     -- ";
#endif   

    std::string command = "cmp " + inFile_name + " " + outFile_name;
    int ret = system(command.c_str());
    return ret;
}


// GZip Compress (Batch of Files) using -l <file.list> -b <size of batch>
void xil_gzip::compress(std::vector<inaccel::vector<uint8_t>> & in,
                        std::vector<inaccel::vector<uint8_t>> & out,
                        std::vector<int> & input_size,
                        uint32_t *file_encode_bytes 
                       ) {
    auto total_start = std::chrono::high_resolution_clock::now();

    // Figure out the total files sent in batch mode
    int total_file_count = input_size.size();

    inaccel::vector<uint32_t> sizeOut[total_file_count];

    std::vector<std::future<void>> responses;

    // Main loop for overlap computation
    for(int file = 0; file < total_file_count; file++) {

        long insize = (long) input_size[file];

        sizeOut[file].resize(1);

        // Set kernel arguments
        inaccel::request request{"com.xilinx.applications.GZip.compress"};
        request.arg(in[file]).arg(out[file]).arg(sizeOut[file]).arg(insize);
        
        // Kernel invocation
        responses.push_back(inaccel::submit(request));

    } // End of mail loop - Batch mode

    for(int file = 0; file < total_file_count; file++) {
        responses[file].get();
        size_t out_cntr = (sizeOut[file].data())[0];
        file_encode_bytes[file] = out_cntr;
    }
    auto total_end = std::chrono::high_resolution_clock::now(); 
    
    long sum_size = 0;
    for(unsigned int i = 0; i < input_size.size(); i++) 
        sum_size += input_size[i];
    
    auto total_time_ns = std::chrono::duration<double, std::nano>(total_end - total_start);
    float total_in_mbps = (float)sum_size * 1000 / total_time_ns.count();
    printf("%d", total_file_count);
    printf("\t\t");
    printf("%.2f",total_in_mbps);
    printf("\n");
}

// GZip compress (Single File) using -i option
uint32_t xil_gzip::compress(inaccel::vector<uint8_t> & in,
                            inaccel::vector<uint8_t> & out,
                            long input_size
                          ) {

    auto total_start = std::chrono::high_resolution_clock::now(); 
    long size_per_unit = (input_size - 1)+ 1;
    long size_of_each_unit_4k = ((size_per_unit -1)/4096 + 1) * 4096;
    
    inaccel::vector<uint32_t> sizeOut(1);
    
    long size_for_each_unit =  size_of_each_unit_4k;
    if (size_for_each_unit > input_size){
        size_for_each_unit = input_size;
    }
    // Set kernel arguments
    inaccel::request request{"com.xilinx.applications.GZip.compress"};
    request.arg(in).arg(out).arg(sizeOut).arg(size_for_each_unit);
    
    // Kernel invocation
    inaccel::submit(request).get();

    uint32_t out_cntr = (sizeOut.data())[0];

    auto total_end = std::chrono::high_resolution_clock::now(); 

    auto total_time_ns = std::chrono::duration<double, std::nano>(total_end - total_start);
    float throughput_in_mbps_1 = (float)input_size * 1000 / total_time_ns.count();
    std::cout << std::fixed << std::setprecision(2) << throughput_in_mbps_1;
    return out_cntr;
}

