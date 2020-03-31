// From a sketchup 
// https://www.hackster.io/matrix-labs/direction-of-arrival-for-matrix-voice-creator-using-odas-b7a15b

#include <json.h>
#include <math.h>
#include <matrix_hal/everloop.h>
#include <matrix_hal/everloop_image.h>
#include <matrix_hal/matrixio_bus.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <array>
#include <iostream>
#include <errno.h>
#include <fcntl.h> /* Added for the nonblocking socket */
#include <unistd.h> /*usleep */

namespace hal = matrix_hal;

/* -------------------------------------------------------------- */
/* ---------- GENERAL CONFIGURATION, LEDs, CONNECTION  ---------- */
/* -------------------------------------------------------------- */
// ENERGY_COUNT : Number of sound energy slots to maintain.
#define ENERGY_COUNT 36
// MAX_VALUE : controls smoothness
#define MAX_VALUE 200
// INCREMENT : controls sensitivity
#define INCREMENT 20
// DECREMENT : controls delay in the dimming
#define DECREMENT 1
// MIN_THRESHOLD: Filters out low energy
#define MIN_THRESHOLD 10
// MAX_BRIGHTNESS: 0 - 255
#define MAX_BRIGHTNESS 50
// SLEEP IN SEC, 0.1s -> 100 ms, wating time before checking if a socket connection is available (accept return > 0)
#define SLEEP_ACCEPT_LOOP 0.5
// How many empty messges should be received before raising a timeout
#define MAX_EMPTY_MESSAGE 200

// This must be the same parameters as in defined in configuration ssl.nPots
#define MAX_ODAS_SOURCES 4

// Activate for debug different components
#define DEBUG_CONNECTION 0
#define DEBUG_DOA 0
#define DEBUG_JSON 0
#define DEBUG_INCOME_MSG 0
#define DEBUG_SSL 0
#define DEBUG_SST 0


/* -------------------------------------------------- */
/* ---------- UTILITIES FOR DEBUG PRINTING ---------- */
/* -------------------------------------------------- */
// https://stackoverflow.com/questions/8487986/file-macro-shows-full-path
#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
// For Windows use '\\' instead of '/'.

// https://stackoverflow.com/questions/1644868/define-macro-for-debug-printing-in-c
#define debug_print(DEBUG, fmt, ...) \
        do { if (DEBUG) fprintf(stdout, "%s:%d:%s(): " fmt, __FILENAME__, \
                                __LINE__, __func__, __VA_ARGS__); fflush(stdout);} while (0)


/* ------------------------------------------------------- */
/* ---------- CONNECTION CONSTANT AND STRUCTURE ---------- */
/* ------------------------------------------------------- */
enum ODAS_data_source{SSL = 0, SST = 1}; // Lookup table to determine the index of the data source
#define NUM_OF_ODAS_DATA_SOURCES 2 // Length of ODAS_data_source

const unsigned int nBytes = 10240;
int backlog = 1; 

char const  *ODAS_data_source_str[NUM_OF_ODAS_DATA_SOURCES] = {"SSL", "SST"}; 
int servers_id[NUM_OF_ODAS_DATA_SOURCES] = {0, 0}; 
unsigned int port_numbers[NUM_OF_ODAS_DATA_SOURCES] = {9001, 9000}; // SSL, SST
struct sockaddr_in servers_address[NUM_OF_ODAS_DATA_SOURCES];
int connections_id[NUM_OF_ODAS_DATA_SOURCES] = {0, 0}; 
char *messages[NUM_OF_ODAS_DATA_SOURCES] = {NULL, NULL};
int messages_size[NUM_OF_ODAS_DATA_SOURCES] = {0, 0}; 


/* --------------------------------------- */
/* ---------- HW LED  STRUCTURE ---------- */
/* --------------------------------------- */
struct hal_leds_struct {
   hal::MatrixIOBus bus;
   hal::EverloopImage image1d;
   hal::Everloop everloop;
}; // hw layer for LEDs control

struct led_energies_struct {
	int energy_array_azimuth[ENERGY_COUNT]; // fi
	int energy_array_elevation[ENERGY_COUNT]; //theta
};

