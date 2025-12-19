#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int
main(int argc, char *argv[])
{
  int fd;
  char buf[100];
  int initial_count, final_count;
  
  // Create a test file with some content
  fd = open("testfile.txt", O_CREATE | O_WRONLY);
  if (fd < 0) {
    printf("Failed to create test file\n");
    exit(1);
  }
  
  // Write some data to the file
  char *test_data = "This is a test file with exactly 100 bytes of data. We need to write enough text here to reach";
  write(fd, test_data, 100);
  close(fd);
  
  // Get initial read count
  initial_count = getreadcount();
  printf("Initial read count: %d\n", initial_count);
  
  // Open and read 100 bytes from the file
  fd = open("testfile.txt", O_RDONLY);
  if (fd < 0) {
    printf("Failed to open test file for reading\n");
    exit(1);
  }
  
  int bytes_read = read(fd, buf, 100);
  printf("Read %d bytes from file\n", bytes_read);
  close(fd);
  
  // Get final read count
  final_count = getreadcount();
  printf("Final read count: %d\n", final_count);
  printf("Increase in read count: %d\n", final_count - initial_count);
  
  // Verify the increase
  if (final_count - initial_count == 100) {
    printf("SUCCESS: Read count increased by exactly 100 bytes\n");
  } else {
    printf("ERROR: Expected increase of 100, got %d\n", final_count - initial_count);
  }
  
  // Clean up
  unlink("testfile.txt");
  
  exit(0);
}
