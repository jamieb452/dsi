#include <stdio.h>
#include <malloc.h>
#include <pcap.h>
#include <getopt.h>
#include "bitreg.h"
#include "mmc.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		   "Usage: %s [options...] <trace file>\n"
		   "SDIO WiFi dongle trace parser.\n"
           "\n"
           "Options:\n"
           "  -d, --decode          Decode the SDIO trace file to the console.\n"
		   "                          This is the default action.\n"
           "  -p, --pcap=FILE       Dump WLAN packets from a trace to a pcap file.\n"
           "\n",
		   argv0);
   exit(1);
}

static const char* decode_wmi_key_type(unsigned char type)
{
	switch(type)
	{
		case 1: return "NONE";
		case 2: return "WEP";
		case 3: return "TKIP";
		case 4: return "AES";
		default: return "";
	}
}

static const char* decode_wmi_key_usage(unsigned char type)
{
	switch(type)
	{
		case 0: return "Pairwise";
		case 1: return "Group";
		case 2: return "Tx";
		default: return "";
	}
}

static void do_decode_wmi_add_cipher_key(unsigned char* data, unsigned int len)
{
	unsigned int i;


	fprintf(stdout, " >> WMI_ADD_CIPHER_KEY:\n");
	fprintf(stdout, " >>  Key index: %d\n", data[0]);
	fprintf(stdout, " >>  Key type: %d\n", decode_wmi_key_type(data[1]));
	fprintf(stdout, " >>  Key usage: %d\n", decode_wmi_key_usage(data[2]));
	fprintf(stdout, " >>  Key replay counter: ");
	for(i=0; i<8; i++)
		fprintf(stdout, "%02X", data[4+i]);
	fprintf(stdout, "\n");
	fprintf(stdout, " >>  Key length: %d\n", data[3]);
	fprintf(stdout, " >>  Key data: ");
	for(i=0; i<data[3]; i++)
		fprintf(stdout,"%02X", data[12+i]);
	fprintf(stdout, "\n");
	fprintf(stdout, " >>  Key control: %d\n", data[44]);
}


static void do_decode_wmi_ready_event(unsigned char* data, unsigned int len)
{
	unsigned int i;


	fprintf(stdout, " >> WMI_READY_EVENT:\n");
	fprintf(stdout, " >>  MAC: ");
	for(i=0; i<6; i++)
		fprintf(stdout, "%02X", data[i]);
	fprintf(stdout, "\n");
	fprintf(stdout, " >>  Phy capability: %d\n", data[6]);
}

static void do_decode(mmc_context* ctx)
{
	unsigned char cmd;
	unsigned int content;
	mmc_cmd53 cmd53;
	mmc_cmd52 cmd52;

	while(mmc_process(ctx))
	{
		if (mmc_is_cmd_ready(ctx))
		{
			cmd = mmc_get_cmd(ctx);
			content = mmc_get_cmd_content(ctx);

			fprintf(stdout, "%02d%08X ", cmd, content);

			if (cmd == 52)
			{
				mmc_get_cmd52_content(ctx, &cmd52);

				if (cmd52.rw)
					fprintf(stdout, "IO_RW_DIRECT   WRITE FNR=%d ADDR=%08x RAW=%d DATA=%02X", cmd52.fnr, cmd52.address, cmd52.raw, cmd52.data);
				else
					fprintf(stdout, "IO_RW_DIRECT   READ FNR=%d ADDR=%08x", cmd52.fnr, cmd52.address);
					
			}
			else if (cmd == 53)
			{
				mmc_get_cmd53_content(ctx, &cmd53);

				fprintf(stdout, "IO_RW_EXTENDED %s FNR=%d ADDR=%08x SIZE=%03d OP=%d BM=%d C=%d", cmd53.rw? "WRITE":"READ ", cmd53.fnr, cmd53.address, cmd53.size, cmd53.opcode, cmd53.bmode, cmd53.count);
			}
			fprintf(stdout, "\n");
		}

		if (mmc_is_resp_ready(ctx))
		{
			cmd = mmc_get_cmd(ctx);
			content = mmc_get_cmd_content(ctx);

			fprintf(stdout, " > %02d%08X ", cmd, content);

			if (cmd == 52)
			{
				fprintf(stdout, "FLAGS=%02X DATA=%02X", (content>>8)&0xFF, content&0xFF);
			}
			else if (cmd == 53)
			{
				fprintf(stdout, "FLAGS=%02X", (content>>8)&0xFF);
			}
			fprintf(stdout, "\n");
		}

		if (mmc_is_dat_ready(ctx))
		{
			unsigned int size = mmc_get_dat_size(ctx);
			unsigned char* data = mmc_get_dat_content(ctx);
			unsigned int address = mmc_get_address(ctx);
			unsigned int fnr = mmc_get_fnr(ctx);

			//mmc_dump(" >>", data, size);

			if (fnr == 1 && address >= 0x800 && address < 0x1000 && size > 6 && data[0] == 0x01)
			{
				unsigned short len = data[1] | (data[2]<<8);
				unsigned short id = data[6] | (data[7]<<8);

				switch(id)
				{
					case 22: do_decode_wmi_add_cipher_key(data+8, len-2); break;

					case 4097: do_decode_wmi_ready_event(data+8, len-2); break;
						
					default:
						fprintf(stdout, "Got WMI id %d\n", id);
						mmc_dump(" >>", data, size);
					break;
				}
			}
			else
			{
				mmc_dump(" >>", data, size);
			}
		}
	}
}