/* ----------------------------------------- */
/* ---------- ODAS DATA STRUCTURE ---------- */
/* ----------------------------------------- */
/* example SSL
{
    "timeStamp": 41888,
    "src": [
        { "x": 0.000, "y": 0.824, "z": 0.566, "E": 0.321 },
        { "x": -0.161, "y": 0.959, "z": 0.232, "E": 0.121 },
        { "x": -0.942, "y": -0.263, "z": 0.211, "E": 0.130 },
        { "x": 0.266, "y": 0.507, "z": 0.820, "E": 0.081 }
    ]
}*/
struct SSL_src_struct {
	double x;
	double y;
	double z;
	double E;
}; // SSL src 
struct SSL_struct {
	SSL_src_struct src[MAX_ODAS_SOURCES]; // TODO, Max value or variable?
	unsigned int timestamp;
}; // SSL struct message

/* example SST
{
    "timeStamp": 41887,
    "src": [
        { "id": 100, "tag": "dynamic", "x": -0.014, "y": 0.901, "z": 0.434, "activity": 0.954 },
        { "id": 112, "tag": "dynamic", "x": -0.966, "y": -0.161, "z": 0.204, "activity": 0.000 },
        { "id": 0, "tag": "", "x": 0.000, "y": 0.000, "z": 0.000, "activity": 0.000 },
        { "id": 0, "tag": "", "x": 0.000, "y": 0.000, "z": 0.000, "activity": 0.000 }
    ]
} */
struct SST_src_struct {
	unsigned int id;
	char tag[20]; // TODO, VERIFY THIS IS NOT BUGGY, NO IDEA WHAT IS THE MAX LEN!!!
	double x;
	double y;
	double z;
	double activity;
}; // SSL src 
struct SST_struct {
	SST_src_struct src[MAX_ODAS_SOURCES]; // TODO, Max value or variable?
	unsigned int timestamp;
}; // SSL struct message


/* --------------------------------------------- */
/* ---------- INTERNAL DATA STRUCTURE ---------- */
/* --------------------------------------------- */
static SSL_struct SSL_data;
static SST_struct SST_data;
static led_energies_struct led_energies;
static unsigned int json_array_id = 0; // a counter to inspect json_array and set value in the proper src structure , not elegant but functional
static unsigned int json_msg_id = SSL; // Default value
static int counter_no_data = MAX_EMPTY_MESSAGE; //counter for empty messages, used for timeout

char* message2str(int odas_id, char msg[]) {
	msg[0] = '\0';
	if (odas_id==SSL) {
		sprintf(msg + strlen(msg), "SSL Message\ntimestamp: %d\n", SSL_data.timestamp);
		double x,y,z,E;
		for (int c = 0; c < MAX_ODAS_SOURCES; c++) {
			x = SSL_data.src[c].x;
			y = SSL_data.src[c].y;
			z = SSL_data.src[c].z;
			E = SSL_data.src[c].E;
			sprintf(msg + strlen(msg), "\tsrc[%d]\tx=%f\ty=%f\tz=%f\tE=%f\n",c,x,y,z,E);
		}
	} else if (odas_id==SST) {
		sprintf(msg + strlen(msg), "SST Message\ntimestamp: %d\n", SST_data.timestamp);
		double x,y,z,activity;
		char* tag;
		unsigned id;
		for (int c = 0; c < MAX_ODAS_SOURCES; c++) {
			// NO IDEA, IF I USE DIRECTLY THE DATA IN THE STRUCTURE I GET ALWAYS 0!
			id = SST_data.src[c].id;
			x = SST_data.src[c].x;
			y = SST_data.src[c].y;
			z = SST_data.src[c].z;
			activity = SST_data.src[c].activity;
			tag = SST_data.src[c].tag;
			sprintf(msg + strlen(msg), "\tsrc[%d]\tid=%d\ttag=%s\tx=%f\ty=%f\tz=%f\tactivity=%f\n",
			                                  c,   id,    tag,    x,    y,    z,    activity);
	    }
	}
	return msg;
}

const double leds_angle_mcreator[35] = {
    170, 159, 149, 139, 129, 118, 108, 98,  87,  77,  67,  57,
    46,  36,  26,  15,  5,   355, 345, 334, 324, 314, 303, 293,
    283, 273, 262, 252, 242, 231, 221, 211, 201, 190, 180};

const double led_angles_mvoice[18] = {170, 150, 130, 110, 90,  70,
                                      50,  30,  10,  350, 330, 310,
                                      290, 270, 250, 230, 210, 190};

