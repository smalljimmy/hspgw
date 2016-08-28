#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PMS_PASSWORD_MAX_LEN 255
static int convertToGuestlist (char guestnames[][PMS_PASSWORD_MAX_LEN], char *guestname, char *delim) {
	
	int i = 0;
	char *ptr = guestname;
	char *ptr2 = NULL;
	while (ptr2 = strstr(ptr, delim)) {
		strncpy(guestnames[i], ptr, ptr2 - ptr);
		guestnames[i++][ptr2 - ptr] = '\0';
		ptr = ptr2 + strlen(delim);
	}
	if (strlen(ptr) > 0) {
		strcpy(guestnames[i++], ptr);
	}
	strcpy (guestnames [i], "");
	
	return i;
}

void testConvert(char* guestname, char* delim) {
	char guestnames[10][PMS_PASSWORD_MAX_LEN];

	printf ("In:'%s'\n", guestname);
	int n = convertToGuestlist (guestnames, guestname, delim);	
	int i;
	
	for (i=0; i<n; i++) {
		printf ("=>'%s'\n", guestnames[i]);				
	}	

}
	
void main() {
	testConvert ("müller", "; ");
	testConvert ("müller; meyer; huber; a; b; c; ", "; ");
	testConvert ("müller;meyer;huber; a; b; c; ", "; ");
	testConvert ("müller;meyer;huber; a; b; c", "; ");
}	