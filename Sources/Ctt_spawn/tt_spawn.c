#include "include/tt_spawn.h"
#include <assert.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/poll.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/resource.h>

#pragma mark Spawn Configuration
//this is a spawn configuration. all parameters and options that are available in tt_spawn are contained in this structure.
typedef struct tt_spawn_config {
	//the path to be executed and the working directory to use while executing
    char *path;
    char *wd;
	
	//arguments for the executable
    char **argv;
    int argv_count;
    
    //environment variables to be assigned to the container before forking
    char **env_keys;
    char **env_values;
	int env_count;
	
	//primary data channels
    tt_pipe stdin;
    tt_pipe stdout;
    tt_pipe stderr;
	
	//internal data channels
    tt_pipe parent; //this represents the pipe that connects the the process monitor in the core application
	tt_pipe latch; //helps synchronize the engagement between core application and container process
} tt_spawn_config;

//copies a spawn configuration and into new address space. this function is used to safely translate the configuration from the core application to the container process after clone is called
static tt_spawn_config tt_spawn_config_copy(tt_spawn_config *input) {
    tt_spawn_config launchConfig;
    
    //copy the path and working directory
    launchConfig.path = strdup(input->path);
    launchConfig.wd = strdup(input->wd);

    //initialize the argument array
    launchConfig.argv = malloc((input->argv_count + 1)*sizeof(char*));
	launchConfig.argv_count = input->argv_count;
    int i = 0;
    while(i < input->argv_count) {
    	launchConfig.argv[i] = (char*)strdup(input->argv[i]);
    	i = i + 1;
    }
	launchConfig.argv[i] = NULL;

	i = 0; //reset iterator
	
	//initialize the environment variables
	launchConfig.env_keys = malloc((input->env_count + 1)*sizeof(char*));
	launchConfig.env_values = malloc((input->env_count + 1)*sizeof(char*));
	launchConfig.env_count = input->env_count;
    while(i < input->env_count) {
    	launchConfig.env_keys[i] = (char*)strdup(input->env_keys[i]);
		launchConfig.env_values[i] = (char*)strdup(input->env_values[i]);
    	i = i + 1;
    }
	launchConfig.env_keys[i] = NULL;	//null terminate keys
	launchConfig.env_values[i] = NULL;	//null terminate values
	
	//assign the pipe structures
    launchConfig.stdin = input->stdin;
    launchConfig.stdout = input->stdout;
    launchConfig.stderr = input->stderr;
    launchConfig.parent = input->parent;
	launchConfig.latch = input->latch;
    return launchConfig;
}

//frees a spawn configuration's allocations from memory
static void tt_spawn_config_free(tt_spawn_config *input) {
    free(input->path);
    free(input->wd);

	//free the argument array, including the strings stored in the array
    int i = 0;
    while(i < input->argv_count) {
    	void* pointerToFree = (void*)input->argv[i];
    	free(pointerToFree);
    	i = i + 1;
    }
    free(input->argv);
    
    //free the environment variables (keys and values)
    i = 0;
    while (i < input->env_count) {
		//free the individual entries in the base array
    	void* keyToFree = (void*)input->env_keys[i];
		void* valueToFree = (void*)input->env_values[i];
    	free(keyToFree);
		free(valueToFree);
    	i = i + 1;
    }
	//free the base arrays
    free(input->env_keys);
	free(input->env_values);
}

//create a new spawn configuration structure with the given values
static tt_spawn_config tt_spawn_config_init(const char *path, const char **argv, int argv_count, const char *wd, const char **env_keys, const char **env_values, int env_count, tt_pipe stdin, tt_pipe stdout, tt_pipe stderr, tt_pipe parent, tt_pipe latch) {
    tt_spawn_config launchConfig;
    launchConfig.path = strdup(path);
    launchConfig.wd = strdup(wd);
    
    //initialize the argument array
    launchConfig.argv = malloc((argv_count + 1)*sizeof(char*));
    launchConfig.argv_count = argv_count;
    int i = 0;
    while(i < argv_count) {
    	launchConfig.argv[i] = strdup(argv[i]);
    	i = i + 1;
    }
    launchConfig.argv[i] = NULL;
	
	//initialize the environment keys and values
	launchConfig.env_keys = malloc((env_count + 1)*sizeof(char*));
	launchConfig.env_values = malloc((env_count + 1)*sizeof(char*));
	launchConfig.env_count = env_count;
	i = 0;
	while(i < env_count) {
		launchConfig.env_keys[i] = strdup(env_keys[i]);
		launchConfig.env_values[i] = strdup(env_values[i]);
		i = i + 1;
	}
	launchConfig.env_keys[i] = NULL;	//null terminate the keys array
	launchConfig.env_values[i] = NULL;	//null terminate the values array
    
    //assign the pipes to the new launchConfig object
    launchConfig.stdin = stdin;
    launchConfig.stdout = stdout;
    launchConfig.stderr = stderr;
    launchConfig.parent = parent;
    launchConfig.latch = latch;
    return launchConfig;
}