void increase_pots() {
  // https://en.wikipedia.org/wiki/Spherical_coordinate_system#Coordinate_system_conversions
  // Convert x,y to angle. TODO: See why x axis from ODAS is inverted... ????
  double x, y, z, E; 
  x = SSL_data.src[json_array_id].x;
  y = SSL_data.src[json_array_id].y;
  z = SSL_data.src[json_array_id].z;
  E = SSL_data.src[json_array_id].E;
  
 // debug_print(DEBUG_DOA, "[ts: %d] Computing for Object@(0x%X) SSL_DATA[%d] (x=%f,y=%f,z=%f,E=%f)", SSL_data.timestamp, &SSL_data, json_array_id, x,y,z,E);
  // TODO: timestamp for some reason break the object and the 
  debug_print(DEBUG_DOA, "[ts: %d] Object (0x%X)SSL_DATA.src[%d] (x=%f,y=%f,z=%f,E=%f)",SSL_data.timestamp, &SSL_data,json_array_id, x,y,z,E);
 
  double angle_fi = fmodf((atan2(y, x) * (180.0 / M_PI)) + 360, 360);
  double angle_theta = 90.0 - fmodf((atan2(sqrt(y*y+x*x), z) * (180.0 / M_PI)) + 180, 180);
  // Convert angle to index
  int i_angle_fi = angle_fi / 360 * ENERGY_COUNT;  // convert degrees to index
  int i_angle_proj_theta = angle_theta / 180 * ENERGY_COUNT;  // convert degrees to index
  
  // Set energy for this angle, azimuth fi
  led_energies.energy_array_azimuth[i_angle_fi] += INCREMENT * E * cos(angle_theta * M_PI / 180.0 );
  led_energies.energy_array_elevation[i_angle_fi] += INCREMENT * E * sin(angle_theta * M_PI / 180.0);
  
  // Set energy for this angle, angle_theta theta
  debug_print(DEBUG_DOA, "angle_fi=%f energy_array_azimuth=%d--- i_angle_proj_theta=%f --- energy_array_elevation=%d\n", angle_fi, led_energies.energy_array_azimuth[i_angle_fi], angle_theta, led_energies.energy_array_elevation[i_angle_proj_theta] );
    
  // Set limit at MAX_VALUE
  led_energies.energy_array_azimuth[i_angle_fi] =
      led_energies.energy_array_azimuth[i_angle_fi] > MAX_VALUE ? MAX_VALUE : led_energies.energy_array_azimuth[i_angle_fi];
  led_energies.energy_array_elevation[i_angle_proj_theta] =
      led_energies.energy_array_elevation[i_angle_proj_theta] > MAX_VALUE ? MAX_VALUE : led_energies.energy_array_elevation[i_angle_proj_theta];
}

void decrease_pots() {
  for (int i = 0; i < ENERGY_COUNT; i++) {
    led_energies.energy_array_azimuth[i] -= (led_energies.energy_array_azimuth[i] > 0) ? DECREMENT : 0;
	led_energies.energy_array_elevation[i] -= (led_energies.energy_array_elevation[i] > 0) ? DECREMENT : 0;
  }
}

void json_parse_array(json_object *jobj, char *key) {
  // Forward Declaration
  void json_parse(json_object * jobj);
  enum json_type type;
  json_object *jarray = jobj;
  if (key) {
    if (json_object_object_get_ex(jobj, key, &jarray) == false) {
      fprintf(stderr, "Error parsing json object\n");
      return;
    }
  }

  int arraylen = json_object_array_length(jarray);
  int i;
  json_object *jvalue;

  for (i = 0; i < arraylen; i++) {
    jvalue = json_object_array_get_idx(jarray, i);
    type = json_object_get_type(jvalue);

    if (type == json_type_array) {
      json_parse_array(jvalue, NULL);
    } else if (type != json_type_object) {
    } else {
	  if (json_array_id>=MAX_ODAS_SOURCES) {
		  fprintf(stderr,"ODAS array too big, discarding json object %d\n",json_array_id);
	  } else {
		  // TODO: should be moved out all this stuff in a specific call??? These are calls specifc for LEDs handling
		  if (json_msg_id ==SSL) {
			  decrease_pots();
		  }
		  debug_print(DEBUG_JSON, "Processing JSON array obj item: %d ", json_array_id);
		  json_parse(jvalue);
		  increase_pots();
	  }
	  json_array_id++;
    }
  }
}

