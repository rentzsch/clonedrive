// clonedrive.c 1.1
//   Copyright (c) 2013-2014 Jonathan 'Wolf' Rentzsch: http://rentzsch.com
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

static void printSha(uint8_t shaDigest[SHA_DIGEST_LENGTH]) {
    for (int shaByteIndex = 0; shaByteIndex < SHA_DIGEST_LENGTH; shaByteIndex++) {
        printf("%02x", shaDigest[shaByteIndex]);
    }
}

static void readDriveSha(int fd,
                         const char *fdName,
                         uint64_t fdReadSize,
                         uint8_t *buffer,
                         const char *taskName,
                         uint8_t shaDigest[SHA_DIGEST_LENGTH])
{
    if (lseek(fd, 0LL, SEEK_SET) != 0LL) {
        perrorf("%s lseek() failed", fdName);
        exit(EXIT_FAILURE);
    }
    
    SHA_CTX shaCtx;
    SHA1_Init(&shaCtx);
    
    struct timeval verifyStart = {0,0};
    
    for (uint64_t drivePos = 0; drivePos < fdReadSize;) {
        {{
            // Throttle progress reporting to 1/sec.
            struct timeval now, delta;
            gettimeofday(&now, NULL);
            timersub(&now, &verifyStart, &delta);
            if (delta.tv_sec >= 1) {
                gettimeofday(&verifyStart, NULL);
                printf("\r%s %.0f%% (%f GB of %f GB)",
                       taskName,
                       ((double)drivePos / (double)fdReadSize) * 100.0,
                       ((double)drivePos) / kGB,
                       ((double)fdReadSize) / kGB);
                fflush(stdout);
            }
        }}
        
        size_t readSize = kBufferSize;
        {{
            uint64_t fdSizeLeft = fdReadSize - drivePos;
            if (fdSizeLeft < kBufferSize) {
                readSize = fdSizeLeft;
            }
        }}
        
        if (read(fd, buffer, readSize) == -1) {
            perrorf("%s read() failed", fdName);
            exit(EXIT_FAILURE);
        }
        
        SHA1_Update(&shaCtx, buffer, readSize);
        
        drivePos += readSize;
    }
    
    printf("\r%s 100%% (%f GB of %f GB)\n",
           taskName,
           ((double)fdReadSize) / kGB,
           ((double)fdReadSize) / kGB);
    
    SHA1_Final(shaDigest, &shaCtx);
}