#pragma mark File Descriptor Polling
//functions, structures, and helpers to monitor up to 16 file handles.
typedef struct fh_result {
	int fh;
	int res; //-1: closed, 0: not available for reading/writing, 1: available for reading/writing
} fh_result;
typedef struct fh_result_set {
	struct fh_result fhs[16];
	int count;
} fh_result_set;

//can only poll (up to) 16 file descriptors at a time
//array of the file descriptors that are able to be read or written from (in the form of a tt_result_set)
static struct fh_result_set poll_fds(int* read, int readingCount, int* write, int writingCount, int milliTimeout) {
	struct pollfd mon_fds[16];
	int p = 0; //index for placing file descriptors into the mon_fds array
	int i = 0; //basic iterator
	
	//integrate the given file handles for reading inbound data
	while(i < readingCount && p < 16) {
		mon_fds[p].fd = read[i];
		mon_fds[p].events = POLLIN;
		i = i + 1;
		p = p + 1;
	}
	
	i = 0; //reset iterator
	
	//integrate the given file handles for writing outbound data
	while(i < writingCount && p < 16) {
		mon_fds[p].fd = write[i];
		mon_fds[p].events = POLLOUT;
		i = i + 1;
		p = p + 1;
	}
	
	//call the linux polling function
	int returnVal = poll(mon_fds, p, milliTimeout);
	struct fh_result_set returnResult;
	returnResult.count = p;
	
	i = 0; //reset iterator
	
	if (returnVal < 0) {
		//there was an error with the poll function.
		//what to do?
	} else if (returnVal != 0) {
		//compile the new list of file descriptors that are either ready for reading or writing
		while(i < p) {
			short retevents = mon_fds[i].revents;
			returnResult.fhs[i].fh = mon_fds[i].fd;
			if (retevents & POLLIN || retevents & POLLOUT) {
				//the file handle is either readable or writable
				returnResult.fhs[i].res = 1;
			} else if (retevents & POLLERR || retevents & POLLHUP) {
				//this file handle (in the context of being a UNIX pipe(), has reached the end-of-stream signal and can be closed)
				returnResult.fhs[i].res = -1;
			} else {
				//nothing to report for this fh
				returnResult.fhs[i].res = 0;
			}
			
			i = i + 1;
		}
	} else {
		//no error, no new file descriptors
		returnResult.count = 0;
	}
	
	return returnResult;
}