void json_parse(json_object *jobj) {
  enum json_type type;
  unsigned int count = 0;
  
  /* if (json_msg_id ==SSL) {
			  decrease_pots();
		  } */
  
  json_object_object_foreach(jobj, key, val) {
    type = json_object_get_type(val);
    switch (type) {
      case json_type_boolean:
        break;
      case json_type_double:
	    if (json_msg_id ==SSL) {
			if (!strcmp(key, "x")) {
			  SSL_data.src[json_array_id].x = json_object_get_double(val);
			  if(DEBUG_JSON) {printf("(0x%X)SSL_data.src[%d].x=%f - ", &SSL_data, json_array_id, SSL_data.src[json_array_id].x);}
			} else if (!strcmp(key, "y")) {
			  SSL_data.src[json_array_id].y = json_object_get_double(val);
			  if(DEBUG_JSON) {printf("(0x%X)SSL_data.src[%d].y=%f - ", &SSL_data, json_array_id, SSL_data.src[json_array_id].y);}
			} else if (!strcmp(key, "z")) {
			  SSL_data.src[json_array_id].z = json_object_get_double(val);
			  if(DEBUG_JSON) {printf("(0x%X)SSL_data.src[%d].z=%f - ", &SSL_data, json_array_id, SSL_data.src[json_array_id].z);}
			} else if (!strcmp(key, "E")) {
			  SSL_data.src[json_array_id].E = json_object_get_double(val);
			  if(DEBUG_JSON) {printf("(0x%X)SSL_data.src[%d].E=%f\n", &SSL_data, json_array_id, SSL_data.src[json_array_id].E);}
			}
//			increase_pots();
		} else if (json_msg_id ==SST) {
			if (!strcmp(key, "x")) {
			  SST_data.src[json_array_id].x = json_object_get_double(val);
			} else if (!strcmp(key, "y")) {
			  SST_data.src[json_array_id].y = json_object_get_double(val);
			} else if (!strcmp(key, "z")) {
			  SST_data.src[json_array_id].z = json_object_get_double(val);
			} else if (!strcmp(key, "activity")) {
			  SST_data.src[json_array_id].activity = json_object_get_double(val);
			} 
		}
        
        count++;
        break;
      case json_type_int:
		if (json_msg_id ==SSL) {
			if (!strcmp(key, "timeStamp")) {
				SSL_data.timestamp = (unsigned int)json_object_get_int(val);
				if(DEBUG_JSON) {printf("----------------------------(0x%X)SSL_data.timestamp=%d - val is \n", &SSL_data, SSL_data.timestamp);}
			} 
		} else if (json_msg_id ==SST) {
			if (!strcmp(key, "timeStamp")) {
				SST_data.timestamp = (unsigned int)json_object_get_int(val);
			} else if (!strcmp(key, "id")) {
				SST_data.src[json_array_id].id = (unsigned int)json_object_get_int(val);
			}
		}
        break;
      case json_type_string:
	    if (json_msg_id ==SSL) {
			
		} else if (json_msg_id ==SST) {
			if (!strcmp(key, "tag")) {
				// ASSUMING MAX LENGTH IS 20 BUGGY!!!
				strncpy(SST_data.src[json_array_id].tag, json_object_get_string(val), 20);
				//printf("%s vs %s \n",SST_data.src[json_array_id].tag, json_object_get_string(val));
				SST_data.src[json_array_id].tag[20] = '\0';
			}
		}
        break;
      case json_type_object:
        if (json_object_object_get_ex(jobj, key, &jobj) == false) {
          fprintf(stderr, "Error parsing json object\n");
          return;
        }
        json_parse(jobj);
        break;
      case json_type_array:
	    json_array_id = 0;
        json_parse_array(jobj, key);
        break;
    }
  }
}

int init_connection(sockaddr_in &server_address, int port_number, int backlog) {
	/*Init a non blocking connection and return the socket ID*/
	int server_id = 0;
	server_id = socket(AF_INET, SOCK_STREAM, 0);
	server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port_number);
	bind(server_id, (struct sockaddr *)&server_address, sizeof(server_address));  
	
	// https://www.cs.tau.ac.il/~eddiea/samples/Non-Blocking/tcp-nonblocking-server.c.html
    /* listen and change the socket into non-blocking state	*/
	listen(server_id, backlog); 
	fcntl(server_id, F_SETFL, O_NONBLOCK);
	
	return server_id;
}

int accept_connection(int server_id) {
	int connection_id = accept(server_id, (struct sockaddr *)NULL, NULL);
	if (connection_id==-1) {
		if (errno==EAGAIN) {
		  debug_print(DEBUG_CONNECTION, "server %d, no data (retry again) - %s",server_id, strerror(errno));
		} else {
		  fprintf(stderr,"accepting connection %d, error errno=%d - %s",server_id, errno, strerror(errno));
		  exit(-1);
		}
	} else {
		debug_print(DEBUG_CONNECTION, " [Connected] id=%d\n", connection_id);
	}
	fflush(stdout);
	return connection_id;
}

