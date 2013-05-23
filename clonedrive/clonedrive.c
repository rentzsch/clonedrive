// clonedrive.c 1.0
//   Copyright (c) 2013 Jonathan 'Wolf' Rentzsch: http://rentzsch.com
//   Some rights reserved: http://opensource.org/licenses/mit
//   https://github.com/rentzsch/clonedrive

#include <stdio.h>
#include <assert.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/disk.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdarg.h>

#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>

#define kBufferSize (512ULL*1024ULL*1024ULL)
#define kGB (1024.0 * 1024.0 * 1024.0)

static void perrorf(const char * __restrict format, ...) {
    char message[100];
    
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    perror(message);
}

static uint64_t driveSize(int fd, const char *fdName) {
    uint64_t blockCount;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount) == -1) {
        if (errno == ENOTTY) {
            // It's really just a file.
            struct stat srcStat;
            if (fstat(fd, &srcStat) == -1) {
                perror("src fstat() failed");
                exit(EXIT_FAILURE);
            }
            
            return srcStat.st_size;
        } else {
            perrorf("%s ioctl(DKIOCGETBLOCKCOUNT) failed", fdName);
            exit(EXIT_FAILURE);
        }
    }
    
    uint32_t blockSize;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize) == -1) {
        perrorf("%s ioctl(DKIOCGETBLOCKSIZE) failed", fdName);
        exit(EXIT_FAILURE);
    }
    
    return ((uint64_t)blockSize) * blockCount;
}

static void printsha(uint8_t shaDigest[SHA_DIGEST_LENGTH]) {
    for (int shaByteIndex = 0; shaByteIndex < SHA_DIGEST_LENGTH; shaByteIndex++) {
        printf("%02x", shaDigest[shaByteIndex]);
    }
}

