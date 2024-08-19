/*
*  Copyright (c) 2024 Michael Marley
*/

/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#define _GNU_SOURCE /* for asprintf */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

#define DEV_INPUT_EVENT "/dev/input"
#define EVENT_DEV_NAME "event"

#define BACKLIGHT_DEVICE "/sys/class/leds/chromeos::kbd_backlight/brightness"
#define DEFAULT_BACKLIGHT_ON_SECONDS 10
#define DEFAULT_BACKLIGHT_BRIGHTNESS 30
#define DEFAULT_FADE_DURATION 100000

// This must be set exactly to the number of elements in the array or else a segfault will occur
#define SEARCH_DEVICE_COUNT 2
const char *search_devices[SEARCH_DEVICE_COUNT]={
	"AT Translated Set 2 keyboard",
	"PIXA3854:00 093A:0274 Touchpad",
};
int found_device_count=0;

pthread_t input_thread;

pthread_cond_t timer_cond;
pthread_mutex_t timer_mutex;

int configured_backlight_on_seconds=DEFAULT_BACKLIGHT_ON_SECONDS;
int configured_backlight_brightness=DEFAULT_BACKLIGHT_BRIGHTNESS;
int configured_fade_duration=DEFAULT_FADE_DURATION;

int desired_backlight_brightness=0;
struct timespec backlight_off_time;

FILE *backlight_brightness_file;

static int is_event_device(const struct dirent *dir){
	return strncmp(EVENT_DEV_NAME,dir->d_name,5)==0;
}

static bool string_in_array(const char *string,const char *array[],int array_length){
	for(int i=0;i<array_length;i++){
		if(!strcmp(string,array[i])){
			return true;
		}
	}
	return false;
}

static int get_input_fds(struct pollfd *fds){
	struct dirent **namelist={};
	int device_count=scandir(DEV_INPUT_EVENT,&namelist,is_event_device,alphasort);
	int clock_type=CLOCK_MONOTONIC;
	if(device_count<=0){
		fprintf(stderr,"%s directory contains no devices!\n",DEV_INPUT_EVENT);
		return EXIT_FAILURE;
	}
	
	for(int i=0;i<device_count&&found_device_count<=SEARCH_DEVICE_COUNT;i++){
		char device_filename[4096];
		int fd=-1;
		char device_name[256]="???";
		
		snprintf(device_filename,sizeof(device_filename),"%s/%s",DEV_INPUT_EVENT,namelist[i]->d_name);
		fd=open(device_filename,O_RDONLY);
		if(fd<0){
			continue;
		}
		ioctl(fd,EVIOCGNAME(sizeof(device_name)),device_name);
		
		if(string_in_array(device_name,search_devices,SEARCH_DEVICE_COUNT)){
			ioctl(fd,EVIOCSCLOCKID,&clock_type);
			printf("Using device %s:\t%s\n",device_filename,device_name);
			fds[found_device_count].fd=fd;
			fds[found_device_count].events=POLLIN;
			found_device_count++;
		}else{
			close(fd);
		}
		
		free(namelist[i]);
	}
	
	if(found_device_count==0){
		fprintf(stderr,"No matching input devices found!\n");
		return EXIT_FAILURE;
	}
	
	return EXIT_SUCCESS;
}

static int timespec_cmp(const struct timespec ts1,const struct timespec ts2){
	if(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec==ts2.tv_nsec){
		return 0;
	}else if((ts1.tv_sec>ts2.tv_sec)||(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec>ts2.tv_nsec)){
		return 1;
	}else{
		return -1;
	}
}

static void set_backlight_brightness(int brightness){
	fprintf(backlight_brightness_file,"%d",brightness);
	fflush(backlight_brightness_file);
}

void *input_handler(void *arg){
	struct pollfd fds[SEARCH_DEVICE_COUNT];
	if(get_input_fds(fds)){
		exit(EXIT_FAILURE);
	}
	
	int i;
	int read_bytes;
	struct input_event input_event[256];
	struct timespec event_time={};
	struct timespec latest_event_time={};
	while(true){
		if(poll(fds,found_device_count,-1)==-1){
			fprintf(stderr,"Poll failure‽\n");
			exit(EXIT_FAILURE);
		}
		
		for(i=0;i<found_device_count;i++){
			if(fds[i].revents&POLLIN){
				read_bytes=read(fds[i].fd,input_event,sizeof(input_event));
				if(read_bytes==-1){
					fprintf(stderr,"Read failure‽\n");
					exit(EXIT_FAILURE);
				}
				
				// Get the index of the last event, which will be the latest event for this device
				int last_event_index=read_bytes/sizeof(struct input_event)-1;
				
				// Convert the event time into a timespec and update latest_event_time if it is more recent
				event_time.tv_sec=input_event[last_event_index].input_event_sec;
				event_time.tv_nsec=input_event[last_event_index].input_event_usec*1000;
				if(timespec_cmp(event_time,latest_event_time)>=0){
					memcpy(&latest_event_time,&event_time,sizeof(struct timespec));
				}
			}
		}
		
		pthread_mutex_lock(&timer_mutex);
		// Set backlight_off_time to latest_event_time plus configured_backlight_on_seconds
		backlight_off_time.tv_sec=latest_event_time.tv_sec+configured_backlight_on_seconds;
		backlight_off_time.tv_nsec=latest_event_time.tv_nsec;
		
		// If the backlight is currently off, signal the main thread to turn it on
		if(desired_backlight_brightness==0){
			printf("Turning backlight on\n");
			desired_backlight_brightness=configured_backlight_brightness;
			pthread_cond_signal(&timer_cond);
		}
		pthread_mutex_unlock(&timer_mutex);
		
		// Sleep here to prevent spinning and using too much CPU
		usleep(500000);
	}
}