bool reception_terminate() {
	/*If any connetion terminated abnormally return true, not receiving data after a while is a sign of data closing*/
	for (int c = 0 ; c < NUM_OF_ODAS_DATA_SOURCES; c++) {	
		debug_print(DEBUG_CONNECTION, " For %s len msg received is %d", ODAS_data_source_str[c], messages_size[c]);
		if (messages_size[c] == -1) {
			debug_print(DEBUG_CONNECTION, "reception terminate  for income data %s" , ODAS_data_source_str[c]);
			return true;
		} else if ((messages_size[c] == 0) ) {
			counter_no_data--;
			if (counter_no_data==0) {
				debug_print(DEBUG_CONNECTION, "timeout for income data %s" , ODAS_data_source_str[c]);
				return true;
			}
		} else {
			counter_no_data = MAX_EMPTY_MESSAGE; // reset the counter
		}
	}
	fflush(stdout);
	return false;
}

void set_led_sst(hal_leds_struct &hw_led, char * message_sst) {
	// at start up there can be some message in queue, this can bring to the second message to be bad formatted
	// This creates a segfault
	json_object *jobj = json_tokener_parse(message_sst);
	if (message_sst[0]!='{') {
		fprintf(stderr, "set_led_sst: Ignoring message, wrong opening character	  ->%c<-}", message_sst[0]);
		return;
	}
	json_msg_id = SST; // Define which object should be saved
    json_parse(jobj);
	
	// Only needed if debugging
	if (DEBUG_SST) {
		char msg[1024];
		debug_print(DEBUG_SST, "GENERATED %s ",message2str(SST, msg));
	}
}

void set_led_ssl(hal_leds_struct &hw_led, char * message_ssl) {
	// at start up there can be some message in queue, this can bring to the second message to be bad formatted
	// This creates a segfault
	json_object *jobj = json_tokener_parse(message_ssl);
	if (message_ssl[0]!='{') {
		fprintf(stderr, "set_led_ssl: Ignoring message, wrong opening character	  ->%c<-}", message_ssl[0]);
		return;
	}
	json_msg_id = SSL; // Define which object should be saved
    json_parse(jobj);
	
	// Only needed if debugging
	if (DEBUG_SSL) {
		char msg[1024];
		debug_print(DEBUG_SSL, "GENERATED %s ",message2str(SSL, msg));
	}

    for (int i = 0; i < hw_led.bus.MatrixLeds(); i++) {
      // led index to angle
      int led_angle = hw_led.bus.MatrixName() == hal::kMatrixCreator
                          ? leds_angle_mcreator[i]
                          : led_angles_mvoice[i];
      // Convert from angle to pots index
      int index_pots = led_angle * ENERGY_COUNT / 360;
      // Mapping from pots values to color
      int color_azimuth = led_energies.energy_array_azimuth[index_pots] * MAX_BRIGHTNESS / MAX_VALUE;
	  int color_elevation = led_energies.energy_array_elevation[index_pots] * MAX_BRIGHTNESS / MAX_VALUE;
	  
      // Removing colors below the threshold
      color_azimuth = (color_azimuth < MIN_THRESHOLD) ? 0 : color_azimuth;
	  color_elevation = (color_elevation < MIN_THRESHOLD) ? 0 : color_elevation;
	  //debug_print(DEBUG_SSL,"led_angle=%d, index_pots=%d, color_azimuth=%d, color_elevation=%d\n", led_angle,index_pots,color_azimuth, color_elevation ); 
	
      hw_led.image1d.leds[i].red = 0;
      hw_led.image1d.leds[i].green = color_elevation;
      hw_led.image1d.leds[i].blue = color_azimuth;
      hw_led.image1d.leds[i].white = 0;
    }
    hw_led.everloop.Write(&hw_led.image1d);
}