int main(int argc, const char *argv[]) {
    //
    // Verify args.
    //
    
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: sudo %s /dev/rdisk8 /dev/rdisk9\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    const char *srcPath = argv[1];
    const char *dstPath = argc == 2 ? NULL : argv[2];
    
    if (dstPath) {
        if (strcmp(srcPath, dstPath) == 0) {
            fprintf(stderr, "destination must be different from source\n");
            exit(EXIT_FAILURE);
        }
    }
    
    //
    // Open src (read-only).
    //
    
    int srcFD = -1;
    {{
        srcFD = open(srcPath, O_RDONLY);
        if (srcFD == -1) {
            perror("src drive open() failed");
            exit(EXIT_FAILURE);
        }
    }}
    
    uint64_t srcDriveSize = driveSize(srcFD, "src drive");
    printf("src drive size: %llu bytes\n", srcDriveSize);
    
    //
    // Open dst read-only to ensure it's big enough.
    //
    
    int dstFD = -1;
    if (dstPath) {
        dstFD = open(dstPath, O_RDONLY);
        if (dstFD == -1) {
            perror("dst drive open(O_RDONLY) failed");
            exit(EXIT_FAILURE);
        }
        uint64_t dstDriveSize = driveSize(dstFD, "dst drive");
        printf("dst drive size: %llu bytes\n", dstDriveSize);
        
        //
        // Make sure it fits.
        //
        
        if (srcDriveSize > dstDriveSize) {
            fprintf(stderr, "can't clone: src drive is bigger than dst drive");
            exit(EXIT_FAILURE);
        }
    }
    
    //
    // Allocate one big reusable buffer.
    //
    
    uint8_t *buffer = malloc(kBufferSize);
    
    //
    // Repeatable Read: First src read.
    //
    
    uint8_t srcFirstDigest[SHA_DIGEST_LENGTH];
    readDriveSha(srcFD,
                 "src drive",
                 srcDriveSize,
                 buffer,
                 "initial read of src drive",
                 srcFirstDigest);
    
    //
    // Repeatable Read: Second src read.
    //
    
    uint8_t srcSecondDigest[SHA_DIGEST_LENGTH];
    readDriveSha(srcFD,
                 "src drive",
                 srcDriveSize,
                 buffer,
                 "verifying repeatable read of src drive",
                 srcSecondDigest);
    
    //
    // Repeatable Read: Ensure we read the same data twice in a row.
    //
    
    if (bcmp(srcFirstDigest, srcSecondDigest, SHA_DIGEST_LENGTH) == 0) {
        printf("repeatable read of src drive successful\n");
    } else {
        printf("FAILURE couldn't repeatedly read src drive\n");
        
        printf("first read:  ");
        printSha(srcFirstDigest);
        printf("\n");
        
        printf("second read: ");
        printSha(srcSecondDigest);
        printf("\n");
        
        exit(EXIT_FAILURE);
    }
    
    if (!dstPath) {
        // Just a repeatable read. Our work here is done.
        goto cleanup;
    }
    
    //
    // Re-open dst (read-write).
    //
    
    {{
        if (close(dstFD) == -1) {
            perror("dst close() failed");
            exit(EXIT_FAILURE);
        }
        dstFD = open(dstPath, O_RDWR);
        if (dstFD == -1) {
            perror("dst open(O_RDWR) failed");
            exit(EXIT_FAILURE);
        }
    }}
    
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
                if (delta.tv_sec >= 1) {
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
        
        printf("\rcloning 100%% (%f GB of %f GB)\n",
               ((double)srcDriveSize) / kGB,
               ((double)srcDriveSize) / kGB);
        
        //
        // Calc the final src checksum.
        //
        
        SHA1_Final(readShaDigest, &readShaCtx);
    }}
    
    //
    // Double-check the data didn't change during the copy itself.
    //
    
    if (bcmp(srcSecondDigest,readShaDigest, SHA_DIGEST_LENGTH) != 0) {
        printf("FAILURE src data changed during copy\n");
        
        printf("first & second reads: ");
        printSha(srcSecondDigest);
        printf("\n");
        
        printf("read during copy:     ");
        printSha(readShaDigest);
        printf("\n");
        
        exit(EXIT_FAILURE);
    }
    
    //
    // Re-open dst read-only for verification.
    //
    
    {{
        if (close(dstFD) == -1) {
            perror("dst close() failed");
            exit(EXIT_FAILURE);
        }
        dstFD = open(dstPath, O_RDONLY);
        if (dstFD == -1) {
            perror("dst open(O_RDONLY) failed");
            exit(EXIT_FAILURE);
        }
    }}
    
    //
    // Verify clone by reading back the written data.
    //
    
    {{
        printf("verifying written data (it's now safe to unplug the src drive)\n");
        
        uint8_t readBackShaDigest[SHA_DIGEST_LENGTH];
        readDriveSha(dstFD,
                     "dst drive",
                     srcDriveSize, // Only read to srcDriveSize, since that's all we've written.
                     buffer,
                     "verifying write",
                     readBackShaDigest);
        
        if (bcmp(readShaDigest, readBackShaDigest, SHA_DIGEST_LENGTH) == 0) {
            printf("SUCCESS\n");
        } else {
            printf("FAILURE\n");
            
            printf("src read:      ");
            printSha(readShaDigest);
            printf("\n");
            
            printf("dst read back: ");
            printSha(readBackShaDigest);
            printf("\n");
        }
    }}
    
cleanup:
    
    //
    // Free our buffer.
    //
    
    free(buffer);
    
    //
    // Close dst for the final time.
    //
    
    if (dstFD != -1) {
        if (close(dstFD) == -1) {
            perror("dst close() failed");
            exit(EXIT_FAILURE);
        }
        dstFD = -1;
    }
    
    //
    // Close src.
    //
    
    if (srcFD != -1) {
        if (close(srcFD) == -1) {
            perror("src close() failed");
            exit(EXIT_FAILURE);
        }
        srcFD = -1;
    }
    
    return 0;
}