int main(int argc, const char *argv[]) {
    //
    // Verify args.
    //
    
    if (argc != 3) {
        fprintf(stderr, "Usage: sudo %s /dev/rdisk8 /dev/rdisk9\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    const char *srcPath = argv[1];
    const char *dstPath = argv[2];
    
    if (strcmp(srcPath, dstPath) == 0) {
        fprintf(stderr, "destination must be different from source\n");
        exit(EXIT_FAILURE);
    }
    
    //
    // Open src.
    //
    
    int srcFD;
    {{
        const char *srcDrivePath = srcPath;
        srcFD = open(srcDrivePath, O_RDONLY);
        if (srcFD == -1) {
            perror("src open() failed");
            exit(EXIT_FAILURE);
        }
    }}
    
    uint64_t srcDriveSize = driveSize(srcFD, "src");
    printf("src drive size: %llu bytes\n", srcDriveSize);
    
    //
    // Open dst.
    //
    
    int dstFD;
    {{
        const char *dstDrivePath = dstPath;
        dstFD = open(dstDrivePath, O_RDWR);
        if (dstFD == -1) {
            perror("dst open() failed");
            exit(EXIT_FAILURE);
        }
    }}
    
    uint64_t dstDriveSize = driveSize(dstFD, "dst");
    printf("dst drive size: %llu bytes\n", dstDriveSize);
    
    //
    // Make sure it fits.
    //
    
    if (srcDriveSize > dstDriveSize) {
        fprintf(stderr, "can't clone: src drive is bigger than dst drive");
        exit(EXIT_FAILURE);
    }
    
    //
    // Allocate the buffer we'll be reusing.
    //
    
    uint8_t *buffer = malloc(kBufferSize);
    
    //
    // Copy data from src to dst.
    //
    
    uint8_t readShaDigest[SHA_DIGEST_LENGTH];
    {{
        SHA_CTX readShaCtx;
        SHA1_Init(&readShaCtx);
        
        struct timeval cloneStart = {0, 0};
        
        for (uint64_t srcDrivePos = 0; srcDrivePos < srcDriveSize;) {
            {{
                // Throttle progress reporting to 1/sec.
                struct timeval now, delta;
                gettimeofday(&now, NULL);
                timersub(&now, &cloneStart, &delta);
                if (1 || delta.tv_sec >= 1) {
                    gettimeofday(&cloneStart, NULL);
                    printf("\rcloning %.0f%% (%f GB of %f GB)",
                           ((double)srcDrivePos / (double)srcDriveSize) * 100.0,
                           ((double)srcDrivePos) / kGB,
                           ((double)srcDriveSize) / kGB);
                    fflush(stdout);
                }
            }}
            
            size_t readSize = kBufferSize;
            {{
                uint64_t srcSizeLeft = srcDriveSize - srcDrivePos;
                if (srcSizeLeft < kBufferSize) {
                    readSize = srcSizeLeft;
                }
            }}
            
            if (read(srcFD, buffer, readSize) == -1) {
                perror("src read() failed");
                exit(EXIT_FAILURE);
            }
            
            SHA1_Update(&readShaCtx, buffer, readSize);
            
            if (write(dstFD, buffer, readSize) == -1) {
                perror("dst write() failed");
                exit(EXIT_FAILURE);
            }
            
            srcDrivePos += readSize;
        }
        
        //
        // Calc the final src checksum.
        //
        
        SHA1_Final(readShaDigest, &readShaCtx);
        printf("\n");
        printsha(readShaDigest);
        printf("  %s\n", srcPath);
        
        //
        // Close src -- we're done here.
        //
        
        close(srcFD);
        srcFD = -1;
    }}
    
    //
    // Verify clone by reading back the written data.
    //
    
    {{
        printf("verifying written data (it's now safe to unplug the src drive)\n");
        
        if (lseek(dstFD, 0LL, SEEK_SET) != 0LL) {
            perror("dst lseek() failed");
            exit(EXIT_FAILURE);
        }
        
        SHA_CTX readBackShaCtx;
        SHA1_Init(&readBackShaCtx);
        
        struct timeval verifyStart = {0,0};
        
        // Note we only read to srcDriveSize, since that's all we've written.
        for (uint64_t dstDrivePos = 0; dstDrivePos < srcDriveSize;) {
            {{
                // Throttle progress reporting to 1/sec.
                struct timeval now, delta;
                gettimeofday(&now, NULL);
                timersub(&now, &verifyStart, &delta);
                if (delta.tv_sec >= 1) {
                    gettimeofday(&verifyStart, NULL);
                    printf("\rverifying %.0f%% (%f GB of %f GB)",
                           ((double)dstDrivePos / (double)srcDriveSize) * 100.0,
                           ((double)dstDrivePos) / kGB,
                           ((double)srcDriveSize) / kGB);
                    fflush(stdout);
                }
            }}
            
            size_t readSize = kBufferSize;
            {{
                uint64_t dstSizeLeft = srcDriveSize - dstDrivePos;
                if (dstSizeLeft < kBufferSize) {
                    readSize = dstSizeLeft;
                }
            }}
            
            if (read(dstFD, buffer, readSize) == -1) {
                perror("dst read() failed");
                exit(EXIT_FAILURE);
            }
            
            SHA1_Update(&readBackShaCtx, buffer, readSize);
            
            dstDrivePos += readSize;
        }
        
        //--
        
        uint8_t readBackShaDigest[SHA_DIGEST_LENGTH];
        SHA1_Final(readBackShaDigest, &readBackShaCtx);
        printf("\n");
        printsha(readBackShaDigest);
        printf("  %s\n", dstPath);
        
        if (bcmp(readShaDigest, readBackShaDigest, SHA_DIGEST_LENGTH) == 0) {
            printf("SUCCESS\n");
        } else {
            printf("FAILURE\n");
        }
        
        //--
        
        close(dstFD);
        dstFD = -1;
    }}
    
    free(buffer);
    
    return 0;
}