#pragma mark Buffer Struct
typedef struct tt_databuff {
	void *buffer;
	int buffSize;
	int i;
} tt_databuff;
//initialize a tt_databuff pointer
static void tt_databuff_init(tt_databuff *buff, int size) {
	buff->buffer = malloc(size);
	buff->buffSize = size;
	buff->i = 0;
}
//how many bytes of unused space is in this buffer? 
static int tt_databuff_remaining_bytes(tt_databuff *inputBuff) {
	return inputBuff->buffSize - inputBuff->i;
}
//resize a data buffer a different size without eliminating the data that the buffer was storing
static void tt_databuff_resize(tt_databuff *inputBuff, int newSize) {
	//capture the existing data buffer pointer
	void* existingData = inputBuff->buffer;
	
	//replace the old allocation with the new allocation in the buffer structure
	inputBuff->buffSize = newSize;
	inputBuff->buffer = memcpy(malloc(newSize), existingData, inputBuff->i); //create a new allocation and copy the contents of the existing allocation into the new allocation
	
	//free the old allocation from memory
	free(existingData);
}
//assigning data to a buffer
static void tt_databuff_append_data(tt_databuff *inputBuff, const void* newData, int newDataCount) {
	if (newDataCount > 0) {
		//do we have enough bytes in the data buffer to absorb this data or do we need to allocate a larger heap? if we do not have enough size...
		while (tt_databuff_remaining_bytes(inputBuff) < newDataCount) {
			//resize the buffer before burning
			tt_databuff_resize(inputBuff, inputBuff->buffSize*2);
		}
		//burn the new data into the buffer
		void* buffHead = inputBuff->buffer + inputBuff->i;
		memcpy(buffHead, newData, newDataCount);
		
		//adjust the head
		inputBuff->i = inputBuff->i + newDataCount;
	}
}
//inserts a given data set at the beginning of a data buffer. similar to appending data, except this function adds the data to the beginning of the buffer.
static void tt_databuff_prepend_data(tt_databuff *inputBuff, const void* newData, int newDataCount) {
	if (newDataCount > 0) {
		void* captureAllocation;
		if (inputBuff->i > 0) {
			//copy the current contents of the buffer into a temporary allocation
			captureAllocation = memcpy(malloc(inputBuff->buffSize), inputBuff->buffer, inputBuff->i);
		}
		//do we have enough bytes in the data buffer to absorb this data or do we need to allocate a larger heap? if we do not have enough size...
		while (tt_databuff_remaining_bytes(inputBuff) <= newDataCount) {
			tt_databuff_resize(inputBuff, inputBuff->buffSize*2);
		}
		
		//insert the new data at the head of the primary allocation
		memcpy(inputBuff->buffer, newData, newDataCount);
		
		if (inputBuff->i > 0) {
			//insert the existing data that was found in the buffer when this function was called
			void* buffHead = inputBuff->buffer + newDataCount;
			memcpy(buffHead, captureAllocation, inputBuff->i);
			//free the temporary allocation
			free(captureAllocation);
		}
		//adjust the i variable of the data_buff to represent the new data that is now contained in the buffer
		inputBuff->i = inputBuff->i + newDataCount;
	}
}
//extracts the first x number of bytes from a buffer. the buffer of data that is returned from this function must be freed by the caller
//if x is larger than the amount of data that can be possibly cut from the buffer, x is adjusted to the returned byte buffer's size.
static void* tt_databuff_cut_data(tt_databuff *inputBuff, int *x) {
	//*x cannot be larger than the buffer's allocation size
	if (*x > inputBuff->buffSize) {
		*x = inputBuff->buffSize;
	}
	//*x cannot be larger than the amount of data the buffer is storing
	if (*x > inputBuff->i) {
		*x = inputBuff->i;
	}
	//if there is no data to cut, we can return now.
	if (*x == 0) {
		return NULL;
	}
	
	//capture the data to be returned
	void* returnHeap = memcpy(malloc(*x), inputBuff->buffer, *x); //the caller of this function is responsible for freeing this heap allocation after it has been called
	int remainingBytes = inputBuff->i - *x;
	
	//if we didn't cut out all of the data from the data buffer...adjust the buffer to account for the data we just cut out
	if (remainingBytes > 0) {
		void* buffHead = inputBuff->buffer + *x; //move the buffer head past the data that has been cut out
		
		//memcpy doesn't support overlapping address spaces in a single call. therefore, the remaining data in the buffer structure is copied to a temporary allocation and then copied back to the buffer structure, written in the buffer's allocation head.
		
		void* captureAllocation = memcpy(malloc(remainingBytes), buffHead, remainingBytes); //duplicate the remaining buffered data to a temporary allocation
		
		memcpy(inputBuff->buffer, captureAllocation, remainingBytes); //re-write the captured data at the head of the data buffer structure.
		
		free(captureAllocation); //free the temporary allocation
	} else {
		remainingBytes = 0;
	}
	
	//adjust the head of the buffer to account for the remaining bytes.
	inputBuff->i = remainingBytes;
	
	return returnHeap;
}
//deinitializer
static void tt_databuff_free(tt_databuff *inputBuff) {
	free(inputBuff->buffer);
	inputBuff->i = 0;
	inputBuff->buffSize = 0;
}
//searches a buffer for cr or lf bytes. returns true if found
static bool hasNewLine(void* buff, int count) {
	int i = 0;
	uint8_t *castedBuff = (uint8_t*)buff;
	while (i < count) {
		if (castedBuff[i] == 10 || castedBuff[i] == 13) {
			return true;
		}
		i = i + 1;
	}
	return false;
}
#pragma mark File Handle I/O
//reads a file handle directly into a pre-allocated tt_databuff. before calling the primary `read` function, this function will check to ensure there is enough free space in the data buffer to capture the specified number of bytes.
//`intoBuffer` will be resized if it does not have enough vacant space to contain the specified number of bytes.
//this function returns `true` when the specified file handle can no longer be read from
static bool readHandle(int fhToRead, tt_databuff *intoBuffer, int maxCaptureSize) {
	//ensure that this buffer has at least enough capacity to handle a complete read of maxCaptureSize size without overflowing
	while (tt_databuff_remaining_bytes(intoBuffer) < maxCaptureSize) {
		tt_databuff_resize(intoBuffer, intoBuffer->buffSize*2);
	}
	
	//read the data from the file handle to *intoBuffer
	void* capturePoint = intoBuffer->buffer + intoBuffer->i;
	int capturedSize = read(fhToRead, capturePoint, maxCaptureSize);
	
	//based on the outcome of the read command we make the final adjustments before returning.
	if (capturedSize > 0) {
		//data was captured from the data buffers.
		//move the iterator variable forward by the amount of data that was captured
		intoBuffer->i = intoBuffer->i + capturedSize;	
		return false;	//returning false because this is not the end of the data stream
	} else if (capturedSize < 0) {
		//there was an error trying to read from the file handle (perhaps because it would have blocked waiting for data). no need to adjust iterator
		return false;	//returning false because this is not the end of the data stream
	} else if (capturedSize == 0) {
		//end of stream has been reached
		//no need to adjust iterator or make any further changes to the buffer structure
		return true;
	}
	return false;
}

