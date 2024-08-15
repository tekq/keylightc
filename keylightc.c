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
#define NUM_FDS 2

#define BACKLIGHT_DEVICE "/sys/class/leds/chromeos::kbd_backlight/brightness"
#define DEFAULT_BACKLIGHT_ON_SECONDS 10
#define DEFAULT_BACKLIGHT_BRIGHTNESS 30
#define DEFAULT_FADE_DURATION 100000

#define KEYBOARD_DEVICE_NAME "AT Translated Set 2 keyboard"
#define TOUCHPAD_DEVICE_NAME "PIXA3854:00 093A:0274 Touchpad"

pthread_t timer_thread;
pthread_cond_t timer_cond;
pthread_mutex_t timer_mutex;
pthread_mutex_t backlight_off_time_mutex;

int configured_backlight_on_seconds=DEFAULT_BACKLIGHT_ON_SECONDS;
int configured_backlight_brightness=DEFAULT_BACKLIGHT_BRIGHTNESS;
int configured_fade_duration=DEFAULT_FADE_DURATION;

int desired_backlight_brightness=0;
struct timespec backlight_off_time;

FILE* backlight_brightness_file;

static int is_event_device(const struct dirent *dir){
	return strncmp(EVENT_DEV_NAME,dir->d_name,5)==0;
}

static int get_input_fds(struct pollfd *fds){
	struct dirent **namelist;
	int i,ndev;
	
	ndev=scandir(DEV_INPUT_EVENT,&namelist,is_event_device,alphasort);
	if(ndev<=0){
		return EXIT_FAILURE;
	}
	
	for(i=0;i<ndev;i++){
		char fname[4096];
		int fd=-1;
		char name[256]="???";
		
		snprintf(fname,sizeof(fname),"%s/%s",DEV_INPUT_EVENT,namelist[i]->d_name);
		fd=open(fname,O_RDONLY);
		if(fd<0){
			continue;
		}
		
		int flags=fcntl(fd,F_GETFL,0);
		fcntl(fd,F_SETFL,flags|O_NONBLOCK);
		
		ioctl(fd,EVIOCGNAME(sizeof(name)),name);
		
		if(!strcmp(KEYBOARD_DEVICE_NAME,name)){
			printf("Found keyboard device: %s:	%s\n",fname,name);
			fds[1].fd=fd;
			fds[1].events=POLLIN;
		}else if(!strcmp(TOUCHPAD_DEVICE_NAME,name)){
			printf("Found touchpad device: %s:	%s\n",fname,name);
			fds[0].fd=fd;
			fds[0].events=POLLIN;
		}else{
			close(fd);
		}
		free(namelist[i]);
	}
	return EXIT_SUCCESS;
}

static int timespec_cmp(struct timespec ts1,struct timespec ts2){
	if(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec==ts2.tv_nsec){
		return 0;
	}else if((ts1.tv_sec>ts2.tv_sec)||(ts1.tv_sec==ts2.tv_sec&&ts1.tv_nsec>ts2.tv_nsec)){
		return 1;
	}else{
		return -1;
	}
}