static int string_to_int(int *result,const int min,const int max,const char *string){
	errno=0;
	const long long_value=strtol(string,NULL,10);
	if(errno!=0){
		return errno;
	}
	if(long_value>=min&&long_value<=max){
		*result=(int)long_value;
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

static int usage(){
	printf("Usage: keylightc [--brightness <brightness>] [--fadeduration <fadeduration>] [--timeout <timeout>]\n\n");
	printf("keylightc - automatic keyboard backlight daemon for Framework laptops\n\n");
	printf("Options:\n");
	printf("  --brightness\t\tbrightness level when active (1-100) [default=%d]\n",DEFAULT_BACKLIGHT_BRIGHTNESS);
	printf("  --fadeduration\tfade time in microseconds (1-1000000) [default=%d]\n",DEFAULT_FADE_DURATION);
	printf("  --timeout\t\tactivity timeout in seconds (1-%d) [default=%d]\n",INT_MAX,DEFAULT_BACKLIGHT_ON_SECONDS);
	printf("  --help\t\tdisplay usage information\n");
	return EXIT_FAILURE;
}

static const struct option long_options[]={
	{"brightness",required_argument,0,'b'},
	{"fadeduration",required_argument,0,'f'},
	{"timeout",required_argument,0,'t'},
	{"help",no_argument,0,'h'},
	{},
};

int main(const int argc,char **argv){
	int option;
	while((option=getopt_long(argc,argv,"",long_options,NULL))!=EOF){
		switch(option){
			case -1:
			case 0:
				break;
			case 'b':
				if(string_to_int(&configured_backlight_brightness,1,100,optarg)){
					return usage();
				}
				break;
			case 'f':
				if(string_to_int(&configured_fade_duration,1,1000000,optarg)){
					return usage();
				}
				break;
			case 't':
				if(string_to_int(&configured_backlight_on_seconds,1,INT_MAX,optarg)){
					return usage();
				}
				break;
			case 'h':
			default:
				return usage();
		}
	}
	
	if(getuid()!=0){
		fprintf(stderr,"Must be run as root!\n");
		return EXIT_FAILURE;
	}
	
	backlight_brightness_file=fopen(BACKLIGHT_DEVICE,"w");
	if(backlight_brightness_file==NULL){
		fprintf(stderr,"Failed to open backlight device!  Are you running Linux kernel 6.11 or later?\n");
		return EXIT_FAILURE;
	}
	set_backlight_brightness(0);
	
	pthread_condattr_t timer_condattr;
	pthread_condattr_init(&timer_condattr);
	pthread_condattr_setclock(&timer_condattr,CLOCK_MONOTONIC);
	pthread_cond_init(&timer_cond,&timer_condattr);
	pthread_mutex_init(&timer_mutex,NULL);
	
	pthread_mutex_lock(&timer_mutex);
	
	// Create the input thread only after locking timer_mutex to ensure it doesn't send us any events before we are ready
	pthread_create(&input_thread,NULL,input_handler,NULL);
	
	int current_backlight_brightness=0;
	int previous_desired_backlight_brightness=-1;
	int fade_interval=0;
	struct timespec current_time={};
	
	while(true){
		clock_gettime(CLOCK_MONOTONIC,&current_time);
		if(desired_backlight_brightness==configured_backlight_brightness&&timespec_cmp(current_time,backlight_off_time)>=0){
			// If current_time is greater than or equal to backlight_off_time, turn the backlight off
			printf("Turning backlight off\n");
			desired_backlight_brightness=0;
		}else if(desired_backlight_brightness!=configured_backlight_brightness){
			// If the backlight is already off, wait to be signaled to turn it back on
			pthread_cond_wait(&timer_cond,&timer_mutex);
		}else{
			// If current_time is less than backlight_off_time, wait until backlight_off_time
			pthread_cond_timedwait(&timer_cond,&timer_mutex,&backlight_off_time);
		}
		
		while(current_backlight_brightness!=desired_backlight_brightness){
			// Allow the input thread to make desired_backlight_brightness changes while in the dimmer loop
			// As long as no change to desired_backlight_brightness can be made outside either the pthread_cond_wait or the dimmer loop, we are safe from races
			pthread_mutex_unlock(&timer_mutex);
			
			// If the desired_backlight_brightness has changed since the last iteration, calculate a new fade_interval
			if(previous_desired_backlight_brightness!=desired_backlight_brightness){
				previous_desired_backlight_brightness=desired_backlight_brightness;
				fade_interval=configured_fade_duration/abs(current_backlight_brightness-desired_backlight_brightness);
			}
			
			if(current_backlight_brightness>desired_backlight_brightness){
				current_backlight_brightness--;
			}else{
				current_backlight_brightness++;
			}
			set_backlight_brightness(current_backlight_brightness);
			usleep(fade_interval);
			
			pthread_mutex_lock(&timer_mutex);
		}
	}
	
	return EXIT_SUCCESS;
}