//writes data to a file handle by cutting a specified number of bytes off a tt_databuff.
//this function is responsible for REMOVING the written data from the tt_databuff after it has been committed to the file handle
//this function returns `true` when the specified file handle can no longer be written to
//during the writing process, `*wroteLine` will be assigned appropriately if a new line was written to the handle or not
static bool writeHandle(int fhToWrite, tt_databuff *storeBuffer, int amountToCut, bool *wroteLine) {
	if (storeBuffer->i < 0) { //if there is actually data contained in this data structure
	
		//cut the data from the data buffer. however much data comes from this cut is what we will attempt to commit to the file handle with the `write` command
		//cutLength is adjusted to account for the amount of data that was successfully pulled from the databuff
		//`databuffCut` needs to be freed by this function if `cutLength` is greater than 0.
		
		int cutLength = amountToCut;
		void* databuffCut = tt_databuff_cut_data(storeBuffer, &cutLength);
		
		if (cutLength > 0) { //if data was actually cut from the structure
			
			//write the data that we cut from the data buffer
			ssize_t writeResult = write(fhToWrite, databuffCut, amountToCut);
			
			if (writeResult > 0) { //was more than 0 bytes written to the file handle?
			
				//determine how much data was written to the file handle compared to the amount of data that was cut from the tt_databuff
				int missingCount = cutLength - writeResult;
				if (missingCount > 0) { //was the amount of data written to the handle the same amount of data that was cut from the tt_databuff initially? if the written amount of data does not match the cut amount of data...
					
					//prepend the missing data back into the beginning of the data buffer so that it may be written again in the next loop iteration
					const void* missingHead = databuffCut + writeResult;
					tt_databuff_prepend_data(storeBuffer, missingHead, missingCount);
				}
				
				//scan the written data for a cr or lf byte
				*wroteLine = hasNewLine(databuffCut, writeResult);
				return false;
				
			} else {
				int errorCapture;
				
				//do not capture errno if there was no explicit error returned from the write command. when it returns 0, we should not expect errno to have a meaningful value
				if (writeResult < 0) {
					errorCapture = errno;
				} else {
					errorCapture = -1;
				}
				
				*wroteLine = false;
				
				//there was no data written to the file handle
				//all of the cut data needs to be added back to the tt_databuff before returning
				tt_databuff_prepend_data(storeBuffer, databuffCut, cutLength);
				
				//determine the error
				switch(errorCapture) {
					case -1: //this should only be true if write is called and it returns exactly 0. this is explicitly defined in the switch to catch the only circumstance where errorCapture should be completely ignored.
						return false;
					case EBADF: //honestly this should never execute under normal circumstances...but just in case....idk
						return true;
					case EPIPE: //indicates that the reading end of the pipes have been closed
						return true;
					default:
						return false;
				}
			}
			
			//free wont take a constant pointer without a warning, so the correct pointer type is created on the stack here before calling the free function
			free(databuffCut); //the cut data buffer needs to be freed if there is 
		} else {
			*wroteLine = false;
		}
		
		return false;
	} else {
		*wroteLine = false;
	}
	
	return false;
}

//special file handle writing function designed for broadcasting messages to the core application from the container process 
static bool notify_parent(int fh, const char* event) {
	char writeBuff[64];
	int writeBuffLen = snprintf(writeBuff, 64, "%s:%i\n", event, getpid());
	if (writeBuffLen > 0 && writeBuffLen < 64) {
		//if the string was successfully built
		ssize_t fhWritten = write(fh, (void*)writeBuff, writeBuffLen);
		if (fhWritten == writeBuffLen) {
			return true; //event written completely and successfully so return true
		} else {
			return false; //event not written completely so return false
		}
		return true;
	} else {
		//the string could not be built. return false to indicate that the event could not be written
		return false;
	}
}
static bool notify_parent_worker(int fh, const char* event, int containerProcess) {
	char writeBuff[64];
	int writeBuffLen = snprintf(writeBuff, 64, "%s:%i:%i\n", event, getpid(), containerProcess);
	if (writeBuffLen > 0 && writeBuffLen < 64) {
		//if the string was successfully built
		ssize_t fhWritten = write(fh, (void*)writeBuff, writeBuffLen);
		if (fhWritten == writeBuffLen) {
			return true; //event written completely and successfully so return true
		} else {
			return false; //event not written completely so return false
		}
		return true;
	} else {
		//the string could not be built. return false to indicate that the event could not be written
		return false;
	}
}

#pragma mark Pipes
//a pipe is considered a null pipe if both the writing fh and the reading fh have negative values
static bool is_null_pipe(tt_pipe input) {
    if (input.reading == -1 && input.writing == -1) {
        return true;
    }
    return false;
}

