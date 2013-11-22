// clonedrive.c 1.0.1
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
#include <dispatch/dispatch.h>

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

static void verifysha(const char* path, uint8_t expectedShaDigest[SHA_DIGEST_LENGTH], void (^completion)(bool match))
{
    int srcFD;
    {{
        const char *srcDrivePath = path;
        srcFD = open(srcDrivePath, O_RDONLY);
        if (srcFD == -1) {
            perror("verifysha open() failed");
            exit(EXIT_FAILURE);
        }
    }}
    
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_io_t srcChannel = dispatch_io_create(DISPATCH_IO_STREAM, srcFD, q, ^(int error) {
        if (error) {
            perror("dispatch_io error");
            exit(EXIT_FAILURE);
        }
        close(srcFD);
    });
    
    uint64_t srcDriveSize = driveSize(srcFD, "verify");
    
    SHA_CTX* readShaCtxPtr = malloc(sizeof(SHA_CTX));
    SHA1_Init(readShaCtxPtr);
    
    __block uint64_t srcDrivePos = 0;
    __block struct timeval verifyStart = {0,0};
    
    dispatch_io_read(srcChannel, 0, SIZE_MAX, q, ^(bool done, dispatch_data_t data, int error) {
        bool close = false;
        if (error) {
            fprintf(stderr, "Error: %s", strerror(error));
            close = true;
        }
        
        {{
            // Throttle progress reporting to 1/sec.
            struct timeval now, delta;
            gettimeofday(&now, NULL);
            timersub(&now, &verifyStart, &delta);
            if (delta.tv_sec >= 1) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    gettimeofday(&verifyStart, NULL);
                    printf("\rverifying %.0f%% (%f GB of %f GB)",
                           ((double)srcDrivePos / (double)srcDriveSize) * 100.0,
                           ((double)srcDrivePos) / kGB,
                           ((double)srcDriveSize) / kGB);
                    fflush(stdout);
                });
            }
        }}
        
        const size_t rxd = data ? dispatch_data_get_size(data) : 0;
        
        if (rxd) {
            dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t offset, const void *buffer, size_t size) {
                SHA1_Update(readShaCtxPtr, buffer, size);
                return true;
            });
            srcDrivePos += rxd;
        }
        else {
            close = true;
        }
        
        if (close) {
            dispatch_io_close(srcChannel, 0);
            dispatch_release(srcChannel);
            
            uint8_t readBackShaDigest[SHA_DIGEST_LENGTH];
            SHA1_Final(readBackShaDigest, readShaCtxPtr);
            
            free(readShaCtxPtr);
            
            printf("\n");
            printsha(readBackShaDigest);
            printf("  %s\n", path);
            
            if (bcmp(expectedShaDigest, readBackShaDigest, SHA_DIGEST_LENGTH) == 0) {
                completion(true);
            } else {
                completion(false);
            }
        }
    });
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
    // Setup dispatch_io channels
    //
    
    dispatch_group_t waiter = dispatch_group_create();
    dispatch_group_enter(waiter); // to prevent races
    dispatch_group_enter(waiter); // for read channel
    dispatch_group_enter(waiter); // for write channel

    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    dispatch_io_t srcChannel = dispatch_io_create(DISPATCH_IO_STREAM, srcFD, q, ^(int error) {
        if (error) {
            perror("dispatch_io error");
            exit(EXIT_FAILURE);
        }
        close(srcFD);
        dispatch_group_leave(waiter);

    });
    
    dispatch_io_t dstChannel = dispatch_io_create(DISPATCH_IO_STREAM, dstFD, q, ^(int error) {
        if (error) {
            perror("dispatch_io error");
            exit(EXIT_FAILURE);
        }
        close(dstFD);
        dispatch_group_leave(waiter);
    });
    
    dispatch_io_set_low_water(srcChannel, kBufferSize >> 2);
    dispatch_io_set_high_water(srcChannel, kBufferSize);
    
    //
    // Copy data from src to dst.
    //
    
    uint8_t _readShaDigest[SHA_DIGEST_LENGTH];
    uint8_t* readShaDigest = _readShaDigest;
    {{
        SHA_CTX readShaCtx;
        SHA_CTX* const readShaCtxPtr = &readShaCtx;
        SHA1_Init(&readShaCtx);
        
        __block struct timeval cloneStart = {0, 0};
        __block uint64_t srcDrivePos = 0;
        
        
        dispatch_io_read(srcChannel, 0, SIZE_MAX, q, ^(bool done, dispatch_data_t data, int error) {
            bool close = false;
            if (error) {
                fprintf(stderr, "Error: %s", strerror(error));
                close = true;
            }
            
            {{
                // Throttle progress reporting to 1/sec.
                struct timeval now, delta;
                gettimeofday(&now, NULL);
                timersub(&now, &cloneStart, &delta);
                if (delta.tv_sec >= 1) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        gettimeofday(&cloneStart, NULL);
                        printf("\rcloning %.0f%% (%f GB of %f GB)",
                               ((double)srcDrivePos / (double)srcDriveSize) * 100.0,
                               ((double)srcDrivePos) / kGB,
                               ((double)srcDriveSize) / kGB);
                        fflush(stdout);
                    });
                }
            }}
            
            dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t offset, const void *buffer, size_t size) {
                SHA1_Update(readShaCtxPtr, buffer, size);
                return true;
            });
            
            const size_t rxd = data ? dispatch_data_get_size(data) : 0;
            
            if (rxd) {
                dispatch_io_write(dstChannel, 0, data, q, ^(bool done, dispatch_data_t data, int error) {
                    bool close = false;
                    if (error) {
                        fprintf(stderr, "Error: %s", strerror(error));
                        close = true;
                    }
                    if (close) {
                        dispatch_io_close(dstChannel, DISPATCH_IO_STOP);
                        dispatch_release(dstChannel);
                    }
                    
                });
                srcDrivePos += rxd;
            }
            else {
                close = true;
            }
            
            if (close) {
                dispatch_io_close(srcChannel, DISPATCH_IO_STOP);
                dispatch_release(srcChannel);
                
                dispatch_io_close(dstChannel, 0);
                dispatch_release(dstChannel);
            }
        });
        
        //
        // Copy is done. Kick off verify.
        //
        
        dispatch_group_notify(waiter, dispatch_get_main_queue(), ^{
            SHA1_Final(readShaDigest, readShaCtxPtr);
            printf("\n");
            printsha(readShaDigest);
            printf("  %s\n", srcPath);
            
            //
            // Verify clone by reading back the written data.
            //
            
            printf("verifying written data (it's now safe to unplug the src drive)\n");
            
            verifysha(dstPath, readShaDigest, ^(bool match) {
                if (match) {
                    printf("SUCCESS\n");
                    exit(EXIT_SUCCESS);
                } else {
                    exit(EXIT_FAILURE);
                    printf("FAILURE\n");
                }
            });
            
        });
        
        dispatch_group_leave(waiter); // to prevent races
        
        dispatch_main(); // never returns
        
    }}
    
    return EXIT_SUCCESS;
}
