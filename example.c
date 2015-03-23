#define IP	"10.42.0.1"
#define PORT 14141

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

int main(int argc, char **argv) {
	static struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 100 * 1000000};
	int dev0 = -1, dev1 = -1;
	struct sockaddr_in remote;
	struct hc_sr04_udp_packet {
		uint8_t id;
		uint64_t distance;
	} packet0, packet1;
	int udp_socket;
	uint16_t port;
	
	if (argc == 2) {
		if (strcmp(argv[1], "0")) {
			dev0 = open("/dev/hc-sr04-0", O_RDONLY);
			if (dev0 == -1) {
				perror("open");
				return -1;
			}
		} else if (strcmp(argv[1], "1")) {
			dev1 = open("/dev/hc-sr04-1", O_RDONLY);
			if (dev1 == -1) {
				perror("open");
				return -1;
			}
		} else {
			fprintf(stderr, "Uso: %s [id_do_sensor]\n", argv[0]);
		}
	} else {
		dev0 = open("/dev/hc-sr04-0", O_RDONLY);
		if (dev0 == -1) {
			perror("open");
			return -1;
		}
		dev1 = open("/dev/hc-sr04-1", O_RDONLY);
		if (dev1 == -1) {
			perror("open");
			return -1;
		}
	}
	
	if ((udp_socket = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
		perror("socket");
		return -1;
	}
	
	memset((char *) &remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;	//IPv4
	if (!inet_pton(AF_INET, IP, &remote.sin_addr.s_addr)) {
		perror("inet_pton");
		return -1;
	}
	remote.sin_port = htons(PORT);
	
	for (;;) {
		int err;
		if (dev0 != -1) {
			if (read(dev0, &packet0.distance, sizeof(packet0.distance)) < 0) {
				perror("read");
			} else {
				if (packet0.distance < 10 * 1000 * 58)
					packet0.distance = 0;
				else
					packet0.distance -= 10 * 1000 * 58;
				if (packet0.distance > 600 * 1000 * 58)
					packet0.distance = 600 * 1000 * 58;
				printf("0 - %llu\n", packet0.distance / 1000 / 58 - 8);
				if (sendto(udp_socket, &packet0, sizeof(packet0), 0, (struct sockaddr *) &remote, sizeof(struct sockaddr_in)) == -1)
					perror("sendto");
			}
			nanosleep(&sleep_time, NULL);
		}
		if (dev1 != -1) {
			if (read(dev1, &packet1.distance, sizeof(packet1.distance)) < 0) {
				perror("read");
			} else {
				if (packet1.distance < 10 * 1000 * 58)
					packet1.distance = 0;
				else
					packet1.distance -= 10 * 1000 * 58;
				if (packet1.distance > 600 * 1000 * 58)
					packet1.distance = 600 * 1000 * 58;
				printf("1 - %llu\n", packet1.distance / 1000 / 58 - 8);
				if (sendto(udp_socket, &packet1, sizeof(packet1), 0, (struct sockaddr *) &remote, sizeof(struct sockaddr_in)) == -1)
					perror("sendto");
			}
			nanosleep(&sleep_time, NULL);
		}
	}
	
	return 0;
}