//creates a new pipe with a writing end that feeds to the reading end
//will return a null pipe if there is a problem updating 
static tt_pipe new_pipe() {
    tt_pipe newPipe;
    int fd[2];
    if (pipe(fd) != 0) { 
		//return a "null pipe" on error
        newPipe.reading = -1;
        newPipe.writing = -1;
    } else {
        newPipe.reading = fd[0];
        newPipe.writing = fd[1];
    }
    return newPipe;
}

//closes all open file descriptors that are not standard inputs or outputs
static void close_handles() {
	struct dirent *cur_dir;
	DIR *dir = opendir("/proc/self/fd/");
	while ((cur_dir = readdir(dir)) != NULL) {
		const char *dname = (const char*)cur_dir->d_name;
		int curFD = atoi(dname);
		switch(curFD) {
			case STDIN_FILENO:
				break;
			case STDOUT_FILENO:
				break;
			case STDERR_FILENO:
				break;
			default:
				close(curFD);
				break;
		}
	}
	closedir(dir);
}

#pragma mark Forking Processes (2)
//1. this is the process that executes the external process
static int tt_worker_proc(void *arg) {
    tt_spawn_config spawnConfig = tt_spawn_config_copy((tt_spawn_config*)arg);
	
	close_handles(); //close all non-standard file handles
	
	//apply environment variables. this must be applied in the `worker process` because it appears from documentation and discussions that setenv() and unsetenv() might leak memory. i have not verified these indicators for myself, but i dont feel like wasting my time trying. this mysterious memory mis-management is not a fatal flaw, however, it means that these `setenv()` and `unsetenv()` functions cannot be called in the same virtual memory environemt as the `core application`, which is how the `container process` is configured (explicitly sharing memory space with parent). setenv() and unsetenv() can safely be called in the worker process, since it has an isolated memory space that dies with the working process.
	int i = 0;
	while (i < spawnConfig.env_count) {
		const char *this_key = (const char*)spawnConfig.env_keys[i];
		const char *this_value = (const char*)spawnConfig.env_values[i];
		setenv(this_key, this_value, 1);
		i = i + 1;
	}
    return execv(spawnConfig.path, spawnConfig.argv); //execute
}
//2. this is the container process
static int tt_container_proc(void *arg) {
	//copy the configuration into an allocation created by *this* new process
    tt_spawn_config spawnConfig = tt_spawn_config_copy((tt_spawn_config*)arg);

	close(spawnConfig.parent.reading); //this file handle is closed because the container process *writes* to this pipe while the core application *reads* from it
	//unlatch from the core application
	close(spawnConfig.latch.reading);
	while (write(spawnConfig.latch.writing, "\n", 1) <= 0) {
		usleep(100000); //sleep for 1/10 seconds
	}
	close(spawnConfig.latch.writing);
	
	//change the current directory of this process
    if (chdir(spawnConfig.wd) != 0) {
        printf("there was an error trying to change the working directory");
    }
	//clear environment variables from the container process.
    int clearEnvResult = clearenv();
    if (clearenv() != 0) {
    	printf("there was an error trying to clear the environment variables that this container inherited from the parent");
    }
	
    //bind the inputs and outputs based on the given configuration structure
	//`interface` file handles direct I/O from the worker process to the container process
    int interface_stdin = -1; //container will *write* to this fh
    int interface_stderr = -1; //container will *read* from this fh
    int interface_stdout = -1; //container will *read* from this fh
	//`external` file handles direct I/O from the container process to the core application
    int external_stdin = -1; //container will *read* from this fh
    int external_stderr = -1; //container will *write* to this fh
    int external_stdout = -1; //container will *write* to this fh
	//these are the data buffers that store the data as it is captured from the worker process and passed to the primary application
	tt_databuff inputBuff;
	inputBuff.i = 0;
	tt_databuff outputBuff;
	outputBuff.i = 0;
	tt_databuff errorBuff;
	errorBuff.i = 0;
	//boolean variables that represent whether or not a data channel is open and still available for **data capture**
// 	bool input_reading_open = false;
// 	bool output_reading_open = false;
// 	bool error_reading_open = false;
	//boolean variables that represent whether or not a data channel is open and still available for **data transmission**
// 	bool input_writing_open = false;
// 	bool output_writing_open = false;
// 	bool error_writing_open = false;

	//phase three (writing to the parent on the control channel) is executed when this boolean is enabled
	bool parent_writable = false;
	//event flags for the third phase
	bool wrote_error_line = false;
	bool wrote_output_line = false;
    //stdout configuration. read the output from the workload process -> write it to the core application
    if (is_null_pipe(spawnConfig.stdout) == false) {
        tt_pipe interface_pipe = new_pipe();
        dup2(interface_pipe.writing, STDOUT_FILENO);
        close(interface_pipe.writing); //this fh is closed because the workload process *writes* to this pipe while the container as the container *reads* from it
        close(spawnConfig.stdout.reading); //this fh is closed because the core application is *reading* this as the container *writes* to it
        interface_stdout = interface_pipe.reading; //*read* from this
        external_stdout = spawnConfig.stdout.writing; //*write* to this
        
		tt_databuff_init(&outputBuff, PIPE_BUF);
		
		//these file descriptors will operate in nonblocking mode 
		fcntl(external_stdout, F_SETFD, O_NONBLOCK);
		fcntl(interface_stdout, F_SETFD, O_NONBLOCK);
    } else {
        int nullDev = open("/dev/null", O_WRONLY);
        dup2(nullDev, STDOUT_FILENO);
        close(nullDev);
    }
    
    //stderr configuration. read the error stream from the workload process -> write it to the core application
    if (is_null_pipe(spawnConfig.stderr) == false) {
        tt_pipe interface_pipe = new_pipe();
        dup2(interface_pipe.writing, STDERR_FILENO);
        close(interface_pipe.writing); //this fh is closed because the workload process *writes* to this pipe as the containter process *reads* from it 
        close(spawnConfig.stderr.reading); //this fh is closed because the core application *reads* from this pipe while the container process *writes* to it
        interface_stderr = interface_pipe.reading; //*read* from this
        external_stderr = spawnConfig.stderr.writing; //*write* to this
        
		tt_databuff_init(&errorBuff, PIPE_BUF);
		
		//these file descriptors will operate in nonblocking mode 
		fcntl(external_stderr, F_SETFD, O_NONBLOCK);
		fcntl(interface_stderr, F_SETFD, O_NONBLOCK);
    } else {
       int nullDev = open("/dev/null", O_WRONLY);
       dup2(nullDev, STDERR_FILENO);
       close(nullDev);
    }

    //stdin configuration. read the input stream from the core application -> write it to the workload
	if (is_null_pipe(spawnConfig.stdin) == false) {
	    tt_pipe interface_pipe = new_pipe();
	    dup2(interface_pipe.reading, STDIN_FILENO);
	    close(interface_pipe.reading); //this fh is closed because the workload *reads* from this pipe while the the container process *writes* to it
		close(spawnConfig.stdin.writing); //this fh is closed because the core application *writes* to this pipe while the container process *reads* from it
		interface_stdin = interface_pipe.writing; //*write* to this
		external_stdin = spawnConfig.stdin.reading; //*read* from this
		
		tt_databuff_init(&inputBuff, PIPE_BUF);
		
		//these file descriptors will operate in nonblocking mode 
		fcntl(external_stdin, F_SETFD, O_NONBLOCK);
		fcntl(interface_stdin, F_SETFD, O_NONBLOCK);
	} else {
		//an interface pipe is still created and bound to the worker process so that we can easily detect when the child process is closed
		tt_pipe interface_pipe = new_pipe();
	    dup2(interface_pipe.reading, STDIN_FILENO);
	    close(interface_pipe.reading); //this fh is closed because the workload *reads* from this pipe while the the container process *writes* to it
		interface_stdin = interface_pipe.writing;
		
		fcntl(interface_stdin, F_SETFD, O_NONBLOCK);
	}
	
    //allocate a stack for the new process
    void *stack;
    void *stackTop;
    struct rlimit stackLimit;
    if (getrlimit(RLIMIT_STACK, &stackLimit) != 0) {
        printf("c: There was an error trying to get the soft resource size for the stack");
    }
    stack = mmap(NULL, stackLimit.rlim_cur, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    stackTop = stack + stackLimit.rlim_cur;
    if (stack == NULL) {
        printf("c: There was an error trying to allocate stack space");
    }

	//launch the worker
	int wpid = clone(tt_worker_proc, stackTop, 0, &spawnConfig);
	
	//interpret the results of the worker process launch
	switch(wpid) {
		case -1:
			//there was an error trying to launch the worker process
			
			//what should I do about this? most likely, should notify the parent through the ctrl channel and then exit.
			break;
			
		default:
			//detach from the worker processes standard inputs and outputs
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			
			//notify the core application that the worker process has launched
			notify_parent_worker(spawnConfig.parent.writing, "l", wpid);
			
			//here begins the main polling loop
			while(interface_stdin != -1 || interface_stdout != -1 || interface_stderr != -1 || external_stdin != -1 || external_stdout != -1 || external_stderr != -1) {
				
				//## phase zero: build the list of file descriptors (reading and writing) that should be polled
				//build a list of the file descriptors to read and write to/from
				int rfds[8];
				int rfdCount = 0;
				int wfds[8];
				int wfdCount = 0;
				//configure stdout channels for polling if the fh's aren't null handles
				if (interface_stdout != -1) {
					rfds[rfdCount] = interface_stdout;
					rfdCount = rfdCount + 1;
				}
				if (external_stdout != -1) {
					wfds[wfdCount] = external_stdout;
					wfdCount = wfdCount + 1;
				}
				//configure stderr channels for polling if the fh's aren't null handles
				if (interface_stderr != -1) {
					rfds[rfdCount] = interface_stderr;
					rfdCount = rfdCount + 1;
				}
				if (external_stderr != -1) {
					wfds[wfdCount] = external_stderr;
					wfdCount = wfdCount + 1;
				}
				//configure stdin channels for polling if the fh's aren't null handles
				if (external_stdin != -1) {
					rfds[rfdCount] = external_stdin;
					rfdCount = rfdCount + 1;
				}
				//`interface_stdin` is always bound to the worker process, regardless of the users configuration
				wfds[wfdCount] = interface_stdin;
				wfdCount = wfdCount + 1;
				//include the parent control channel in the polling writable array.
				wfds[wfdCount] = spawnConfig.parent.writing;
				wfdCount = wfdCount + 1;
				//## end phase zero
				
				//## first phase: reading
				//timeout for the reading phase should be longer than the timeout for the writing phase
				int i = 0;
				if (rfdCount != 0) { //if we have non-null descriptors to read, poll them
					fh_result_set pollResult = poll_fds(rfds, rfdCount, wfds, 0, 400);
					
					while(i < pollResult.count) { //iterate through the file descriptors that have been flagged by the polling function
						fh_result curResult = pollResult.fhs[i];
						
						switch(curResult.res) {
							case 1: //file handle is available to read.
								if (curResult.fh == interface_stderr) {
									//capture the data. if end of file was reached...
									if (readHandle(interface_stderr, &errorBuff, PIPE_BUF) == true) {										
										//close the `interace_stderr` file descriptor, since it is no longer needed
										close(interface_stderr);
										interface_stderr = -1;
									}
								} else if (curResult.fh == interface_stdout) {
									//capture the data. if end of file was reached...
									if (readHandle(interface_stdout, &outputBuff, PIPE_BUF) == true) {
										//close the `interface_stdout` file descriptor, since it is no longer needed
										close(interface_stdout);
										interface_stdout = -1;
									}
								} else if (curResult.fh == external_stdin) {
									//capture the data. if end of file was reached...
									if (readHandle(external_stdin, &inputBuff, PIPE_BUF) == true) {
										close(external_stdin);
										external_stdin = -1;
									}
								}
								break;
							case -1: //for any of the file descriptors that are marked as end-of-stream, flip the boolean flag to exclude these file descriptors from future iterations of the main loop
								if (curResult.fh == interface_stderr) {
									close(interface_stderr);
									interface_stderr = -1;
								} else if (curResult.fh == interface_stdout) {
									close(interface_stdout);
									interface_stdout = -1;
								} else if (curResult.fh == external_stdin) { 
									close(external_stdin);
									external_stdin = -1;
								}
							default: //default do nothing, this file handle is not available to read from
								break;
						}
						
						i = i +1; //rinse and repeat for all readable file descriptors.
					}
					
					i = 0; //reset iterator for next phase
				}
				//## end phase one.
				
				//## second phase: writing
				//timeout for this phase should be minimal
				if (wfdCount != 0) {
					fh_result_set pollResult = poll_fds(rfds, 0, wfds, wfdCount, 100);
					while (i < pollResult.count) {
						fh_result curResult = pollResult.fhs[i];
						bool didWriteLine = false;
						switch(curResult.res) {
							case -1: //file handle has reached end of stream.
								if (curResult.fh == external_stderr) {
									close(external_stderr);
									external_stderr = -1;
								} else if (curResult.fh == external_stdout) {
									close(external_stdout);
									external_stdout = -1;
								} else if (curResult.fh == interface_stdin) {
									close(interface_stdin);
									interface_stdin = -1;
								} else if (curResult.fh ==  spawnConfig.parent.writing) {
									parent_writable = false; //disable phase three
								}
								break;
							case 1: //file handle is available to write.
								if (curResult.fh == external_stderr) {
									//write buffered data to the file handle
									if (writeHandle(external_stderr, &errorBuff, PIPE_BUF, &didWriteLine) == true) {
										//if the reading end of this `external` pipe has already been closed, close this writing end of it
										close(external_stderr);
										external_stderr = -1;
									} else if (interface_stderr == -1 && errorBuff.i == 0) {
										//if the interface end of this channel has already closed and the buffer for the channel contains 0 bytes, close this `external` end of the channel.
										close(external_stderr);
										external_stderr = -1;
									}
									//enable the phase three event flag if we wrote a line
									if (didWriteLine == true) {
										wrote_error_line = true;
									}
								} else if (curResult.fh == external_stdout) {
									//write buffered data to the file handle
									if (writeHandle(external_stdout, &outputBuff, PIPE_BUF, &didWriteLine) == true) {
										//if the reading end of this `external` pipe has already been closed, close this writing end of it
										close(external_stdout);
										external_stdout = -1;
									} else if (interface_stdout == -1 && outputBuff.i == 0) {
										//if the interface end of this channel has already closed and the buffer for the channel contains 0 bytes, close this `external` end of the channel.
										close(external_stdout);
										external_stdout = -1;
									}
									//enable the phase three event flag if we wrote a line
									if (didWriteLine == true) {
										wrote_output_line = true;
									}
								} else if (curResult.fh == interface_stdin) {
									//write buffered data to the file handle
									if (writeHandle(interface_stdin, &inputBuff, PIPE_BUF, &didWriteLine) == true) {
										//if the reading end of this `interface` pipe has already been closed, close this writing end of it. when this is triggered, it is an indication that the worker application has exited
										close(interface_stdin);
										interface_stdin = -1;
									}
									//do not close this `interface_stdin` file handle simply because the `external` part of this channel might not be configured or active
									//line notifications are not applied to data channels that are directed towards the worker process. this is one of those channels
								} else if (curResult.fh ==  spawnConfig.parent.writing) {
									parent_writable = true; //enable phase three
								}
								break;
							default:
								//if the control channel is explicitly marked as unwritable...
								if (curResult.fh == spawnConfig.parent.writing) {
									parent_writable = false; //...disable phase three
								}
								break;
						}
						
						i = i +1;
					}
					i = 0;
				}
				//## end phase two
				
				//phase three: write event flags to the control channel
				if (parent_writable == true) {
					if (wrote_error_line == true) {
						bool didNotify = notify_parent(spawnConfig.parent.writing, "ne");
						if (didNotify == true) {
							wrote_error_line = false;
						}
					}
					
					if (wrote_output_line == true) {
						bool didNotify = notify_parent(spawnConfig.parent.writing, "no");
						if (didNotify == true) {
							wrote_output_line = false;
						}
					}
				}
				//## end phase three
			} // end main loop
			
			i = 0;
			while (notify_parent(spawnConfig.parent.writing, "x") == false) {
				i = i + 1;
			}
			break;
	}
    return 0;
}


/*
tt_spawn_core is the primary high-level function that encompasses all functionality in this project. this is the function that will be called by SwiftSlash directly.

*Arguments*
- path: the executable to launch
- wd: specifies the path that the process shall use as its working directory at launch
- input: the pipe that tt_spawn will *read* from to direct as input to the workload process
- output: the pipe that tt_spawn will *write* to as it reads the output stream of the workload process
- error: the pipe that tt_spawn will *write* to as it reads the error stream of the workload process
- notify: the file handle that tt_spawn shall use to *write* to the global process monitor in the core application
- latch: temporary internal io pipe that synchronizes the core application with the container process to facilitate a safe data exchange
*/
pid_t tt_spawn_core(const char *path, const char **argv, const int argv_count, const char **env_keys, const char **env_values, int env_count, const char *wd, tt_pipe input, tt_pipe output, tt_pipe error, tt_pipe notify) {
    pid_t lpid;
    //allocate a stack for the container process
    void *stack;
    void *stackTop;
    struct rlimit stackLimit;
    int result = getrlimit(RLIMIT_STACK, &stackLimit);
    if (result != 0) {
        printf("[tt_spawn][ERROR] :: There was an error trying to get the soft resource  size for the stack allocation that was bound to be used for the tt_spawn container process");
    }
    stack = mmap(NULL, stackLimit.rlim_cur, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    stackTop = stack + stackLimit.rlim_cur; //stacks populate their memory space in reverse order, so we need to pass the end of the buffer instead of the start of the buffer.
    if (stack == NULL) {
        printf("[tt_spawn][ERROR] :: There was an error trying to allocate stack space");
    }

    //build a configuration structure based on the arguments passed to this function
    tt_spawn_config launchConfig = tt_spawn_config_init(path, argv, argv_count, wd, env_keys, env_values, env_count, input, output, error, notify, new_pipe());

	//launch the container
	lpid = clone(tt_container_proc, stackTop, CLONE_VM | SIGCHLD, &launchConfig);
    
	//interpret result
    switch(lpid) {
        case -1:
            printf("[tt_spawn][ERROR] :: There was an error calling clone\n");
            break;

        default: //in the core application, successfully launched container
			close(launchConfig.latch.writing);
            if (is_null_pipe(input) == false) {
                close(input.reading); //core application *writes* to this pipe, so it will close the reading end
            }
            if (is_null_pipe(output) == false) {
                close(output.writing); //core application *reads* from this pipe, so it will close the writing end
            }
            if (is_null_pipe(error) == false) {
                close(error.writing); //core application *reads* from this pipe, so it will close the writing end
            }
			
			//wait for the container process to unlatch
			void* tempbuff = malloc(16);
			while (read(launchConfig.latch.reading, tempbuff, 1) <= 0) {
				usleep(100000); //sleep for 1/10 seconds
			}
			free(tempbuff);
			close(launchConfig.latch.reading);
			
			//free the initial configuration now that the child has unlatched with its own copy of the data
			tt_spawn_config_free(&launchConfig);
    }
    
    return lpid;
}