void *timer(){
	int current_backlight_brightness=0;
	int previous_desired_backlight_brightness=-1;
	int fade_interval=0;
	struct timespec current_time;
	struct timespec local_backlight_off_time;
	
	pthread_mutex_lock(&timer_mutex);
	while(true){
		// Lock and copy to ensure main thread doesn't touch backlight_off_time while we read it
		pthread_mutex_lock(&backlight_off_time_mutex);
		memcpy(&local_backlight_off_time,&backlight_off_time,sizeof(struct timespec));
		pthread_mutex_unlock(&backlight_off_time_mutex);
		
		clock_gettime(CLOCK_MONOTONIC,&current_time);
		if(desired_backlight_brightness==configured_backlight_brightness&&timespec_cmp(current_time,local_backlight_off_time)>=0){
			// If current_time is greater than or equal to backlight_off_time, turn the backlight off
			printf("Turning backlight off\n");
			desired_backlight_brightness=0;
		}else if(desired_backlight_brightness!=configured_backlight_brightness){
			// If the backlight is already off, wait to be signaled to turn it back on
			pthread_cond_wait(&timer_cond,&timer_mutex);
		}else{
			// If current_time is less than backlight_off_time, wait until backlight_off_time
			pthread_cond_timedwait(&timer_cond,&timer_mutex,&local_backlight_off_time);
		}
		
		// Allow the main thread to make desired_backlight_brightness changes while in the dimmer loop
		// As long as no change to desired_backlight_brightness can be made outside either the pthread_cond_wait or the dimmer loop, we are safe from races
		pthread_mutex_unlock(&timer_mutex);
		while(current_backlight_brightness!=desired_backlight_brightness){
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
			fprintf(backlight_brightness_file,"%d",current_backlight_brightness);
			fflush(backlight_brightness_file);
			usleep(fade_interval);
			
			// Only re-lock once the fade is done to save CPU cycles
			if(current_backlight_brightness==desired_backlight_brightness){
				pthread_mutex_lock(&timer_mutex);
			}
		}
	}
}

static int string_to_int(int *result, int min, int max, char *string){
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
	printf("  --fadeduration\tfade time in microseconds (1-%d) [default=%d]\n",INT_MAX,DEFAULT_FADE_DURATION);
	printf("  --timeout\t\tactivity timeout in seconds (1-%d) [default=%d]\n",INT_MAX,DEFAULT_BACKLIGHT_ON_SECONDS);
	printf("  --help\t\tdisplay usage information\n");
	return EXIT_FAILURE;
}

static const struct option long_options[]={
	{"brightness",required_argument,0,'b'},
	{"fadeduration",required_argument,0,'f'},
	{"timeout",required_argument,0,'t'},
	{"help",no_argument,0,'h'},
	{NULL,0,0,'\0'},
};

int main(int argc,char **argv){
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
				if(string_to_int(&configured_fade_duration,1,INT_MAX,optarg)){
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
		fprintf(stderr,"Failed to open backlight device!\n");
		return EXIT_FAILURE;
	}
	
	struct input_event input_event[64];
	struct pollfd fds[NUM_FDS];
	get_input_fds(fds);
	
	pthread_condattr_t timer_condattr;
	pthread_condattr_init(&timer_condattr);
	pthread_condattr_setclock(&timer_condattr,CLOCK_MONOTONIC);
	pthread_cond_init(&timer_cond,&timer_condattr);
	pthread_mutex_init(&timer_mutex,NULL);
	pthread_mutex_init(&backlight_off_time_mutex,NULL);
	
	pthread_create(&timer_thread,NULL,timer,NULL);
	
	int i;
	while(true){
		if(poll(fds,NUM_FDS,-1)==-1){
			fprintf(stderr,"Poll failure‽\n");
			return EXIT_FAILURE;
		}
		
		for(i=0;i<NUM_FDS;i++){
			if(fds[i].revents&POLLIN){
				(void)!read(fds[i].fd,input_event,sizeof(input_event));
				
				pthread_mutex_lock(&backlight_off_time_mutex);
				clock_gettime(CLOCK_MONOTONIC,&backlight_off_time);
				backlight_off_time.tv_sec+=configured_backlight_on_seconds;
				pthread_mutex_unlock(&backlight_off_time_mutex);
				
				pthread_mutex_lock(&timer_mutex);
				if(desired_backlight_brightness==0){
					printf("Turning backlight on\n");
					desired_backlight_brightness=configured_backlight_brightness;
					pthread_cond_signal(&timer_cond);
				}
				pthread_mutex_unlock(&timer_mutex);
				
				// Extra read helps to reduce events delayed by the sleep below
				(void)!read(fds[i].fd,input_event,sizeof(input_event));
			}
		}
		
		// Sleep here to prevent spinning and using too much CPU
		usleep(1000000);
	}
	
	return EXIT_SUCCESS;
}