static void do_pcap_dump(mmc_context* ctx, const char* fname)
{
	pcap_t* pc = 0;
	pcap_dumper_t* pd = 0;
	struct pcap_pkthdr phdr;

	// Create Ethernet (linktype=1) pcap file, with max packet size of 64kb
	pc = pcap_open_dead(1, 0x10000);
	pd = pcap_dump_open(pc, fname);

	if (pd == 0)
	{
		fprintf(stderr, "could not create pcap file %s\n", fname);
		return;
	}


	while(mmc_process(ctx))
	{
		if (mmc_is_dat_ready(ctx))
		{
			unsigned int address = mmc_get_address(ctx);
			unsigned int fnr = mmc_get_fnr(ctx);
			unsigned int rw = mmc_get_rw(ctx);
			unsigned int size = mmc_get_dat_size(ctx);
			unsigned char* data = mmc_get_dat_content(ctx);

			if (fnr == 1 && address >= 0x800 && address < 0x1000 && size > 6 && data[0] == 0x02)
			{
				unsigned int endpoint = data[0];
				unsigned int len = data[2] | (data[3]<<8);

				if (len > 14)
				{
					// replace with length specified in Ethernet header
					len = 14 + (data[20]<<8) + data[21];
					phdr.caplen = len;
					phdr.len = len;
					phdr.ts.tv_sec = 0;
					phdr.ts.tv_usec = 0;

					pcap_dump((u_char*)pd, &phdr, data+8); 
				}
			}
		}
	}

	pcap_dump_close(pd);
}

int main(int argc, char* argv[])
{
	enum actions
	{
		Decode,
		GeneratePcap,
	};
	char pcapfname[512];
	char tracefname[512];
	mmc_context ctx;
	int action = Decode;
	int c;
	

	while (1) 
	{
		int option_index;
		static struct option long_options[] = 
		{
			{"decode", 0, NULL, 'd'},
			{"pcap", 1, NULL, 'p'},
			{NULL},
		};

		c = getopt_long(argc, argv, "dp:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) 
		{
			case 'd':
				action = Decode;
			break;

			case 'p':
				action = GeneratePcap;
				strncpy(pcapfname, optarg, sizeof(pcapfname));
			break;

			default:
				usage(argv[0]);
		}
	}

	if (optind == argc - 1) 
	{
		// Exactly one extra argument- a trace file
		strncpy(tracefname, argv[optind], sizeof(tracefname));
	} 
	else if ( (optind < argc) || (argc == 1) )
	{
		// Too many extra args
		usage(argv[0]);
	}


	if (0 == mmc_init(&ctx, tracefname))
	{
		fprintf(stderr, "could not open file %s\n", tracefname);

		exit(1);
	}

	switch(action)
	{
		case GeneratePcap:
			do_pcap_dump(&ctx, pcapfname);
		break;

		case Decode:
			do_decode(&ctx);
		break;
	}

	mmc_destroy(&ctx);

	return 0;
}