int main(int argc, char *argv[]) {
  int c; // a counter for cycles for 
  
  // Everloop Initialization
  hal_leds_struct hw_led;
  if (!hw_led.bus.Init()) return false;
  hw_led.image1d = hal::EverloopImage(hw_led.bus.MatrixLeds());
  hw_led.everloop.Setup(&hw_led.bus);

  // Clear all LEDs
  for (hal::LedValue &led : hw_led.image1d.leds) {
    led.red = 0;
    led.green = 0;
    led.blue = 0;
    led.white = 0;
  }
  hw_led.everloop.Write(&hw_led.image1d);

  
// INIT MESSAGES
  printf("(0x%X)SSL_data and (0x%X)SST_data", &SSL_data, &SST_data);
  printf("Init messages ");
  for (c = 0 ; c < NUM_OF_ODAS_DATA_SOURCES; c++) {	
    messages[c] = (char *)malloc(sizeof(char) * nBytes);
	memset(messages[c], '\0', sizeof(char) * nBytes);
	messages_size[c] = strlen(messages[c]);
	printf(" ...  %s(len=%d,nBytes=%d)", ODAS_data_source_str[c], messages_size[c], nBytes);
  }
  printf("[OK]\n");
  fflush(stdout);

// INIT CONNECTIONS
  printf(" Init listening");
  for (c = 0 ; c < NUM_OF_ODAS_DATA_SOURCES; c++) {
	  if (port_numbers[c]) {
		  printf(" ... %s ", ODAS_data_source_str[c]);
		  servers_id[c] = init_connection(servers_address[c], port_numbers[c], backlog); 
		  printf(" (%d)", servers_id[c]);
	  }
  }
  printf(" [OK]\n");
  fflush(stdout);

// ACCEPT CONNECTIONS
  printf(" Waiting For Connections\n ");
  bool services_connected = false;
  printf("Connecting: ");
  while (!services_connected) {
	for (c = 0 ; c < NUM_OF_ODAS_DATA_SOURCES; c++) {
	  if (port_numbers[c]) {
		printf("[%s", ODAS_data_source_str[c]);
		connections_id[c] = accept_connection(servers_id[c]); 
		printf("%s", connections_id[c] >= 0 ? " CONNECTED]\n" : ".]");
		fflush(stdout);
	  } 
    }
	services_connected = true;
	for (c = 0 ; c < NUM_OF_ODAS_DATA_SOURCES; c++) {	
	  services_connected = services_connected and (port_numbers[c] ? connections_id[c] > 0 : true);
    }
	printf("[services_connected %s\n", services_connected ? "True]" : "False]");
	//services_connected = (portNumber_ssl ? connection_id_ssl > 0 : true) and (portNumber_sst ? connection_id_sst > 0 : true); 
    usleep( (unsigned int)(SLEEP_ACCEPT_LOOP*1000000) ); 
  } 
  printf("Connection [OK]\n");
  fflush(stdout);

// RECEIVING DATA
  printf("Receiving data........... \n");
  unsigned long n_cycles = 1; // Just a counter
  while (!reception_terminate()) { 
	  // Separator to print only when debugging but not with the debug formatting	  
	  if (DEBUG_INCOME_MSG) { printf("---------------------------------\nSTART RECEPTION: %d\n---------------------------------\n", n_cycles);}
	  for (c = 0 ; c < NUM_OF_ODAS_DATA_SOURCES; c++) {	
	    memset(messages[c], '\0', sizeof(char) * nBytes); // Reset before using, fill the message of NULLs
		messages_size[c] = port_numbers[c] ? recv(connections_id[c] , messages[c], nBytes, 0): 0; // Received the message, if available
		if (messages_size[c]) {
			messages[c][messages_size[c]] = 0x00; 
			debug_print(DEBUG_INCOME_MSG, "RECEIVED message %s: len=%d - \n||%s||\n", ODAS_data_source_str[c], messages_size[c], messages[c]);
			if (ODAS_data_source_str[c]=="SSL") {
				set_led_ssl(hw_led, messages[c]);
			} else if (ODAS_data_source_str[c]=="SST") {
				set_led_sst(hw_led, messages[c]);
			} else {
				fprintf(stderr, "Unknown message type %s\n", ODAS_data_source_str[c]);
			}
		} else {
			debug_print(DEBUG_INCOME_MSG, "returned messages_size for %s: len=%d\n", ODAS_data_source_str[c], messages_size[c]);
		}
		if (DEBUG_INCOME_MSG) { printf("END RECEPTION message %s: len=%d\n+-+-+-+-+-+-+-+-+-+-\n", ODAS_data_source_str[c], messages_size[c]); }
		fflush(stdout);
	  }
	  if (DEBUG_INCOME_MSG) { printf("---------------------------------\nEND RECEPTION: %d\n---------------------------------\n\n", n_cycles);}
	  n_cycles++;
   }
   printf("Received Data terminated [OK]\n");
}
